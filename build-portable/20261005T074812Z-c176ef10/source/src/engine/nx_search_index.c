#include "engine/nx_search_index_internal.h"
#include "core/nx_mem.h"
#include "core/nx_utf8.h"
#include "index/nx_trigram.h"
#include <stdlib.h>

#define INDEX_MEMORY_LIMIT (256u * 1024u * 1024u)
#define INDEX_TEXT_LIMIT (64u * 1024u * 1024u)
#define INDEX_TOKEN_LIMIT (4u * 1024u * 1024u)
#define INDEX_TRIGRAM_LIMIT (16u * 1024u * 1024u)
typedef struct term_pair { nx_slice text; uint32_t row; } term_pair;
typedef struct trigram_pair { uint32_t key, row; } trigram_pair;

static int compare_text(nx_slice a, nx_slice b) {
    size_t n = a.n < b.n ? a.n : b.n;
    int cmp = n ? memcmp(a.p, b.p, n) : 0;
    return cmp ? cmp : (a.n > b.n) - (a.n < b.n);
}
static int compare_term_pair(const void *aa, const void *bb) {
    const term_pair *a = aa, *b = bb;
    int cmp = compare_text(a->text, b->text);
    return cmp ? cmp : (a->row > b->row) - (a->row < b->row);
}
static int compare_trigram_pair(const void *aa, const void *bb) {
    const trigram_pair *a = aa, *b = bb;
    return a->key != b->key ? (a->key > b->key) - (a->key < b->key) :
        (a->row > b->row) - (a->row < b->row);
}
static void *index_alloc(nx_search_index *index, size_t count, size_t width, nx_status *st) {
    size_t size;
    if (*st != NX_OK) return NULL;
    if (nx_mul_overflow(count, width, &size) || size > INDEX_MEMORY_LIMIT - index->memory_bytes) {
        *st = NX_ERR_LIMIT; return NULL;
    }
    if (!size) return NULL;
    void *ptr = nx_calloc(count, width);
    if (!ptr) { *st = NX_ERR_NOMEM; return NULL; }
    index->memory_bytes += size;
    return ptr;
}

static nx_status build_field(nx_search_index *index, uint32_t field, size_t *text_total, size_t *token_total) {
    nx_prepared_field *f = &index->fields[field];
    nx_status st = NX_OK;
    size_t rows = index->table.rows, bytes = 0, token_count = 0;
    nx_arena scratch; nx_arena_init(&scratch, 0);
    /* Size/validate before large allocations. Analyzer scratch is reset for each
     * document and bounded by the same 1 MiB field profile as scan search. */
    for (uint32_t row = 0; row < rows && st == NX_OK; row++) {
        nx_cell cell; st = nx_table_get(&index->table, row, field, &cell);
        if (st != NX_OK || !cell.present) continue;
        if (cell.as.text.n > 1024u * 1024u || cell.as.text.n > INDEX_TEXT_LIMIT - *text_total) {
            st = NX_ERR_LIMIT; break;
        }
        *text_total += cell.as.text.n; bytes += cell.as.text.n;
        nx_arena_reset(&scratch);
        nx_utf8_tokens tokens;
        st = nx_utf8_tokenize(&scratch, cell.as.text, NULL, &tokens);
        if (st != NX_OK) break;
        if (tokens.count > INDEX_TOKEN_LIMIT - *token_total) { st = NX_ERR_LIMIT; break; }
        *token_total += tokens.count; token_count += tokens.count;
    }
    f->text = true;
    f->normalized = index_alloc(index, rows, sizeof(*f->normalized), &st);
    f->lengths = index_alloc(index, rows, sizeof(*f->lengths), &st);
    /* One byte is retained even for empty text to distinguish present empty
     * values from absent values without relying on a NULL pointer sentinel. */
    f->text_bytes = index_alloc(index, bytes + 1, 1, &st);
    term_pair *pairs = index_alloc(index, token_count, sizeof(*pairs), &st);
    size_t used = 0, n = 0, raw_trigrams = 0;
    for (uint32_t row = 0; row < rows && st == NX_OK; row++) {
        nx_cell cell; st = nx_table_get(&index->table, row, field, &cell);
        if (st != NX_OK || !cell.present) continue;
        nx_arena_reset(&scratch);
        nx_slice normalized;
        st = nx_utf8_normalize(&scratch, cell.as.text, NX_UTF8_FOLD_ASCII | NX_UTF8_FOLD_WIDTH,
                               1024u * 1024u, &normalized);
        if (st != NX_OK) break;
        memcpy(f->text_bytes + used, normalized.p, normalized.n);
        f->normalized[row] = nx_slice_make(f->text_bytes + used, normalized.n);
        used += normalized.n;
        if (normalized.n >= 3) raw_trigrams += normalized.n - 2;
        if (raw_trigrams > INDEX_TRIGRAM_LIMIT) { st = NX_ERR_LIMIT; break; }
        nx_utf8_tokens tokens;
        st = nx_utf8_tokenize(&scratch, f->normalized[row], NULL, &tokens);
        if (st != NX_OK) break;
        f->lengths[row] = (uint32_t)tokens.count;
        f->total_length += tokens.count;
        if (tokens.count) f->population++;
        for (size_t t = 0; t < tokens.count; t++) {
            if (n >= token_count) { st = NX_ERR_CORRUPT; break; }
            pairs[n].text = nx_slice_sub(f->normalized[row], tokens.items[t].offset, tokens.items[t].length);
            pairs[n++].row = row;
        }
    }
    nx_arena_free(&scratch);
    if (st == NX_OK && n != token_count) st = NX_ERR_CORRUPT;
    if (st == NX_OK && token_count) qsort(pairs, token_count, sizeof(*pairs), compare_term_pair);
    size_t terms = 0, postings = 0;
    if (st == NX_OK) for (size_t i = 0; i < token_count; i++) {
        bool new_term = i == 0 || !nx_slice_eq(pairs[i - 1].text, pairs[i].text);
        if (new_term) terms++;
        if (new_term || pairs[i - 1].row != pairs[i].row) postings++;
    }
    f->terms = index_alloc(index, terms, sizeof(*f->terms), &st);
    f->postings = index_alloc(index, postings, sizeof(*f->postings), &st);
    if (st == NX_OK) {
        uint32_t term = 0, post = 0;
        for (size_t i = 0; i < token_count; i++) {
            bool new_term = i == 0 || !nx_slice_eq(pairs[i - 1].text, pairs[i].text);
            if (new_term) {
                term = f->term_count++;
                f->terms[term].text = pairs[i].text;
                f->terms[term].start = post;
            }
            if (new_term || pairs[i - 1].row != pairs[i].row) {
                f->postings[post].row = pairs[i].row;
                f->postings[post++].tf = 1;
                f->terms[term].count++;
            } else f->postings[post - 1].tf++;
        }
    }
    if (pairs) { nx_free(pairs); index->memory_bytes -= token_count * sizeof(*pairs); }
    if (st != NX_OK) return st;

    trigram_pair *triples = index_alloc(index, raw_trigrams, sizeof(*triples), &st);
    n = 0;
    for (uint32_t row = 0; row < rows && st == NX_OK; row++) {
        nx_slice text = f->normalized[row];
        for (size_t j = 0; j + 2 < text.n; j++) {
            triples[n].key = nx_trigram_pack(text.p[j], text.p[j + 1], text.p[j + 2]);
            triples[n++].row = row;
        }
    }
    if (st == NX_OK && n) qsort(triples, n, sizeof(*triples), compare_trigram_pair);
    size_t keys = 0, trigram_postings = 0;
    if (st == NX_OK) for (size_t i = 0; i < n; i++) {
        bool new_key = i == 0 || triples[i - 1].key != triples[i].key;
        if (new_key) keys++;
        if (new_key || triples[i - 1].row != triples[i].row) trigram_postings++;
    }
    f->trigrams = index_alloc(index, keys, sizeof(*f->trigrams), &st);
    f->trigram_rows = index_alloc(index, trigram_postings, sizeof(*f->trigram_rows), &st);
    if (st == NX_OK) {
        uint32_t key = 0, post = 0;
        for (size_t i = 0; i < n; i++) {
            bool new_key = i == 0 || triples[i - 1].key != triples[i].key;
            if (new_key) {
                key = f->trigram_count++;
                f->trigrams[key].key = triples[i].key;
                f->trigrams[key].start = post;
            }
            if (new_key || triples[i - 1].row != triples[i].row) {
                f->trigram_rows[post++] = triples[i].row;
                f->trigrams[key].count++;
            }
        }
    }
    if (triples) { nx_free(triples); index->memory_bytes -= raw_trigrams * sizeof(*triples); }
    return st;
}

nx_status nx_search_index_build(const nx_table *table, nx_search_index **out, nx_error *error) {
    nx_error_clear(error);
    if (out) *out = NULL;
    if (!out || !table || !table->bytes.p || table->rows > 1000000 || table->fields > 256) {
        nx_error_set(error, NX_ERR_INVALID, -1, 0, "Search preparation needs a validated table and output");
        return NX_ERR_INVALID;
    }
    nx_search_index *index = nx_calloc(1, sizeof(*index));
    nx_status st = index ? NX_OK : NX_ERR_NOMEM;
    if (index) {
        index->table = *table; index->memory_bytes = sizeof(*index);
        index->fields = index_alloc(index, table->fields, sizeof(*index->fields), &st);
        size_t text_total = 0, token_total = 0;
        for (uint32_t field = 0; field < table->fields && st == NX_OK; field++) {
            nx_field info; st = nx_table_field(table, field, &info);
            if (st == NX_OK && info.type == NX_FIELD_TEXT)
                st = build_field(index, field, &text_total, &token_total);
        }
    }
    if (st != NX_OK) {
        nx_search_index_free(index);
        nx_error_set(error, st, -1, 0, "Cannot prepare search index: %s", nx_status_str(st));
        return st;
    }
    *out = index; return NX_OK;
}
void nx_search_index_free(nx_search_index *index) {
    if (!index) return;
    if (index->fields) for (uint32_t i = 0; i < index->table.fields; i++) {
        nx_prepared_field *f = &index->fields[i];
        nx_free(f->normalized); nx_free(f->text_bytes); nx_free(f->lengths);
        nx_free(f->terms); nx_free(f->postings); nx_free(f->trigrams); nx_free(f->trigram_rows);
    }
    nx_free(index->fields); nx_free(index);
}
size_t nx_search_index_memory_bytes(const nx_search_index *index) { return index ? index->memory_bytes : 0; }
