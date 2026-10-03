#include "query/nx_ast.h"
#include <stdio.h>
#include <locale.h>

const char *nx_op_str(nx_op op) {
    static const char *const names[] = {":", "=", "!=", "<", "<=", ">", ">=", "~"};
    return (unsigned)op < NX_ARRAY_LEN(names) ? names[op] : "?";
}
const char *nx_node_kind_str(nx_node_kind k) {
    static const char *const names[] = {"and", "or", "not", "clause"};
    return (unsigned)k < NX_ARRAY_LEN(names) ? names[k] : "?";
}
const char *nx_value_kind_str(nx_value_kind k) {
    static const char *const names[] = {"word", "string", "regex", "number", "datetime", "range", "anyof", "call", "star", "vector"};
    return (unsigned)k < NX_ARRAY_LEN(names) ? names[k] : "?";
}
const char *nx_stmt_mode_str(nx_stmt_mode m) {
    static const char *const names[] = {"NORMAL", "EXPLAIN", "EXPLAIN ANALYZE", "WATCH"};
    return (unsigned)m < NX_ARRAY_LEN(names) ? names[m] : "?";
}
static void str(nx_buf *b, nx_str s) { nx_buf_put(b, s.s, s.len); }
static void name(nx_buf *b, nx_name n) { nx_buf_put(b, n.s, n.len); }
static void quoted(nx_buf *b, nx_str s) {
    nx_buf_put_u8(b, '"');
    for (uint32_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.s[i];
        if (c == '"' || c == '\\') { nx_buf_put_u8(b, '\\'); nx_buf_put_u8(b, c); }
        else if (c == '\n') nx_buf_put_str(b, "\\n");
        else if (c == '\t') nx_buf_put_str(b, "\\t");
        else if (c < 32) nx_buf_printf(b, "\\u%04x", (unsigned)c);
        else nx_buf_put_u8(b, c);
    }
    nx_buf_put_u8(b, '"');
}
static void decimal(nx_buf *b, double d, int precision) {
    char text[128];
    int n = snprintf(text, sizeof text, "%.*g", precision, d);
    if (n < 0 || (size_t)n >= sizeof text) { b->oom = true; return; }
    /* Formatting honors a caller's locale; NexusQL always uses a dot. */
    const char *point = localeconv()->decimal_point;
    char *found = point && *point ? strstr(text, point) : NULL;
    if (found && strcmp(point, ".") != 0) {
        nx_buf_put(b, text, (size_t)(found - text)); nx_buf_put_u8(b, '.');
        nx_buf_put_str(b, found + strlen(point));
    } else nx_buf_put(b, text, (size_t)n);
}
void nx_ast_print_value(nx_buf *b, const nx_value *v) {
    if (!v) { nx_buf_put_u8(b, '*'); return; }
    switch (v->kind) {
        case NX_VAL_WORD: str(b, v->u.text); break;
        case NX_VAL_STRING: quoted(b, v->u.text); break;
        case NX_VAL_NUMBER: str(b, v->u.number.raw); break;
        case NX_VAL_DATETIME: str(b, v->u.datetime.raw); break;
        case NX_VAL_STAR: nx_buf_put_u8(b, '*'); break;
        case NX_VAL_REGEX: {
            nx_buf_put_u8(b, '/');
            for (uint32_t i = 0; i < v->u.regex.pattern.len; i++) {
                uint8_t c = (uint8_t)v->u.regex.pattern.s[i];
                if (c == '/') nx_buf_put_u8(b, '\\');
                nx_buf_put_u8(b, c);
            }
            nx_buf_put_u8(b, '/');
            if (v->u.regex.flags & NX_REGEX_ICASE) nx_buf_put_u8(b, 'i');
            if (v->u.regex.flags & NX_REGEX_DOTALL) nx_buf_put_u8(b, 's');
            if (v->u.regex.flags & NX_REGEX_MULTI) nx_buf_put_u8(b, 'm');
            break;
        }
        case NX_VAL_RANGE:
            nx_ast_print_value(b, v->u.range.lo); nx_buf_put_str(b, ".."); nx_ast_print_value(b, v->u.range.hi); break;
        case NX_VAL_ANYOF:
            nx_buf_put_u8(b, '(');
            for (uint32_t i = 0; i < v->u.anyof.n; i++) {
                if (i) nx_buf_put_str(b, " OR ");
                nx_ast_print_value(b, v->u.anyof.items[i]);
            }
            nx_buf_put_u8(b, ')'); break;
        case NX_VAL_CALL:
            str(b, v->u.call.name); nx_buf_put_u8(b, '(');
            for (uint32_t i = 0; i < v->u.call.nargs; i++) {
                if (i) nx_buf_put_u8(b, ',');
                nx_ast_print_value(b, v->u.call.args[i]);
            }
            nx_buf_put_u8(b, ')'); break;
        case NX_VAL_VECTOR:
            nx_buf_put_u8(b, '[');
            for (uint32_t i = 0; i < v->u.vec.n; i++) {
                if (i) nx_buf_put_u8(b, ',');
                decimal(b, v->u.vec.v[i], 9);
            }
            nx_buf_put_u8(b, ']'); break;
    }
}
static int precedence(const nx_node *n) { return n->kind == NX_NODE_OR ? 1 : n->kind == NX_NODE_AND ? 2 : n->kind == NX_NODE_NOT ? 3 : 4; }
static void print_node(nx_buf *b, const nx_node *n, int parent) {
    if (!n) return;
    int own = precedence(n); bool parens = own < parent;
    if (parens) nx_buf_put_u8(b, '(');
    if (n->kind == NX_NODE_CLAUSE) {
        const nx_clause *c = &n->u.clause;
        if (c->field.s) {
            name(b, c->field);
            if (c->nparams) {
                nx_buf_put_u8(b, '(');
                for (uint32_t i = 0; i < c->nparams; i++) {
                    if (i) nx_buf_put_u8(b, ',');
                    name(b, c->params[i].name); nx_buf_put_u8(b, '='); nx_ast_print_value(b, c->params[i].value);
                }
                nx_buf_put_u8(b, ')');
            }
            if (c->op >= NX_OP_LT && c->op <= NX_OP_GE) nx_buf_put_u8(b, ':');
            nx_buf_put_str(b, nx_op_str(c->op));
        }
        nx_ast_print_value(b, c->value);
        if (c->boost != 1) { nx_buf_put_u8(b, '^'); decimal(b, c->boost, 17); }
    } else if (n->kind == NX_NODE_NOT) {
        nx_buf_put_str(b, "NOT "); print_node(b, n->u.list.kids[0], own);
    } else {
        for (uint32_t i = 0; i < n->u.list.n; i++) {
            if (i) nx_buf_put_str(b, n->kind == NX_NODE_AND ? " AND " : " OR ");
            print_node(b, n->u.list.kids[i], own);
        }
    }
    if (parens) nx_buf_put_u8(b, ')');
}
void nx_ast_print_node(nx_buf *b, const nx_node *n) { print_node(b, n, 0); }
static void separator(nx_buf *b, size_t start) { if (b->len > start) nx_buf_put_u8(b, ' '); }
void nx_ast_print_stmt(nx_buf *b, const nx_stmt *s) {
    if (!s) return;
    size_t start = b->len;
    if (s->mode != NX_STMT_NORMAL) nx_buf_put_str(b, nx_stmt_mode_str(s->mode));
    if (s->nsources) {
        separator(b, start); nx_buf_put_str(b, "SOURCE(");
        for (uint32_t i = 0; i < s->nsources; i++) { if (i) nx_buf_put_u8(b, ','); name(b, s->sources[i]); }
        nx_buf_put_u8(b, ')');
    }
    if (s->where) { separator(b, start); nx_ast_print_node(b, s->where); }
    if (s->nsort) {
        separator(b, start); nx_buf_put_str(b, "SORT BY ");
        for (uint32_t i = 0; i < s->nsort; i++) {
            if (i) nx_buf_put_u8(b, ',');
            name(b, s->sort[i].field);
            if (s->sort[i].dir != NX_SORT_DEFAULT) nx_buf_put_str(b, s->sort[i].dir == NX_SORT_ASC ? " ASC" : " DESC");
        }
    }
    if (s->has_limit) { separator(b, start); nx_buf_printf(b, "LIMIT %lld", (long long)s->limit); }
    if (s->has_offset) { separator(b, start); nx_buf_printf(b, "OFFSET %lld", (long long)s->offset); }
    if (s->nfacets) {
        separator(b, start); nx_buf_put_str(b, "FACET ");
        for (uint32_t i = 0; i < s->nfacets; i++) { if (i) nx_buf_put_u8(b, ','); name(b, s->facets[i]); }
    }
}
static bool equal_str(nx_str a, nx_str b) { return a.len == b.len && (a.len == 0 || memcmp(a.s, b.s, a.len) == 0); }
static bool equal_name(nx_name a, nx_name b) {
    nx_str x = {a.s, a.len}, y = {b.s, b.len};
    return (a.s != NULL) == (b.s != NULL) && equal_str(x, y);
}
bool nx_ast_equal_value(const nx_value *a, const nx_value *b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case NX_VAL_WORD: case NX_VAL_STRING: return equal_str(a->u.text, b->u.text);
        case NX_VAL_REGEX: return a->u.regex.flags == b->u.regex.flags && equal_str(a->u.regex.pattern, b->u.regex.pattern);
        case NX_VAL_NUMBER:
            return equal_str(a->u.number.raw, b->u.number.raw) && a->u.number.d == b->u.number.d &&
                   a->u.number.has_int == b->u.number.has_int && a->u.number.i == b->u.number.i;
        case NX_VAL_DATETIME: return equal_str(a->u.datetime.raw, b->u.datetime.raw);
        case NX_VAL_STAR: return true;
        case NX_VAL_RANGE:
            return a->u.range.lo_open == b->u.range.lo_open && a->u.range.hi_open == b->u.range.hi_open &&
                   nx_ast_equal_value(a->u.range.lo, b->u.range.lo) && nx_ast_equal_value(a->u.range.hi, b->u.range.hi);
        case NX_VAL_ANYOF:
            if (a->u.anyof.n != b->u.anyof.n) return false;
            for (uint32_t i = 0; i < a->u.anyof.n; i++) if (!nx_ast_equal_value(a->u.anyof.items[i], b->u.anyof.items[i])) return false;
            return true;
        case NX_VAL_CALL:
            if (!equal_str(a->u.call.name, b->u.call.name) || a->u.call.nargs != b->u.call.nargs) return false;
            for (uint32_t i = 0; i < a->u.call.nargs; i++) if (!nx_ast_equal_value(a->u.call.args[i], b->u.call.args[i])) return false;
            return true;
        case NX_VAL_VECTOR:
            if (a->u.vec.n != b->u.vec.n) return false;
            for (uint32_t i = 0; i < a->u.vec.n; i++) if (a->u.vec.v[i] != b->u.vec.v[i]) return false;
            return true;
    }
    return false;
}
bool nx_ast_equal_node(const nx_node *a, const nx_node *b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return false;
    if (a->kind == NX_NODE_CLAUSE) {
        const nx_clause *x = &a->u.clause, *y = &b->u.clause;
        if (!equal_name(x->field, y->field) || x->op != y->op || x->boost != y->boost || x->nparams != y->nparams ||
            !nx_ast_equal_value(x->value, y->value)) return false;
        for (uint32_t i = 0; i < x->nparams; i++)
            if (!equal_name(x->params[i].name, y->params[i].name) || !nx_ast_equal_value(x->params[i].value, y->params[i].value)) return false;
        return true;
    }
    if (a->u.list.n != b->u.list.n) return false;
    for (uint32_t i = 0; i < a->u.list.n; i++) if (!nx_ast_equal_node(a->u.list.kids[i], b->u.list.kids[i])) return false;
    return true;
}
bool nx_ast_equal_stmt(const nx_stmt *a, const nx_stmt *b) {
    if (!a || !b) return a == b;
    if (a->mode != b->mode || a->nsources != b->nsources || a->nsort != b->nsort || a->nfacets != b->nfacets ||
        a->nclauses != b->nclauses || a->has_limit != b->has_limit || a->has_offset != b->has_offset ||
        (a->has_limit && a->limit != b->limit) || (a->has_offset && a->offset != b->offset) || !nx_ast_equal_node(a->where, b->where)) return false;
    for (uint32_t i = 0; i < a->nsources; i++) if (!equal_name(a->sources[i], b->sources[i])) return false;
    for (uint32_t i = 0; i < a->nfacets; i++) if (!equal_name(a->facets[i], b->facets[i])) return false;
    for (uint32_t i = 0; i < a->nsort; i++)
        if (!equal_name(a->sort[i].field, b->sort[i].field) || a->sort[i].dir != b->sort[i].dir) return false;
    return true;
}
static void span(nx_buf *b, nx_span s, bool spans) { if (spans) nx_buf_printf(b, "@%u,%u", s.off, s.len); }
void nx_ast_dump_value(nx_buf *b, const nx_value *v, bool spans) {
    if (!v) { nx_buf_put_u8(b, '*'); return; }
    static const char *const tags[] = {"w", "s", "re", "n", "dt", "range", "any", "call", "*", "vec"};
    nx_buf_put_str(b, tags[v->kind]);
    if (v->kind != NX_VAL_STAR) {
        nx_buf_put_u8(b, '(');
        if (v->kind == NX_VAL_RANGE) {
            nx_ast_dump_value(b, v->u.range.lo, spans); nx_buf_put_u8(b, ' '); nx_ast_dump_value(b, v->u.range.hi, spans);
        } else if (v->kind == NX_VAL_ANYOF) {
            for (uint32_t i = 0; i < v->u.anyof.n; i++) { if (i) nx_buf_put_u8(b, ' '); nx_ast_dump_value(b, v->u.anyof.items[i], spans); }
        } else if (v->kind == NX_VAL_CALL) {
            str(b, v->u.call.name);
            for (uint32_t i = 0; i < v->u.call.nargs; i++) { nx_buf_put_u8(b, ' '); nx_ast_dump_value(b, v->u.call.args[i], spans); }
        } else if (v->kind == NX_VAL_VECTOR) {
            for (uint32_t i = 0; i < v->u.vec.n; i++) { if (i) nx_buf_put_u8(b, ' '); decimal(b, v->u.vec.v[i], 9); }
        } else nx_ast_print_value(b, v);
        nx_buf_put_u8(b, ')');
    }
    span(b, v->span, spans);
}
void nx_ast_dump_node(nx_buf *b, const nx_node *n, bool spans) {
    if (!n) return;
    nx_buf_put_u8(b, '('); nx_buf_put_str(b, n->kind == NX_NODE_CLAUSE ? "c" : nx_node_kind_str(n->kind)); span(b, n->span, spans);
    if (n->kind == NX_NODE_CLAUSE) {
        const nx_clause *c = &n->u.clause;
        nx_buf_put_u8(b, ' '); if (c->field.s) name(b, c->field); else nx_buf_put_u8(b, '-');
        if (c->nparams) {
            nx_buf_put_u8(b, '{');
            for (uint32_t i = 0; i < c->nparams; i++) {
                if (i) nx_buf_put_u8(b, ' ');
                name(b, c->params[i].name); nx_buf_put_u8(b, '='); nx_ast_dump_value(b, c->params[i].value, spans);
            }
            nx_buf_put_u8(b, '}');
        }
        nx_buf_put_u8(b, ' '); nx_buf_put_str(b, nx_op_str(c->op)); nx_buf_put_u8(b, ' '); nx_ast_dump_value(b, c->value, spans);
        if (c->boost != 1) { nx_buf_put_str(b, " ^"); decimal(b, c->boost, 17); }
    } else for (uint32_t i = 0; i < n->u.list.n; i++) { nx_buf_put_u8(b, ' '); nx_ast_dump_node(b, n->u.list.kids[i], spans); }
    nx_buf_put_u8(b, ')');
}
static void dump_stmt(nx_buf *b, const nx_stmt *s, bool spans) {
    nx_buf_put_str(b, "(query");
    if (s) {
        if (s->mode != NX_STMT_NORMAL) { nx_buf_put_u8(b, ' '); nx_buf_put_str(b, s->mode == NX_STMT_EXPLAIN_ANALYZE ? "EXPLAIN-ANALYZE" : nx_stmt_mode_str(s->mode)); }
        if (s->nsources) { nx_buf_put_str(b, " (source"); for (uint32_t i = 0; i < s->nsources; i++) { nx_buf_put_u8(b, ' '); name(b, s->sources[i]); } nx_buf_put_u8(b, ')'); }
        if (s->where) { nx_buf_put_u8(b, ' '); nx_ast_dump_node(b, s->where, spans); }
        if (s->nsort) {
            nx_buf_put_str(b, " (sort");
            for (uint32_t i = 0; i < s->nsort; i++) {
                nx_buf_put_u8(b, ' '); name(b, s->sort[i].field);
                if (s->sort[i].dir != NX_SORT_DEFAULT) nx_buf_put_str(b, s->sort[i].dir == NX_SORT_ASC ? ":asc" : ":desc");
            }
            nx_buf_put_u8(b, ')');
        }
        if (s->has_limit) nx_buf_printf(b, " (limit %lld)", (long long)s->limit);
        if (s->has_offset) nx_buf_printf(b, " (offset %lld)", (long long)s->offset);
        if (s->nfacets) { nx_buf_put_str(b, " (facet"); for (uint32_t i = 0; i < s->nfacets; i++) { nx_buf_put_u8(b, ' '); name(b, s->facets[i]); } nx_buf_put_u8(b, ')'); }
    }
    nx_buf_put_u8(b, ')');
}
void nx_ast_dump_sexpr(nx_buf *b, const nx_stmt *s) { dump_stmt(b, s, false); }
void nx_ast_dump_sexpr_spans(nx_buf *b, const nx_stmt *s) { dump_stmt(b, s, true); }
