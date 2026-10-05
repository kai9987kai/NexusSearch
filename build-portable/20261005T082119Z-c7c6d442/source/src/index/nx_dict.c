#include "nx_dict.h"
#include "core/nx_mem.h"
#include "core/nx_crc32c.h"

#define DICT_MAGIC UINT32_C(0x5443444e)
#define DICT_VERSION 1u
#define DICT_HEADER 32u

static int term_compare(nx_slice a, nx_slice b) {
    size_t n = NX_MIN(a.n, b.n);
    int cmp = n ? memcmp(a.p, b.p, n) : 0;
    return cmp ? cmp : (a.n > b.n) - (a.n < b.n);
}

/* Typed in-place heap sort: deterministic and bounded auxiliary memory. */
static void sift(nx_slice *terms, size_t count, size_t root) {
    while (root < count / 2u) {
        size_t child = root * 2u + 1u;
        if (child + 1u < count && term_compare(terms[child], terms[child + 1u]) < 0) child++;
        if (term_compare(terms[root], terms[child]) >= 0) break;
        nx_slice tmp = terms[root]; terms[root] = terms[child]; terms[child] = tmp;
        root = child;
    }
}

static void sort_terms(nx_slice *terms, size_t count) {
    for (size_t i = count / 2u; i; i--) sift(terms, count, i - 1u);
    for (size_t n = count; n > 1u; n--) {
        nx_slice tmp = terms[0]; terms[0] = terms[n - 1u]; terms[n - 1u] = tmp;
        sift(terms, n - 1u, 0);
    }
}

static uint32_t dict_crc(nx_slice bytes) {
    uint32_t crc = nx_crc32c(0, bytes.p, DICT_HEADER - 4u);
    return nx_crc32c(crc, bytes.p + DICT_HEADER, bytes.n - DICT_HEADER);
}

nx_status nx_dict_build(const nx_slice *terms, size_t count, nx_buf *out) {
    if (!out || (count && !terms)) return NX_ERR_INVALID;
    if (count > NX_DICT_MAX_TERMS) return NX_ERR_LIMIT;
    if (out->oom) return NX_ERR_NOMEM;
    size_t input_bytes = 0;
    for (size_t i = 0; i < count; i++) {
        if (terms[i].n && !terms[i].p) return NX_ERR_INVALID;
        if (terms[i].n > NX_DICT_MAX_TERM_BYTES) return NX_ERR_LIMIT;
        if (nx_add_overflow(input_bytes, terms[i].n, &input_bytes) || input_bytes > NX_DICT_MAX_DATA_BYTES)
            return NX_ERR_LIMIT;
    }
    nx_slice *copy = count ? NX_NEW_ARRAY(nx_slice, count) : NULL;
    if (count && !copy) return NX_ERR_NOMEM;
    if (count) memcpy(copy, terms, count * sizeof *copy);
    sort_terms(copy, count);
    size_t unique = 0, total = 0;
    for (size_t i = 0; i < count; i++) {
        if (unique && nx_slice_eq(copy[unique - 1u], copy[i])) continue;
        if (nx_add_overflow(total, copy[i].n, &total) || total > NX_DICT_MAX_DATA_BYTES) {
            nx_free(copy); return NX_ERR_LIMIT;
        }
        copy[unique++] = copy[i];
    }
    size_t offsets_size, data_offset, size;
    if (nx_mul_overflow(unique + 1u, 4u, &offsets_size) ||
        nx_add_overflow(DICT_HEADER, offsets_size, &data_offset) || nx_add_overflow(data_offset, total, &size)) {
        nx_free(copy); return NX_ERR_LIMIT;
    }
    nx_buf tmp; nx_buf_init(&tmp);
    nx_buf_put_zeros(&tmp, size);
    if (tmp.oom) { nx_buf_free(&tmp); nx_free(copy); return NX_ERR_NOMEM; }
    nx_st32(tmp.data, DICT_MAGIC); nx_st32(tmp.data + 4, DICT_VERSION);
    nx_st32(tmp.data + 8, (uint32_t)unique); nx_st32(tmp.data + 12, (uint32_t)total);
    nx_st32(tmp.data + 16, DICT_HEADER); nx_st32(tmp.data + 24, (uint32_t)data_offset);
    size_t off = 0;
    for (size_t i = 0; i < unique; i++) {
        nx_st32(tmp.data + DICT_HEADER + i * 4u, (uint32_t)off);
        if (copy[i].n) memcpy(tmp.data + data_offset + off, copy[i].p, copy[i].n);
        off += copy[i].n;
    }
    nx_st32(tmp.data + DICT_HEADER + unique * 4u, (uint32_t)off);
    nx_st32(tmp.data + 28, dict_crc(nx_buf_slice(&tmp)));
    nx_status st = nx_buf_reserve(out, size) ? NX_OK : NX_ERR_NOMEM;
    if (st == NX_OK) nx_buf_put(out, tmp.data, size);
    nx_buf_free(&tmp); nx_free(copy);
    return st;
}

nx_status nx_dict_open(nx_slice bytes, nx_dict *out) {
    if (!out) return NX_ERR_INVALID;
    memset(out, 0, sizeof *out);
    if (!bytes.p || bytes.n < DICT_HEADER) return NX_ERR_CORRUPT;
    nx_cursor cur = nx_cursor_make(bytes);
    uint32_t magic = nx_rd_u32(&cur), version = nx_rd_u32(&cur), count = nx_rd_u32(&cur);
    uint32_t total = nx_rd_u32(&cur), header = nx_rd_u32(&cur), reserved = nx_rd_u32(&cur);
    uint32_t data_offset = nx_rd_u32(&cur), crc = nx_rd_u32(&cur);
    if (magic != DICT_MAGIC) return NX_ERR_CORRUPT;
    if (version != DICT_VERSION) return NX_ERR_VERSION;
    size_t offsets_size, expected;
    if (count > NX_DICT_MAX_TERMS || total > NX_DICT_MAX_DATA_BYTES || header != DICT_HEADER || reserved ||
        nx_mul_overflow((size_t)count + 1u, 4u, &offsets_size) ||
        nx_add_overflow(DICT_HEADER, offsets_size, &expected) || expected != data_offset ||
        offsets_size > nx_cursor_left(&cur)) return NX_ERR_CORRUPT;
    nx_slice offsets = nx_rd_bytes(&cur, offsets_size);
    if (total != nx_cursor_left(&cur) || crc != dict_crc(bytes)) return NX_ERR_CORRUPT;
    nx_slice data = nx_rd_bytes(&cur, total), previous = nx_slice_make(NULL, 0);
    uint32_t begin = nx_ld32(offsets.p);
    if (begin) return NX_ERR_CORRUPT;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t end = nx_ld32(offsets.p + ((size_t)i + 1u) * 4u);
        if (end < begin || end > total || end - begin > NX_DICT_MAX_TERM_BYTES) return NX_ERR_CORRUPT;
        nx_slice term = nx_slice_sub(data, begin, end - begin);
        if (i && term_compare(previous, term) >= 0) return NX_ERR_CORRUPT;
        previous = term; begin = end;
    }
    if (cur.err || begin != total) return NX_ERR_CORRUPT;
    nx_dict view = {bytes, count, data_offset};
    *out = view;
    return NX_OK;
}

static bool valid_view(const nx_dict *dict) {
    return dict && dict->bytes.p && dict->count <= NX_DICT_MAX_TERMS &&
           dict->data_offset == DICT_HEADER + ((size_t)dict->count + 1u) * 4u &&
           dict->bytes.n >= dict->data_offset;
}

nx_status nx_dict_term(const nx_dict *dict, uint32_t ordinal, nx_slice *out) {
    if (!out) return NX_ERR_INVALID;
    *out = nx_slice_make(NULL, 0);
    if (!valid_view(dict) || ordinal >= dict->count) return NX_ERR_INVALID;
    size_t begin = nx_ld32(dict->bytes.p + DICT_HEADER + (size_t)ordinal * 4u);
    size_t end = nx_ld32(dict->bytes.p + DICT_HEADER + ((size_t)ordinal + 1u) * 4u);
    if (end < begin || end > dict->bytes.n - dict->data_offset) return NX_ERR_CORRUPT;
    *out = nx_slice_sub(dict->bytes, dict->data_offset + begin, end - begin);
    return NX_OK;
}

static uint32_t lower_bound(const nx_dict *dict, nx_slice term, bool prefix_upper) {
    uint32_t lo = 0, hi = dict->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        nx_slice candidate;
        (void)nx_dict_term(dict, mid, &candidate);
        int cmp = term_compare(candidate, term);
        if (prefix_upper && candidate.n >= term.n && (!term.n || !memcmp(candidate.p, term.p, term.n))) cmp = -1;
        if (cmp < 0) lo = mid + 1u; else hi = mid;
    }
    return lo;
}

nx_status nx_dict_lookup(const nx_dict *dict, nx_slice term, uint32_t *ordinal) {
    if (!ordinal) return NX_ERR_INVALID;
    *ordinal = UINT32_MAX;
    if (!valid_view(dict) || (term.n && !term.p)) return NX_ERR_INVALID;
    if (term.n > NX_DICT_MAX_TERM_BYTES) return NX_ERR_LIMIT;
    uint32_t i = lower_bound(dict, term, false);
    nx_slice candidate;
    if (i == dict->count || nx_dict_term(dict, i, &candidate) != NX_OK || !nx_slice_eq(candidate, term))
        return NX_ERR_NOT_FOUND;
    *ordinal = i;
    return NX_OK;
}

nx_status nx_dict_prefix_range(const nx_dict *dict, nx_slice prefix, uint32_t max_expansion,
                              uint32_t *begin, uint32_t *end) {
    if (begin) *begin = 0;
    if (end) *end = 0;
    if (!begin || !end || !valid_view(dict) || (prefix.n && !prefix.p)) return NX_ERR_INVALID;
    if (prefix.n > NX_DICT_MAX_TERM_BYTES) return NX_ERR_LIMIT;
    uint32_t first = lower_bound(dict, prefix, false), last = lower_bound(dict, prefix, true);
    if (last - first > max_expansion) return NX_ERR_LIMIT;
    *begin = first; *end = last;
    return NX_OK;
}
