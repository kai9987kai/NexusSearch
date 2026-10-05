#include "nx_test.h"
#include "index/nx_dict.h"
#include "core/nx_crc32c.h"

static int oracle_compare(nx_slice a, nx_slice b) {
    size_t n = a.n < b.n ? a.n : b.n;
    for (size_t i = 0; i < n; i++) {
        if (a.p[i] < b.p[i]) return -1;
        if (a.p[i] > b.p[i]) return 1;
    }
    return (a.n > b.n) - (a.n < b.n);
}

static size_t oracle_sort(nx_slice *terms, size_t count) {
    for (size_t i = 1; i < count; i++) {
        nx_slice term = terms[i]; size_t j = i;
        while (j && oracle_compare(term, terms[j - 1u]) < 0) { terms[j] = terms[j - 1u]; j--; }
        terms[j] = term;
    }
    size_t unique = 0;
    for (size_t i = 0; i < count; i++)
        if (!unique || oracle_compare(terms[i], terms[unique - 1u])) terms[unique++] = terms[i];
    return unique;
}

static bool starts_with(nx_slice term, nx_slice prefix) {
    return term.n >= prefix.n && (!prefix.n || memcmp(term.p, prefix.p, prefix.n) == 0);
}

static void check_prefix(const nx_dict *dict, const nx_slice *sorted, size_t count, nx_slice prefix) {
    size_t first = 0, last;
    while (first < count && oracle_compare(sorted[first], prefix) < 0) first++;
    last = first;
    while (last < count && starts_with(sorted[last], prefix)) last++;
    uint32_t begin = UINT32_MAX, end = UINT32_MAX;
    NX_CHECK_OK(nx_dict_prefix_range(dict, prefix, (uint32_t)count, &begin, &end));
    NX_CHECK_EQ_U(begin, first); NX_CHECK_EQ_U(end, last);
    if (first < last) {
        NX_CHECK(nx_dict_prefix_range(dict, prefix, (uint32_t)(last - first - 1u), &begin, &end) == NX_ERR_LIMIT);
        NX_CHECK_EQ_U(begin, 0); NX_CHECK_EQ_U(end, 0);
        NX_CHECK_OK(nx_dict_prefix_range(dict, prefix, (uint32_t)(last - first), &begin, &end));
        NX_CHECK_EQ_U(begin, first); NX_CHECK_EQ_U(end, last);
    } else NX_CHECK_OK(nx_dict_prefix_range(dict, prefix, 0, &begin, &end));
}

static void test_dict_edges_binary(void) {
    static const uint8_t binary[] = {'a', 0, 'b'}, high[] = {255, 255}, nul[] = {0};
    nx_slice terms[] = {nx_slice_cstr("z"), nx_slice_make(binary, sizeof binary), nx_slice_cstr("alpha"),
                       nx_slice_make(NULL, 0), nx_slice_cstr("alpha"), nx_slice_cstr("a"),
                       nx_slice_make(high, sizeof high), nx_slice_make(nul, sizeof nul)};
    nx_slice sorted[NX_ARRAY_LEN(terms)]; memcpy(sorted, terms, sizeof terms);
    size_t unique = oracle_sort(sorted, NX_ARRAY_LEN(sorted));
    nx_buf bytes; nx_buf_init(&bytes);
    nx_buf_put_str(&bytes, "prefix");
    NX_CHECK_OK(nx_dict_build(terms, NX_ARRAY_LEN(terms), &bytes));
    NX_CHECK(memcmp(bytes.data, "prefix", 6) == 0);
    nx_dict dict;
    nx_mem_fail_after(0);
    nx_status st = nx_dict_open(nx_slice_sub(nx_buf_slice(&bytes), 6, bytes.len - 6u), &dict);
    nx_mem_fail_after(-1);
    NX_CHECK_OK(st); NX_CHECK_EQ_U(dict.count, unique);
    for (uint32_t i = 0; i < dict.count; i++) {
        nx_slice term; uint32_t ordinal = UINT32_MAX;
        NX_CHECK_OK(nx_dict_term(&dict, i, &term)); NX_CHECK(nx_slice_eq(term, sorted[i]));
        NX_CHECK_OK(nx_dict_lookup(&dict, term, &ordinal)); NX_CHECK_EQ_U(ordinal, i);
        for (size_t n = 0; n <= term.n; n++) check_prefix(&dict, sorted, unique, nx_slice_make(term.p, n));
    }
    nx_slice missing = nx_slice_cstr("alpine"); uint32_t ordinal = 0;
    NX_CHECK(nx_dict_lookup(&dict, missing, &ordinal) == NX_ERR_NOT_FOUND);
    NX_CHECK_EQ_U(ordinal, UINT32_MAX);
    check_prefix(&dict, sorted, unique, missing);
    nx_slice invalid = nx_slice_make(NULL, 1), term;
    NX_CHECK(nx_dict_lookup(&dict, invalid, &ordinal) == NX_ERR_INVALID);
    NX_CHECK(nx_dict_term(&dict, dict.count, &term) == NX_ERR_INVALID); NX_CHECK(term.p == NULL);
    nx_buf_free(&bytes);

    nx_buf_init(&bytes);
    NX_CHECK_OK(nx_dict_build(NULL, 0, &bytes));
    NX_CHECK_OK(nx_dict_open(nx_buf_slice(&bytes), &dict)); NX_CHECK_EQ_U(dict.count, 0);
    check_prefix(&dict, NULL, 0, nx_slice_make(NULL, 0));
    NX_CHECK(nx_dict_lookup(&dict, nx_slice_cstr(""), &ordinal) == NX_ERR_NOT_FOUND);
    NX_CHECK(nx_dict_build(NULL, 1, &bytes) == NX_ERR_INVALID);
    NX_CHECK(nx_dict_build(terms, (size_t)NX_DICT_MAX_TERMS + 1u, &bytes) == NX_ERR_LIMIT);
    NX_CHECK(nx_dict_build(&invalid, 1, &bytes) == NX_ERR_INVALID);
    nx_slice too_large = nx_slice_make("a", (size_t)NX_DICT_MAX_TERM_BYTES + 1u);
    NX_CHECK(nx_dict_build(&too_large, 1, &bytes) == NX_ERR_LIMIT);
    nx_slice *repeated = NX_NEW_ARRAY(nx_slice, 1025);
    NX_REQUIRE(repeated);
    /* Build validates lengths before dereferencing oversized aggregate data. */
    for (size_t i = 0; i < 1025; i++) repeated[i] = nx_slice_make("a", NX_DICT_MAX_TERM_BYTES);
    NX_CHECK(nx_dict_build(repeated, 1025, &bytes) == NX_ERR_LIMIT);
    nx_free(repeated);
    nx_buf_free(&bytes);
}

static void test_dict_random_oracle(void) {
    enum { N = 211, MAX_BYTES = 16 };
    nx_rng rng = nx_test_rng(882);
    uint8_t text[N][MAX_BYTES]; nx_slice terms[N], sorted[N];
    for (size_t iter = 0; iter < nx_test_iters(80); iter++) {
        size_t count = (size_t)(nx_rng_u64(&rng) % N);
        for (size_t i = 0; i < count; i++) {
            size_t n = (size_t)(nx_rng_u64(&rng) % (MAX_BYTES + 1u));
            for (size_t j = 0; j < n; j++) text[i][j] = (uint8_t)(nx_rng_u64(&rng) % (iter % 2u ? 4u : 256u));
            terms[i] = nx_slice_make(text[i], n);
            if (i && i % 7u == 0) terms[i] = terms[i - 1u];
        }
        memcpy(sorted, terms, count * sizeof *terms);
        size_t unique = oracle_sort(sorted, count);
        nx_buf bytes; nx_buf_init(&bytes);
        NX_CHECK_OK(nx_dict_build(terms, count, &bytes));
        nx_dict dict; NX_CHECK_OK(nx_dict_open(nx_buf_slice(&bytes), &dict));
        NX_CHECK_EQ_U(dict.count, unique);
        for (uint32_t i = 0; i < dict.count; i++) {
            nx_slice term; uint32_t ordinal = UINT32_MAX;
            NX_CHECK_OK(nx_dict_term(&dict, i, &term)); NX_CHECK(nx_slice_eq(term, sorted[i]));
            NX_CHECK_OK(nx_dict_lookup(&dict, term, &ordinal)); NX_CHECK_EQ_U(ordinal, i);
            for (size_t n = 0; n <= term.n; n++) check_prefix(&dict, sorted, unique, nx_slice_make(term.p, n));
        }
        /* Permuting the same multiset must produce byte-identical output. */
        for (size_t i = count; i > 1u; i--) {
            size_t j = (size_t)(nx_rng_u64(&rng) % i);
            nx_slice tmp = terms[j]; terms[j] = terms[i - 1u]; terms[i - 1u] = tmp;
        }
        nx_buf permuted; nx_buf_init(&permuted);
        NX_CHECK_OK(nx_dict_build(terms, count, &permuted));
        NX_CHECK(nx_slice_eq(nx_buf_slice(&bytes), nx_buf_slice(&permuted)));
        nx_buf_free(&permuted); nx_buf_free(&bytes);
    }
}

static void patch_crc(nx_buf *bytes) {
    uint32_t crc = nx_crc32c(0, bytes->data, 28);
    crc = nx_crc32c(crc, bytes->data + 32, bytes->len - 32u);
    nx_st32(bytes->data + 28, crc);
}

static void test_dict_corruption(void) {
    nx_slice terms[] = {nx_slice_cstr("aa"), nx_slice_cstr("bb"), nx_slice_cstr("cc")};
    nx_buf bytes; nx_buf_init(&bytes);
    NX_CHECK_OK(nx_dict_build(terms, NX_ARRAY_LEN(terms), &bytes));
    nx_dict dict;
    for (size_t n = 0; n < bytes.len; n++) {
        NX_CHECK(nx_dict_open(nx_slice_make(bytes.data, n), &dict) != NX_OK); NX_CHECK(dict.bytes.p == NULL);
    }
    nx_rng rng = nx_test_rng(883);
    for (size_t i = 0; i < nx_test_iters(1000); i++) {
        size_t off = (size_t)(nx_rng_u64(&rng) % bytes.len);
        uint8_t old = bytes.data[off]; bytes.data[off] ^= (uint8_t)(1u << (nx_rng_u64(&rng) % 8u));
        NX_CHECK(nx_dict_open(nx_buf_slice(&bytes), &dict) != NX_OK);
        bytes.data[off] = old;
    }
    nx_buf mutated; nx_buf_init(&mutated);
    for (size_t i = 0; i < nx_test_iters(300); i++) {
        nx_buf_clear(&mutated); nx_buf_put(&mutated, bytes.data, bytes.len);
        size_t destination = (size_t)(nx_rng_u64(&rng) % (bytes.len - 4u));
        size_t source = (size_t)(nx_rng_u64(&rng) % (bytes.len - 4u));
        memcpy(mutated.data + destination, bytes.data + source, 4);
        patch_crc(&mutated);
        nx_status st = nx_dict_open(nx_buf_slice(&mutated), &dict);
        if (st == NX_OK) {
            nx_slice previous = nx_slice_make(NULL, 0);
            for (uint32_t ordinal = 0; ordinal < dict.count; ordinal++) {
                nx_slice term; uint32_t found = UINT32_MAX;
                NX_CHECK_OK(nx_dict_term(&dict, ordinal, &term));
                NX_CHECK(!ordinal || oracle_compare(previous, term) < 0);
                NX_CHECK_OK(nx_dict_lookup(&dict, term, &found)); NX_CHECK_EQ_U(found, ordinal);
                previous = term;
            }
        }
    }
    nx_buf_free(&mutated);
    nx_st32(bytes.data + 32, 1); patch_crc(&bytes);
    NX_CHECK(nx_dict_open(nx_buf_slice(&bytes), &dict) == NX_ERR_CORRUPT);
    nx_st32(bytes.data + 32, 0);
    nx_st32(bytes.data + 36, 7); patch_crc(&bytes);
    NX_CHECK(nx_dict_open(nx_buf_slice(&bytes), &dict) == NX_ERR_CORRUPT);
    nx_st32(bytes.data + 36, 2);
    bytes.data[50] = 'a'; bytes.data[51] = 'a'; patch_crc(&bytes);
    NX_CHECK(nx_dict_open(nx_buf_slice(&bytes), &dict) == NX_ERR_CORRUPT);
    bytes.data[50] = 'b'; bytes.data[51] = 'b'; patch_crc(&bytes);
    NX_CHECK_OK(nx_dict_open(nx_buf_slice(&bytes), &dict));
    nx_buf_put_u8(&bytes, 0);
    NX_CHECK(nx_dict_open(nx_buf_slice(&bytes), &dict) == NX_ERR_CORRUPT);
    nx_buf_free(&bytes);
}

static nx_status dict_oom(void *ctx) {
    (void)ctx;
    nx_slice terms[] = {nx_slice_cstr("one"), nx_slice_cstr("two"), nx_slice_cstr("three")};
    nx_buf bytes; nx_buf_init(&bytes);
    nx_status st = nx_dict_build(terms, NX_ARRAY_LEN(terms), &bytes);
    nx_dict dict;
    if (st == NX_OK) st = nx_dict_open(nx_buf_slice(&bytes), &dict);
    uint32_t ordinal;
    if (st == NX_OK) st = nx_dict_lookup(&dict, terms[0], &ordinal);
    uint32_t begin, end;
    if (st == NX_OK) st = nx_dict_prefix_range(&dict, nx_slice_cstr("t"), 3, &begin, &end);
    nx_buf_free(&bytes);
    return st;
}

static void test_dict_oom_transaction(void) {
    nx_test_oom_sweep(dict_oom, NULL, 100);
    uint8_t large[2048]; memset(large, 'x', sizeof large);
    nx_slice terms[] = {nx_slice_cstr("alpha"), nx_slice_make(large, sizeof large)};
    for (int fail = 0; fail < 8; fail++) {
        nx_buf out; nx_buf_init(&out); nx_buf_put_str(&out, "prefix");
        nx_mem_fail_after(fail);
        nx_status st = nx_dict_build(terms, NX_ARRAY_LEN(terms), &out);
        nx_mem_fail_after(-1);
        if (st != NX_OK) { NX_CHECK(st == NX_ERR_NOMEM); NX_CHECK_EQ_U(out.len, 6); }
        NX_CHECK(memcmp(out.data, "prefix", 6) == 0);
        nx_buf_free(&out);
    }
}

typedef struct reader_context { const nx_dict *dict; bool ok; } reader_context;
static void concurrent_read(void *ctx) {
    reader_context *c = (reader_context *)ctx; c->ok = true;
    for (size_t i = 0; i < 1000; i++) {
        uint32_t ordinal = (uint32_t)(i % c->dict->count), found = UINT32_MAX;
        nx_slice term;
        if (nx_dict_term(c->dict, ordinal, &term) != NX_OK ||
            nx_dict_lookup(c->dict, term, &found) != NX_OK || found != ordinal) c->ok = false;
    }
}
static void test_dict_concurrent_reads(void) {
    nx_slice terms[] = {nx_slice_cstr("first"), nx_slice_cstr("second"), nx_slice_cstr("third")};
    nx_buf bytes; nx_buf_init(&bytes);
    NX_CHECK_OK(nx_dict_build(terms, 3, &bytes));
    nx_dict dict; NX_CHECK_OK(nx_dict_open(nx_buf_slice(&bytes), &dict));
    nx_thread threads[4]; reader_context ctx[4]; bool started[4];
    for (size_t i = 0; i < 4; i++) {
        ctx[i].dict = &dict; ctx[i].ok = false;
        started[i] = nx_thread_start(&threads[i], concurrent_read, &ctx[i]); NX_CHECK(started[i]);
    }
    for (size_t i = 0; i < 4; i++) if (started[i]) { nx_thread_join(&threads[i]); NX_CHECK(ctx[i].ok); }
    nx_buf_free(&bytes);
}

int main(void) {
    NX_RUN(test_dict_edges_binary);
    NX_RUN(test_dict_random_oracle);
    NX_RUN(test_dict_corruption);
    NX_RUN(test_dict_oom_transaction);
    NX_RUN(test_dict_concurrent_reads);
    return nx_test_summary();
}
