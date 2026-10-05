#include "engine/nx_search.h"
#include "engine/nx_search_index_internal.h"
#include "core/nx_mem.h"
#include "core/nx_utf8.h"
#include "query/nx_parse.h"
#include "index/nx_regex.h"
#include "index/nx_fuzzy.h"
#include "index/nx_trigram.h"
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <locale.h>

#define SEARCH_ARENA_LIMIT (32u * 1024u * 1024u)
#define SEARCH_TERMS 16u
#define NO_FIELD UINT32_MAX
typedef enum value_kind { V_EXISTS, V_INT, V_FLOAT, V_BOOL, V_EXACT, V_TEXT,
    V_REGEX, V_SUBSTR, V_PREFIX, V_FUZZY, V_VECTOR, V_RANGE, V_ANY } value_kind;
typedef struct value value;
struct value {
    value_kind kind;
    int64_t integer;
    double real;
    nx_slice text;
    size_t *prefix;
    nx_utf8_tokens tokens;
    nx_regex *regex;
    value *cleanup_next;
    value **items;
    uint32_t count, distance;
    bool phrase;
    const nx_value *source;
};
typedef struct plan plan;
struct plan {
    const nx_node *source;
    plan **kids;
    value *v;
    uint32_t field;
    nx_field_type type;
    uint8_t *bits;
};
typedef struct search_context {
    const nx_table *table;
    const nx_search_index *prepared;
    nx_search_options options;
    nx_search_result stats;
    nx_arena arena;
    nx_error *error;
    nx_status status;
    value *values;
    bool lexical, vector;
    uint32_t sort_fields[8];
    bool sort_desc[8];
    uint32_t nsort;
} search_context;
typedef struct ranked_hit {
    nx_search_hit hit;
    double lexical, vector;
    uint32_t lr, vr;
    bool has_vector;
} ranked_hit;

static bool finite_number(double x) { return x >= -DBL_MAX && x <= DBL_MAX; }
static bool eqs(nx_slice s, const char *v) { return nx_slice_eq(s, nx_slice_cstr(v)); }
static nx_slice str_slice(nx_str s) { return nx_slice_make(s.s, s.len); }
static int compare_bytes(nx_slice a, nx_slice b) {
    size_t n = a.n < b.n ? a.n : b.n;
    int c = n ? memcmp(a.p, b.p, n) : 0;
    return c ? c : (a.n > b.n) - (a.n < b.n);
}
static bool bit(const plan *p, uint32_t row) { return (p->bits[row / 8] & (1u << (row % 8))) != 0; }
static void set_bit(plan *p, uint32_t row) { p->bits[row / 8] |= (uint8_t)(1u << (row % 8)); }
static nx_status fail(search_context *c, nx_status st, nx_span span, const char *message) {
    if (c->status == NX_OK) {
        c->status = st;
        nx_error_set(c->error, st, span.off, span.len, "%s", message);
    }
    return st;
}
static bool work(search_context *c, size_t n) {
    if (n > c->options.max_work - c->stats.work) {
        fail(c, NX_ERR_LIMIT, (nx_span){0, 0}, "Search work limit exceeded"); return false;
    }
    c->stats.work += n; return true;
}
static void *alloc_arena(search_context *c, size_t n, size_t align) {
    if (n > SEARCH_ARENA_LIMIT || c->arena.total > SEARCH_ARENA_LIMIT - n) {
        fail(c, NX_ERR_LIMIT, (nx_span){0, 0}, "Search plan memory limit exceeded"); return NULL;
    }
    void *p = nx_arena_zalloc(&c->arena, n ? n : 1, align);
    if (!p) fail(c, NX_ERR_NOMEM, (nx_span){0, 0}, "Cannot allocate search plan");
    return p;
}
static nx_slice raw_text(const nx_value *v) {
    if (v->kind == NX_VAL_WORD || v->kind == NX_VAL_STRING) return str_slice(v->u.text);
    if (v->kind == NX_VAL_NUMBER) return str_slice(v->u.number.raw);
    if (v->kind == NX_VAL_DATETIME) return str_slice(v->u.datetime.raw);
    return nx_slice_make(NULL, 0);
}
static bool compare_result(int cmp, nx_op op) {
    switch (op) {
        case NX_OP_MATCH: case NX_OP_EQ: return cmp == 0;
        case NX_OP_NE: return cmp != 0;
        case NX_OP_LT: return cmp < 0; case NX_OP_LE: return cmp <= 0;
        case NX_OP_GT: return cmp > 0; case NX_OP_GE: return cmp >= 0;
        default: return false;
    }
}
static nx_compare bsi_op(nx_op op) {
    switch (op) {
        case NX_OP_NE: return NX_CMP_NE; case NX_OP_LT: return NX_CMP_LT;
        case NX_OP_LE: return NX_CMP_LE; case NX_OP_GT: return NX_CMP_GT;
        case NX_OP_GE: return NX_CMP_GE; default: return NX_CMP_EQ;
    }
}

static value *bind_value(search_context *c, const nx_value *src, nx_field_type type, nx_op op) {
    value *v = alloc_arena(c, sizeof(*v), _Alignof(value));
    if (!v) return NULL;
    v->source = src; v->cleanup_next = c->values; c->values = v;
    if (src->kind == NX_VAL_STAR) {
        if (op != NX_OP_MATCH) goto unsupported;
        v->kind = V_EXISTS; return v;
    }
    if (src->kind == NX_VAL_ANYOF || src->kind == NX_VAL_RANGE) {
        if (op != NX_OP_MATCH && op != NX_OP_EQ) goto unsupported;
        v->kind = src->kind == NX_VAL_ANYOF ? V_ANY : V_RANGE;
        v->count = src->kind == NX_VAL_ANYOF ? src->u.anyof.n : 2;
        if (v->count > 256) goto limit;
        if (v->kind == V_RANGE && type != NX_FIELD_INT && type != NX_FIELD_FLOAT) goto mismatch;
        v->items = alloc_arena(c, (size_t)v->count * sizeof(value *), _Alignof(value *));
        if (!v->items) return NULL;
        for (uint32_t i = 0; i < v->count; i++) {
            const nx_value *child = v->kind == V_ANY ? src->u.anyof.items[i] :
                (i == 0 ? src->u.range.lo : src->u.range.hi);
            if (child) {
                v->items[i] = bind_value(c, child, type, v->kind == V_ANY ? op : NX_OP_MATCH);
                if (!v->items[i]) return NULL;
            }
        }
        return v;
    }
    if (src->kind == NX_VAL_CALL && eqs(str_slice(src->u.call.name), "exists") && src->u.call.nargs == 0) {
        if (op != NX_OP_MATCH) goto unsupported;
        v->kind = V_EXISTS; return v;
    }
    if (type == NX_FIELD_INT || type == NX_FIELD_FLOAT) {
        if (op == NX_OP_FUZZY) goto mismatch;
        v->kind = type == NX_FIELD_INT ? V_INT : V_FLOAT;
        if (src->kind == NX_VAL_DATETIME && src->u.datetime.v.base_rel == NX_REL_NONE &&
            src->u.datetime.v.prec == NX_DT_PREC_YEAR) {
            v->integer = src->u.datetime.v.year; v->real = (double)v->integer; return v;
        }
        if (src->kind != NX_VAL_NUMBER) goto mismatch;
        if (nx_number_suffix(src).len) goto unsupported;
        if (type == NX_FIELD_INT && !src->u.number.has_int) goto mismatch;
        v->integer = src->u.number.i; v->real = src->u.number.d; return v;
    }
    if (type == NX_FIELD_BOOL) {
        if (op != NX_OP_MATCH && op != NX_OP_EQ && op != NX_OP_NE) goto mismatch;
        nx_slice s = raw_text(src);
        if (eqs(s, "true") || eqs(s, "yes") || eqs(s, "1")) v->integer = 1;
        else if (eqs(s, "false") || eqs(s, "no") || eqs(s, "0")) v->integer = 0;
        else goto mismatch;
        v->kind = V_BOOL; return v;
    }
    if (type == NX_FIELD_VECTOR) {
        if (op != NX_OP_MATCH || src->kind != NX_VAL_VECTOR) goto unsupported;
        v->kind = V_VECTOR; return v;
    }
    if (type != NX_FIELD_TEXT) goto mismatch;
    if (op != NX_OP_MATCH && op != NX_OP_EQ && op != NX_OP_NE && op != NX_OP_FUZZY) goto mismatch;
    if (src->kind == NX_VAL_REGEX) {
        if (op != NX_OP_MATCH) goto unsupported;
        v->kind = V_REGEX;
        nx_status st = nx_regex_compile(str_slice(src->u.regex.pattern), src->u.regex.flags, &v->regex, NULL);
        if (st != NX_OK) { fail(c, st, src->span, "Invalid or unsupported regular expression"); return NULL; }
        return v;
    }
    v->kind = op == NX_OP_EQ || op == NX_OP_NE ? V_EXACT : op == NX_OP_FUZZY ? V_FUZZY : V_TEXT;
    v->phrase = src->kind == NX_VAL_STRING;
    v->text = raw_text(src);
    v->distance = UINT32_MAX;
    if (src->kind == NX_VAL_CALL) {
        if (op != NX_OP_MATCH || src->u.call.nargs < 1) goto unsupported;
        nx_slice name = str_slice(src->u.call.name);
        if (eqs(name, "substr")) v->kind = V_SUBSTR;
        else if (eqs(name, "prefix")) v->kind = V_PREFIX;
        else if (eqs(name, "phrase")) { v->kind = V_TEXT; v->phrase = true; }
        else if (eqs(name, "fuzzy")) v->kind = V_FUZZY;
        else if (eqs(name, "regex") && src->u.call.nargs == 1) {
            const nx_value *arg = src->u.call.args[0];
            if (arg->kind == NX_VAL_REGEX) return bind_value(c, arg, type, op);
            if (arg->kind != NX_VAL_WORD && arg->kind != NX_VAL_STRING) goto mismatch;
            v->kind = V_REGEX;
            nx_status rs = nx_regex_compile(raw_text(arg), 0, &v->regex, NULL);
            if (rs != NX_OK) { fail(c, rs, src->span, "Invalid or unsupported regular expression"); return NULL; }
            return v;
        }
        else goto unsupported;
        v->text = raw_text(src->u.call.args[0]);
        if (v->kind == V_FUZZY && src->u.call.nargs == 2) {
            const nx_value *d = src->u.call.args[1];
            if (d->kind != NX_VAL_NUMBER || !d->u.number.has_int || nx_number_suffix(d).len ||
                d->u.number.i < 0 || d->u.number.i > 64) goto mismatch;
            v->distance = (uint32_t)d->u.number.i;
        } else if (src->u.call.nargs != 1) goto unsupported;
    }
    if (!v->text.p) goto mismatch;
    if (v->kind == V_EXACT) return v;
    if (src->kind == NX_VAL_WORD && v->kind == V_TEXT) {
        for (size_t i = 0; i < v->text.n; i++) {
            if (v->text.p[i] == '*' && i == v->text.n - 1 && i != 0) {
                v->kind = V_PREFIX; v->text.n--; break;
            }
            if (v->text.p[i] == '*' || v->text.p[i] == '?') goto unsupported;
        }
    }
    nx_status st = nx_utf8_normalize(&c->arena, v->text, NX_UTF8_FOLD_ASCII | NX_UTF8_FOLD_WIDTH,
                                     16384, &v->text);
    if (st == NX_OK && v->kind == V_TEXT) st = nx_utf8_tokenize(&c->arena, v->text, NULL, &v->tokens);
    if (st != NX_OK) { fail(c, st, src->span, "Cannot analyze query text"); return NULL; }
    if (c->arena.total > SEARCH_ARENA_LIMIT) goto limit;
    if (v->kind == V_TEXT && (v->tokens.count == 0 || v->tokens.count > SEARCH_TERMS)) goto limit;
    if (v->kind != V_SUBSTR && v->text.n == 0) goto mismatch;
    if (v->kind == V_SUBSTR && v->text.n) {
        v->prefix = alloc_arena(c, v->text.n * sizeof(size_t), _Alignof(size_t));
        if (!v->prefix) return NULL;
        for (size_t i = 1, j = 0; i < v->text.n; i++) {
            while (j && v->text.p[i] != v->text.p[j]) j = v->prefix[j - 1];
            if (v->text.p[i] == v->text.p[j]) j++;
            v->prefix[i] = j;
        }
    }
    if (v->kind == V_FUZZY && v->distance == UINT32_MAX) {
        size_t at = 0, scalars = 0; uint32_t cp;
        while (nx_utf8_decode(v->text, &at, &cp) == NX_OK) scalars++;
        v->distance = scalars <= 2 ? 0u : scalars <= 5 ? 1u : 2u;
    }
    return v;
unsupported:
    fail(c, NX_ERR_UNSUPPORTED, src->span, "This value/operator is not supported by snapshot search"); return NULL;
mismatch:
    fail(c, NX_ERR_TYPE, src->span, "Query value or operator does not match the field type"); return NULL;
limit:
    fail(c, NX_ERR_LIMIT, src->span, "Query expansion/text limit exceeded"); return NULL;
}

static bool scores_text(const value *v) {
    if (v->kind == V_ANY) {
        for (uint32_t i = 0; i < v->count; i++) if (scores_text(v->items[i])) return true;
        return false;
    }
    return v->kind >= V_TEXT && v->kind <= V_FUZZY;
}
static size_t count_indexes(const value *v) {
    if (v->kind == V_INT) return 1;
    size_t count = 0;
    if (v->kind == V_ANY) for (uint32_t i = 0; i < v->count; i++) count += count_indexes(v->items[i]);
    return count;
}
static plan *bind_node(search_context *c, const nx_node *node, bool positive) {
    plan *p = alloc_arena(c, sizeof(*p), _Alignof(plan));
    if (!p) return NULL;
    p->source = node; p->field = NO_FIELD;
    if (node->kind != NX_NODE_CLAUSE) {
        p->kids = alloc_arena(c, (size_t)node->u.list.n * sizeof(plan *), _Alignof(plan *));
        if (!p->kids) return NULL;
        for (uint32_t i = 0; i < node->u.list.n; i++) {
            p->kids[i] = bind_node(c, node->u.list.kids[i], positive && node->kind != NX_NODE_NOT);
            if (!p->kids[i]) return NULL;
        }
        return p;
    }
    const nx_clause *cl = &node->u.clause;
    if (cl->nparams) { fail(c, NX_ERR_UNSUPPORTED, node->span, "Query parameters are not implemented"); return NULL; }
    if (!finite_number(cl->boost) || cl->boost < 0) { fail(c, NX_ERR_TYPE, node->span, "Invalid query boost"); return NULL; }
    nx_field field = {0};
    if (cl->field.s) {
        nx_status st = nx_table_find(c->table, nx_slice_make(cl->field.s, cl->field.len), &p->field);
        if (st != NX_OK && cl->field.len == 8 && memcmp(cl->field.s, "semantic", 8) == 0) {
            uint32_t found = 0;
            for (uint32_t f = 0; f < c->table->fields; f++) {
                nx_table_field(c->table, f, &field);
                if (field.type == NX_FIELD_VECTOR) { p->field = f; found++; }
            }
            st = found == 1 ? NX_OK : NX_ERR_TYPE;
        }
        if (st != NX_OK) { fail(c, st == NX_ERR_NOT_FOUND ? NX_ERR_TYPE : st, cl->field.span, "Unknown or ambiguous query field"); return NULL; }
        nx_table_field(c->table, p->field, &field); p->type = field.type;
    } else p->type = NX_FIELD_TEXT;
    p->v = bind_value(c, cl->value, p->type, cl->op);
    if (!p->v) return NULL;
    if (p->type == NX_FIELD_VECTOR && p->v->kind != V_EXISTS &&
        (p->v->kind != V_VECTOR || cl->value->u.vec.n != field.dims)) {
        fail(c, NX_ERR_TYPE, node->span, "Vector dimensions do not match schema"); return NULL;
    }
    if (positive && p->v->kind == V_VECTOR) c->vector = true;
    if (positive && p->type == NX_FIELD_TEXT && scores_text(p->v)) c->lexical = true;
    if (p->type == NX_FIELD_INT && c->options.indexed) c->stats.numeric_indexes += count_indexes(p->v);
    return p;
}

static bool contains(nx_slice hay, const value *v) {
    if (v->text.n > hay.n) return false;
    if (!v->text.n) return true;
    for (size_t i = 0, j = 0; i < hay.n; i++) {
        while (j && hay.p[i] != v->text.p[j]) j = v->prefix[j - 1];
        if (hay.p[i] == v->text.p[j]) j++;
        if (j == v->text.n) return true;
    }
    return false;
}
static bool scalar_match(const nx_cell *cell, const value *v, nx_op op) {
    if (!cell->present) return false;
    switch (v->kind) {
        case V_EXISTS: case V_VECTOR: return true;
        case V_INT: return compare_result((cell->as.integer > v->integer) - (cell->as.integer < v->integer), op);
        case V_FLOAT: return compare_result((cell->as.real > v->real) - (cell->as.real < v->real), op);
        case V_BOOL: return compare_result((int)cell->as.boolean - (int)v->integer, op);
        case V_EXACT: return compare_result(compare_bytes(cell->as.text, v->text), op);
        case V_RANGE: return (!v->items[0] || scalar_match(cell, v->items[0], NX_OP_GE)) &&
                            (!v->items[1] || scalar_match(cell, v->items[1], NX_OP_LE));
        default: return false;
    }
}

static int prepared_key_compare(const void *aa, const void *bb) {
    uint32_t a = *(const uint32_t *)aa, b = *(const uint32_t *)bb;
    return (a > b) - (a < b);
}
static const nx_prepared_term *prepared_find_term(const nx_prepared_field *field, nx_slice text) {
    size_t lo = 0, hi = field->term_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compare_bytes(field->terms[mid].text, text);
        if (cmp < 0) lo = mid + 1; else hi = mid;
    }
    return lo < field->term_count && nx_slice_eq(field->terms[lo].text, text) ? &field->terms[lo] : NULL;
}
static const nx_prepared_posting *prepared_find_posting(const nx_prepared_field *field,
                                                        const nx_prepared_term *term,
                                                        uint32_t row) {
    size_t lo = term->start, hi = (size_t)term->start + term->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (field->postings[mid].row < row) lo = mid + 1; else hi = mid;
    }
    return lo < (size_t)term->start + term->count && field->postings[lo].row == row ?
        &field->postings[lo] : NULL;
}
static const nx_prepared_trigram *prepared_find_trigram(const nx_prepared_field *field, uint32_t key) {
    size_t lo = 0, hi = field->trigram_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (field->trigrams[mid].key < key) lo = mid + 1; else hi = mid;
    }
    return lo < field->trigram_count && field->trigrams[lo].key == key ? &field->trigrams[lo] : NULL;
}
static bool prepared_trigram_has_row(const nx_prepared_field *field,
                                     const nx_prepared_trigram *trigram, uint32_t row) {
    size_t lo = trigram->start, hi = (size_t)trigram->start + trigram->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (field->trigram_rows[mid] < row) lo = mid + 1; else hi = mid;
    }
    return lo < (size_t)trigram->start + trigram->count && field->trigram_rows[lo] == row;
}
static bool prepared_phrase_matches(nx_slice text, const nx_utf8_tokens *query,
                                    nx_arena *scratch, nx_status *status) {
    nx_utf8_tokens tokens;
    *status = nx_utf8_tokenize(scratch, text, NULL, &tokens);
    if (*status != NX_OK) return false;
    for (size_t start = 0; start + query->count <= tokens.count; start++) {
        bool same = true;
        for (size_t q = 0; q < query->count; q++) {
            if (!nx_slice_eq(tokens.items[start + q].text, query->items[q].text)) { same = false; break; }
        }
        if (same) return true;
    }
    return false;
}

/* Exact prepared term and trigram execution. Plain terms use collection-wide
 * BM25 statistics from the immutable preparation. Phrase and substring hits
 * are verified against the original normalized field bytes. */
static nx_status prepared_text_value(search_context *c, plan *p, const value *v,
                                     uint32_t field_id, double *scores) {
    const nx_prepared_field *field = &c->prepared->fields[field_id];
    size_t rows = c->table->rows;
    if (!field->text) return NX_ERR_CORRUPT;
    if (v->kind == V_TEXT) {
        c->stats.prepared_text = true;
        if (!v->tokens.count) return NX_ERR_LIMIT;
        const nx_prepared_term *query_terms[SEARCH_TERMS] = {0};
        for (size_t q = 0; q < v->tokens.count; q++) {
            query_terms[q] = prepared_find_term(field, v->tokens.items[q].text);
            if (!query_terms[q]) return NX_OK;
        }
        const nx_prepared_term *first = query_terms[0];
        double avg = field->population ? (double)field->total_length / (double)field->population : 0;
        nx_arena scratch; nx_arena_init(&scratch, 0);
        nx_status st = NX_OK;
        for (uint32_t i = 0; i < first->count && st == NX_OK; i++) {
            const nx_prepared_posting *candidate = &field->postings[first->start + i];
            if (!work(c, v->tokens.count)) { st = NX_ERR_LIMIT; break; }
            uint32_t row = candidate->row;
            uint32_t tfs[SEARCH_TERMS];
            bool matches = true;
            for (size_t q = 0; q < v->tokens.count; q++) {
                const nx_prepared_posting *posting = q == 0 ? candidate :
                    prepared_find_posting(field, query_terms[q], row);
                if (!posting) { matches = false; break; }
                tfs[q] = posting->tf;
            }
            if (!matches) continue;
            if (v->phrase) {
                if (!work(c, (size_t)field->normalized[row].n + 1)) { st = NX_ERR_LIMIT; break; }
                c->stats.scanned_cells++;
                nx_arena_reset(&scratch);
                matches = prepared_phrase_matches(field->normalized[row], &v->tokens, &scratch, &st);
                if (st != NX_OK) break;
            }
            if (!matches) continue;
            double score = 0;
            if (avg > 0) {
                double dl = field->lengths[row];
                for (size_t q = 0; q < v->tokens.count; q++) {
                    double tf = tfs[q], df = query_terms[q]->count;
                    score += log1p(((double)field->population - df + .5) / (df + .5)) * tf /
                        (tf + c->options.bm25_k1 * (1 - c->options.bm25_b +
                        c->options.bm25_b * dl / avg));
                }
            }
            set_bit(p, row);
            scores[row] += score * p->source->u.clause.boost;
            if (!finite_number(scores[row])) st = NX_ERR_LIMIT;
        }
        nx_arena_free(&scratch);
        return st;
    }
    if (v->kind == V_PREFIX) {
        c->stats.prepared_text = true;
        /* A sorted dictionary turns a prefix into one contiguous term range. */
        size_t lo = 0, hi = field->term_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (compare_bytes(field->terms[mid].text, v->text) < 0) lo = mid + 1; else hi = mid;
        }
        uint8_t *matched = nx_calloc(rows ? rows : 1, 1);
        if (!matched) return NX_ERR_NOMEM;
        nx_status st = NX_OK;
        for (size_t i = lo; i < field->term_count; i++) {
            nx_slice term = field->terms[i].text;
            if (term.n < v->text.n || memcmp(term.p, v->text.p, v->text.n) != 0) break;
            const nx_prepared_term *entry = &field->terms[i];
            for (uint32_t j = 0; j < entry->count; j++) {
                if (!work(c, 1)) { st = NX_ERR_LIMIT; break; }
                matched[field->postings[entry->start + j].row] = 1;
            }
            if (st != NX_OK) break;
        }
        if (st == NX_OK && !work(c, rows)) st = NX_ERR_LIMIT;
        for (uint32_t row = 0; row < rows && st == NX_OK; row++) if (matched[row]) {
            set_bit(p, row);
            scores[row] += p->source->u.clause.boost;
            if (!finite_number(scores[row])) st = NX_ERR_LIMIT;
        }
        nx_free(matched);
        return st;
    }
    if (v->kind == V_SUBSTR && v->text.n >= 3) {
        c->stats.prepared_text = true;
        size_t nkeys = v->text.n - 2;
        uint32_t *keys = alloc_arena(c, nkeys * sizeof(*keys), _Alignof(uint32_t));
        if (!keys) return c->status;
        for (size_t i = 0; i < nkeys; i++)
            keys[i] = nx_trigram_pack(v->text.p[i], v->text.p[i + 1], v->text.p[i + 2]);
        qsort(keys, nkeys, sizeof(*keys), prepared_key_compare);
        size_t unique = 0;
        for (size_t i = 0; i < nkeys; i++) if (i == 0 || keys[i] != keys[i - 1]) keys[unique++] = keys[i];
        const nx_prepared_trigram *rarest = NULL;
        for (size_t i = 0; i < unique; i++) {
            const nx_prepared_trigram *candidate = prepared_find_trigram(field, keys[i]);
            if (!candidate) return NX_OK;
            if (!rarest || candidate->count < rarest->count) rarest = candidate;
        }
        for (uint32_t i = 0; i < rarest->count; i++) {
            uint32_t row = field->trigram_rows[rarest->start + i];
            if (!work(c, unique)) return NX_ERR_LIMIT;
            bool candidate = true;
            for (size_t k = 0; k < unique; k++) {
                const nx_prepared_trigram *posting = prepared_find_trigram(field, keys[k]);
                if (!prepared_trigram_has_row(field, posting, row)) { candidate = false; break; }
            }
            if (!candidate) continue;
            nx_slice text = field->normalized[row];
            if (!work(c, text.n + 1)) return NX_ERR_LIMIT;
            if (contains(text, v)) {
                set_bit(p, row);
                scores[row] += p->source->u.clause.boost;
                if (!finite_number(scores[row])) return NX_ERR_LIMIT;
            }
        }
        return NX_OK;
    }
    /* Short substrings, regex and fuzzy matching keep their exact scan path. */
    (void)rows;
    return NX_ERR_UNSUPPORTED;
}

/* A field-at-a-time reference BM25 scorer. Raw tf/dl are transient; statistics
 * cover all documents with tokens, before Boolean filtering. */
static nx_status text_value(search_context *c, plan *p, const value *v, uint32_t field, double *scores) {
    if (c->prepared && (v->kind == V_TEXT || v->kind == V_PREFIX ||
                        (v->kind == V_SUBSTR && v->text.n >= 3))) {
        nx_status prepared = prepared_text_value(c, p, v, field, scores);
        if (prepared != NX_ERR_UNSUPPORTED) return prepared;
    }
    c->stats.scanned_text = true;
    size_t rows = c->table->rows, nt = v->kind == V_TEXT ? v->tokens.count : 0;
    size_t width = nt + 1, cells;
    if (nx_mul_overflow(rows, width, &cells) || cells > 16u * 1024u * 1024u) return NX_ERR_LIMIT;
    uint32_t *counts = nt ? nx_calloc(cells ? cells : 1, sizeof(*counts)) : NULL;
    if (nt && !counts) return NX_ERR_NOMEM;
    uint32_t df[SEARCH_TERMS] = {0}; size_t population = 0, total_length = 0;
    nx_arena scratch; nx_arena_init(&scratch, 0);
    nx_status st = NX_OK;
    uint8_t *matched = nx_calloc(rows ? rows : 1, 1);
    if (!matched) { nx_free(counts); nx_arena_free(&scratch); return NX_ERR_NOMEM; }
    for (uint32_t row = 0; row < rows && st == NX_OK; row++) {
        nx_cell cell; nx_table_get(c->table, row, field, &cell);
        if (!cell.present) continue;
        size_t charge;
        if (nx_mul_overflow(cell.as.text.n + 1, nt + 1, &charge) || !work(c, charge)) { st = NX_ERR_LIMIT; break; }
        c->stats.scanned_cells++;
        nx_arena_reset(&scratch);
        if (v->kind == V_REGEX) {
            bool yes = false;
            size_t remaining = c->options.max_work - c->stats.work;
            if (!remaining) { st = NX_ERR_LIMIT; break; }
            uint64_t used = 0;
            st = nx_regex_match_counted(v->regex, cell.as.text, (uint64_t)remaining, &yes, &used);
            if (!work(c, (size_t)used)) st = NX_ERR_LIMIT;
            matched[row] = (uint8_t)yes; continue;
        }
        nx_slice normalized;
        st = nx_utf8_normalize(&scratch, cell.as.text, NX_UTF8_FOLD_ASCII | NX_UTF8_FOLD_WIDTH,
                              1024u * 1024u, &normalized);
        if (st != NX_OK) break;
        if (v->kind == V_SUBSTR) {
            if (!work(c, normalized.n + 1)) { st = NX_ERR_LIMIT; break; }
            matched[row] = (uint8_t)contains(normalized, v); continue;
        }
        nx_utf8_tokens ts;
        st = nx_utf8_tokenize(&scratch, cell.as.text, NULL, &ts);
        if (st != NX_OK) break;
        if (v->kind == V_TEXT) {
            counts[(size_t)row * width] = (uint32_t)ts.count;
            if (ts.count) { population++; total_length += ts.count; }
            bool all = true;
            for (size_t q = 0; q < nt; q++) {
                uint32_t tf = 0;
                for (size_t t = 0; t < ts.count; t++) if (nx_slice_eq(ts.items[t].text, v->tokens.items[q].text)) tf++;
                counts[(size_t)row * width + q + 1] = tf;
                if (tf) df[q]++; else all = false;
            }
            if (all && v->phrase) {
                all = false;
                for (size_t t = 0; t + nt <= ts.count && !all; t++) {
                    all = true;
                    for (size_t q = 0; q < nt; q++) if (!nx_slice_eq(ts.items[t + q].text, v->tokens.items[q].text)) { all = false; break; }
                }
            }
            matched[row] = (uint8_t)all;
        } else {
            for (size_t t = 0; t < ts.count && !matched[row]; t++) {
                nx_slice token = ts.items[t].text;
                if (v->kind == V_PREFIX) matched[row] = (uint8_t)(token.n >= v->text.n &&
                    (!v->text.n || !memcmp(token.p, v->text.p, v->text.n)));
                else if (v->kind == V_FUZZY) {
                    size_t cost;
                    if (nx_mul_overflow(token.n + v->text.n + 1, (size_t)v->distance * 2 + 1, &cost) || !work(c, cost)) { st = NX_ERR_LIMIT; break; }
                    uint32_t distance = 0;
                    st = nx_fuzzy_distance(token, v->text, v->distance, &distance);
                    matched[row] = (uint8_t)(st == NX_OK && distance <= v->distance);
                }
            }
        }
    }
    for (uint32_t row = 0; row < rows && st == NX_OK; row++) if (matched[row]) {
        set_bit(p, row);
        double score = nt ? 0 : 1;
        if (nt && population) {
            double avg = (double)total_length / (double)population;
            double dl = counts[(size_t)row * width];
            for (size_t q = 0; q < nt; q++) {
                double tf = counts[(size_t)row * width + q + 1];
                if (tf) score += log1p(((double)population - df[q] + .5) / ((double)df[q] + .5)) * tf /
                    (tf + c->options.bm25_k1 * (1 - c->options.bm25_b + c->options.bm25_b * dl / avg));
            }
        }
        scores[row] += score * p->source->u.clause.boost;
        if (!finite_number(scores[row])) st = NX_ERR_LIMIT;
    }
    nx_free(matched); nx_free(counts); nx_arena_free(&scratch); return st;
}

static nx_status eval_value(search_context *c, plan *p, const value *v, double *scores) {
    if (v->kind == V_ANY) {
        for (uint32_t i = 0; i < v->count; i++) {
            nx_status st = eval_value(c, p, v->items[i], scores); if (st != NX_OK) return st;
        }
        return NX_OK;
    }
    if (p->field == NO_FIELD && v->kind == V_EXISTS) {
        for (uint32_t row = 0; row < c->table->rows; row++) set_bit(p, row);
        return work(c, c->table->rows) ? NX_OK : NX_ERR_LIMIT;
    }
    if (v->kind >= V_TEXT && v->kind <= V_FUZZY) {
        if (p->field != NO_FIELD) return text_value(c, p, v, p->field, scores);
        for (uint32_t f = 0; f < c->table->fields; f++) {
            nx_field info; nx_table_field(c->table, f, &info);
            if (info.type == NX_FIELD_TEXT && !eqs(info.name, "_id")) {
                nx_status st = text_value(c, p, v, f, scores); if (st != NX_OK) return st;
            }
        }
        return NX_OK;
    }
    if (p->field == NO_FIELD) return NX_ERR_UNSUPPORTED;
    if (v->kind == V_INT && c->options.indexed) {
        nx_bsi index; nx_buf output; nx_buf_init(&output);
        if (!work(c, ((size_t)c->table->rows + 63) / 64 * 65)) return NX_ERR_LIMIT;
        nx_status st = nx_table_int_index(c->table, p->field, &index);
        if (st == NX_OK) st = nx_bsi_filter(&index, bsi_op(p->source->u.clause.op), v->integer, &output);
        nx_bitmap bitmap;
        if (st == NX_OK) st = nx_bitmap_open(nx_buf_slice(&output), &bitmap);
        if (st == NX_OK) {
            nx_bitmap_iter it; nx_bitmap_iter_init(&bitmap, &it); uint32_t row;
            while (nx_bitmap_next(&it, &row)) set_bit(p, row);
        }
        nx_buf_free(&output); return st;
    }
    for (uint32_t row = 0; row < c->table->rows; row++) {
        if (!work(c, 1)) return NX_ERR_LIMIT;
        nx_cell cell; nx_table_get(c->table, row, p->field, &cell); c->stats.scanned_cells++;
        if (cell.present && v->kind == V_EXACT && !work(c, cell.as.text.n + v->text.n)) return NX_ERR_LIMIT;
        if (scalar_match(&cell, v, p->source->u.clause.op)) set_bit(p, row);
    }
    return NX_OK;
}

static nx_status eval_node(search_context *c, plan *p, double **out_scores) {
    size_t rows = c->table->rows;
    p->bits = alloc_arena(c, (rows + 7) / 8, 1);
    if (!p->bits) return c->status;
    double *scores = nx_calloc(rows ? rows : 1, sizeof(double));
    if (!scores) return NX_ERR_NOMEM;
    nx_status st = NX_OK;
    if (p->source->kind == NX_NODE_CLAUSE) st = eval_value(c, p, p->v, scores);
    else {
        nx_node_kind kind = p->source->kind;
        if (kind == NX_NODE_AND) memset(p->bits, 255, (rows + 7) / 8);
        for (uint32_t k = 0; k < p->source->u.list.n && st == NX_OK; k++) {
            double *child_scores = NULL;
            st = eval_node(c, p->kids[k], &child_scores);
            if (st != NX_OK) break;
            if (!work(c, rows)) st = NX_ERR_LIMIT;
            for (uint32_t row = 0; row < rows && st == NX_OK; row++) {
                bool child = bit(p->kids[k], row);
                if (kind == NX_NODE_NOT) { if (!child) set_bit(p, row); }
                else {
                    if (kind == NX_NODE_AND && !child) p->bits[row / 8] &= (uint8_t)~(1u << (row % 8));
                    if (kind == NX_NODE_OR && child) set_bit(p, row);
                    if (child) scores[row] += child_scores[row];
                    if (!finite_number(scores[row])) st = NX_ERR_LIMIT;
                }
            }
            nx_free(child_scores);
        }
    }
    if (st != NX_OK) { nx_free(scores); return st; }
    *out_scores = scores; return NX_OK;
}

static nx_status vector_score(search_context *c, const plan *p, uint32_t row, double *score, bool *active) {
    if (!bit(p, row) || p->source->kind == NX_NODE_NOT) return NX_OK;
    if (p->source->kind != NX_NODE_CLAUSE) {
        for (uint32_t k = 0; k < p->source->u.list.n; k++) {
            nx_status st = vector_score(c, p->kids[k], row, score, active); if (st != NX_OK) return st;
        }
    } else if (p->v->kind == V_VECTOR) {
        nx_cell cell; nx_table_get(c->table, row, p->field, &cell);
        const nx_value *v = p->v->source;
        if (!work(c, v->u.vec.n)) return NX_ERR_LIMIT;
        double dot = 0, aa = 0, bb = 0;
        for (uint32_t j = 0; j < v->u.vec.n; j++) {
            double a = nx_cell_vector_at(&cell, j), b = v->u.vec.v[j];
            dot += a * b; aa += a * a; bb += b * b;
        }
        double cosine = aa == 0 || bb == 0 ? 0 : dot / sqrt(aa * bb);
        if (cosine > 1) cosine = 1;
        if (cosine < -1) cosine = -1;
        *score += cosine * p->source->u.clause.boost; *active = true; c->stats.vectors_scored++;
        if (!finite_number(*score)) return NX_ERR_LIMIT;
    }
    return NX_OK;
}

static int hit_compare(search_context *c, const ranked_hit *a, const ranked_hit *b, int channel) {
    if (channel == 1 || channel == 2) {
        double x = channel == 1 ? a->lexical : a->vector, y = channel == 1 ? b->lexical : b->vector;
        if (channel == 2 && a->has_vector != b->has_vector) return a->has_vector ? -1 : 1;
        if (x != y) return x > y ? -1 : 1;
    } else if (c->nsort) {
        for (uint32_t i = 0; i < c->nsort; i++) {
            int cmp = 0;
            if (c->sort_fields[i] == NO_FIELD) cmp = (a->hit.score > b->hit.score) - (a->hit.score < b->hit.score);
            else {
                nx_cell x, y;
                nx_table_get(c->table, a->hit.row, c->sort_fields[i], &x);
                nx_table_get(c->table, b->hit.row, c->sort_fields[i], &y);
                if (x.present != y.present) return x.present ? -1 : 1;
                if (!x.present) continue;
                switch (x.type) {
                    case NX_FIELD_INT: cmp = (x.as.integer > y.as.integer) - (x.as.integer < y.as.integer); break;
                    case NX_FIELD_FLOAT: cmp = (x.as.real > y.as.real) - (x.as.real < y.as.real); break;
                    case NX_FIELD_BOOL: cmp = (int)x.as.boolean - (int)y.as.boolean; break;
                    case NX_FIELD_TEXT:
                        if (!work(c, x.as.text.n + y.as.text.n)) return 0;
                        cmp = compare_bytes(x.as.text, y.as.text); break;
                    default: break;
                }
            }
            if (cmp) return c->sort_desc[i] ? -cmp : cmp;
        }
    } else if (a->hit.score != b->hit.score) return a->hit.score > b->hit.score ? -1 : 1;
    if (!work(c, a->hit.id.n + b->hit.id.n)) return 0;
    return compare_bytes(a->hit.id, b->hit.id);
}
static nx_status sort_hits(search_context *c, ranked_hit *hits, size_t n, ranked_hit *tmp, int channel) {
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t base = 0; base < n; base += width * 2) {
            size_t mid = base + width < n ? base + width : n;
            size_t end = mid + width < n ? mid + width : n;
            size_t i = base, j = mid, k = base;
            while (i < mid || j < end) {
                if (!work(c, 1 + c->nsort)) return NX_ERR_LIMIT;
                tmp[k++] = j == end || (i < mid && hit_compare(c, &hits[i], &hits[j], channel) <= 0) ? hits[i++] : hits[j++];
            }
        }
        if (c->status != NX_OK) return c->status;
        memcpy(hits, tmp, n * sizeof(*hits));
    }
    return NX_OK;
}

nx_search_options nx_search_default_options(void) {
    nx_search_options o = {true, 100000000, 1000, 1.2, .75}; return o;
}
void nx_search_result_free(nx_search_result *r) { if (r) { nx_free(r->hits); memset(r, 0, sizeof(*r)); } }
static nx_status search_run(const nx_table *table, nx_slice query, const nx_search_options *options,
                            const nx_search_index *prepared, nx_search_result *out, nx_error *error) {
    nx_error_clear(error);
    if (!out) { nx_error_set(error, NX_ERR_INVALID, -1, 0, "Missing search output"); return NX_ERR_INVALID; }
    memset(out, 0, sizeof(*out));
    if (!table || !table->bytes.p || (!query.p && query.n)) {
        nx_error_set(error, NX_ERR_INVALID, -1, 0, "Search needs a validated table and query bytes"); return NX_ERR_INVALID;
    }
    search_context c = {0}; c.table = table; c.prepared = prepared; c.error = error;
    c.options = options ? *options : nx_search_default_options();
    if (!c.options.indexed) c.prepared = NULL;
    if (!finite_number(c.options.bm25_k1) || c.options.bm25_k1 < 0 || c.options.bm25_b < 0 ||
        c.options.bm25_b > 1 || !finite_number(c.options.bm25_b) || c.options.max_hits > 1000000) {
        nx_error_set(error, NX_ERR_INVALID, -1, 0, "Invalid search options"); return NX_ERR_INVALID;
    }
    nx_arena_init(&c.arena, 0);
    nx_parse_limits limits = nx_parse_limits_default(); limits.max_clauses = 256; limits.max_list_items = 256; limits.max_depth = 32;
    nx_stmt *stmt = NULL; plan *root = NULL; double *lexical = NULL;
    ranked_hit *hits = NULL, *tmp = NULL;
    nx_status st = nx_query_parse(&c.arena, (const char *)query.p, query.n, &limits, &stmt, error);
    size_t offset = 0, limit = 20;
    if (st != NX_OK) goto done;
    if (stmt->mode == NX_STMT_WATCH || stmt->nsources || stmt->nfacets) {
        st = fail(&c, NX_ERR_UNSUPPORTED, stmt->span, "WATCH, SOURCE and FACET are not implemented"); goto done;
    }
    if (stmt->nsort > 8) { st = NX_ERR_LIMIT; goto done; }
    c.nsort = stmt->nsort;
    for (uint32_t i = 0; i < stmt->nsort; i++) {
        nx_slice name = nx_slice_make(stmt->sort[i].field.s, stmt->sort[i].field.len);
        if (eqs(name, "_score")) c.sort_fields[i] = NO_FIELD;
        else {
            st = nx_table_find(table, name, &c.sort_fields[i]);
            nx_field f;
            if (st != NX_OK) { st = NX_ERR_TYPE; goto done; }
            nx_table_field(table, c.sort_fields[i], &f);
            if (f.type == NX_FIELD_VECTOR) { st = NX_ERR_TYPE; goto done; }
        }
        c.sort_desc[i] = stmt->sort[i].dir == NX_SORT_DESC ||
            (stmt->sort[i].dir == NX_SORT_DEFAULT && c.sort_fields[i] == NO_FIELD);
    }
    if (stmt->has_limit) {
        if ((uint64_t)stmt->limit > c.options.max_hits) { st = NX_ERR_LIMIT; goto done; }
        limit = (size_t)stmt->limit;
    } else if (limit > c.options.max_hits) limit = c.options.max_hits;
    if (stmt->has_offset) {
        if ((uint64_t)stmt->offset > SIZE_MAX) { st = NX_ERR_LIMIT; goto done; }
        offset = (size_t)stmt->offset;
    }
    if (stmt->where) {
        root = bind_node(&c, stmt->where, true);
        if (!root) { st = c.status; goto done; }
    }
    if (stmt->mode == NX_STMT_EXPLAIN) { c.stats.explain_only = true; goto success; }
    if (root) { st = eval_node(&c, root, &lexical); if (st != NX_OK) goto done; }
    hits = nx_calloc(table->rows ? table->rows : 1, sizeof(*hits));
    tmp = nx_calloc(table->rows ? table->rows : 1, sizeof(*tmp));
    if (!hits || !tmp) { st = NX_ERR_NOMEM; goto done; }
    for (uint32_t row = 0; row < table->rows; row++) if (!root || bit(root, row)) {
        ranked_hit *hit = &hits[c.stats.total++]; hit->hit.row = row;
        hit->hit.id = nx_table_id(table, row); hit->hit.document = nx_table_document(table, row);
        hit->lexical = lexical ? lexical[row] : 0;
        if (root && c.vector) {
            st = vector_score(&c, root, row, &hit->vector, &hit->has_vector); if (st != NX_OK) goto done;
        }
        hit->hit.score = c.vector && !c.lexical ? hit->vector : hit->lexical;
    }
    if (c.lexical && c.vector) {
        st = sort_hits(&c, hits, c.stats.total, tmp, 1); if (st != NX_OK) goto done;
        uint32_t rank = 0;
        for (size_t i = 0; i < c.stats.total; i++) if (hits[i].lexical > 0) hits[i].lr = ++rank;
        st = sort_hits(&c, hits, c.stats.total, tmp, 2); if (st != NX_OK) goto done;
        rank = 0;
        for (size_t i = 0; i < c.stats.total; i++) if (hits[i].has_vector) hits[i].vr = ++rank;
        for (size_t i = 0; i < c.stats.total; i++) hits[i].hit.score =
            (hits[i].lr ? 1.0 / (60.0 + hits[i].lr) : 0) + (hits[i].vr ? 1.0 / (60.0 + hits[i].vr) : 0);
    }
    st = sort_hits(&c, hits, c.stats.total, tmp, 0); if (st != NX_OK) goto done;
    c.stats.count = offset < c.stats.total ? c.stats.total - offset : 0;
    if (c.stats.count > limit) c.stats.count = limit;
    if (c.stats.count) {
        c.stats.hits = nx_calloc(c.stats.count, sizeof(*c.stats.hits));
        if (!c.stats.hits) { st = NX_ERR_NOMEM; goto done; }
        for (size_t i = 0; i < c.stats.count; i++) c.stats.hits[i] = hits[offset + i].hit;
    }
success:
    c.stats.indexed = c.options.indexed; c.stats.has_lexical = c.lexical; c.stats.has_vector = c.vector;
    *out = c.stats;
done:
    if (st != NX_OK) {
        nx_free(c.stats.hits);
        if (!error || error->code == NX_OK) nx_error_set(error, st, -1, 0, "%s", nx_status_str(st));
    }
    for (value *v = c.values; v; v = v->cleanup_next) nx_regex_free(v->regex);
    nx_free(hits); nx_free(tmp); nx_free(lexical); nx_arena_free(&c.arena);
    return st;
}

nx_status nx_search(const nx_table *table, nx_slice query, const nx_search_options *options,
                    nx_search_result *out, nx_error *error) {
    return search_run(table, query, options, NULL, out, error);
}

nx_status nx_search_index_search(const nx_search_index *index, nx_slice query,
                                 const nx_search_options *options,
                                 nx_search_result *out, nx_error *error) {
    if (!index) {
        nx_error_clear(error);
        if (out) memset(out, 0, sizeof(*out));
        nx_error_set(error, NX_ERR_INVALID, -1, 0, "Missing prepared search index");
        return NX_ERR_INVALID;
    }
    return search_run(&index->table, query, options, index, out, error);
}

static void json_string(nx_buf *out, nx_slice s) {
    nx_buf_put_u8(out, '"');
    for (size_t i = 0; i < s.n; i++) {
        uint8_t ch = s.p[i];
        if (ch == '"' || ch == '\\') { nx_buf_put_u8(out, '\\'); nx_buf_put_u8(out, ch); }
        else if (ch < 32) nx_buf_printf(out, "\\u%04x", (unsigned)ch);
        else nx_buf_put_u8(out, ch);
    }
    nx_buf_put_u8(out, '"');
}
nx_status nx_search_json(const nx_search_result *r, nx_buf *out) {
    if (!r || !out || (r->count && !r->hits)) return NX_ERR_INVALID;
    size_t start = out->len; bool old_oom = out->oom;
    nx_buf_printf(out, "{\"total\":%zu,\"count\":%zu,\"explain_only\":%s,\"execution\":{\"numeric_mode\":\"%s\",\"lexical\":\"%s\",\"vector\":\"%s\",\"fusion\":\"%s\",\"numeric_indexes\":%zu,\"scanned_cells\":%zu,\"vectors_scored\":%zu,\"work\":%zu},\"hits\":[",
        r->total, r->count, r->explain_only ? "true" : "false", r->indexed ? "bsi" : "scan",
        r->has_lexical ? (r->prepared_text && r->scanned_text ? "text_mixed" :
            r->prepared_text ? "text_postings" : "text_scan") : "none", r->has_vector ? "exact_cosine" : "none",
        r->has_lexical && r->has_vector ? "rrf" : "none", r->numeric_indexes, r->scanned_cells, r->vectors_scored, r->work);
    for (size_t i = 0; i < r->count; i++) {
        if (i) nx_buf_put_u8(out, ',');
        nx_buf_put_str(out, "{\"_id\":"); json_string(out, r->hits[i].id);
        nx_buf_printf(out, ",\"row\":%" PRIu32, r->hits[i].row);
        if (!finite_number(r->hits[i].score)) { out->len = start; out->oom = old_oom; return NX_ERR_INVALID; }
        /* Keep JSON decimal punctuation independent of the caller's locale. */
        char number[64]; (void)snprintf(number, sizeof(number), "%.17g", r->hits[i].score);
        nx_buf_put_str(out, ",\"score\":");
        const char *point = localeconv()->decimal_point;
        const char *found = point && *point ? strstr(number, point) : NULL;
        if (found && strcmp(point, ".") != 0) {
            nx_buf_put(out, number, (size_t)(found - number)); nx_buf_put_u8(out, '.');
            nx_buf_put_str(out, found + strlen(point));
        } else nx_buf_put_str(out, number);
        nx_buf_put_str(out, ",\"document\":"); nx_buf_put(out, r->hits[i].document.p, r->hits[i].document.n);
        nx_buf_put_u8(out, '}');
    }
    nx_buf_put_str(out, "]}");
    if (out->oom) { out->len = start; out->oom = old_oom; return NX_ERR_NOMEM; }
    return NX_OK;
}
