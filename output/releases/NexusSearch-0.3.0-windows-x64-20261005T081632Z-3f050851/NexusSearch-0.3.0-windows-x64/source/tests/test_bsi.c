#include "nx_test.h"
#include "index/nx_bsi.h"
#include "core/nx_crc32c.h"

static bool compare_value(int64_t a, nx_compare op, int64_t b) {
    switch (op) {
        case NX_CMP_EQ: return a == b;
        case NX_CMP_NE: return a != b;
        case NX_CMP_LT: return a < b;
        case NX_CMP_LE: return a <= b;
        case NX_CMP_GT: return a > b;
        case NX_CMP_GE: return a >= b;
    }
    return false;
}

static void compare_index(const nx_bsi *index, const int64_t *values, const uint8_t *present, int64_t key) {
    for (int op = NX_CMP_EQ; op <= NX_CMP_GE; op++) {
        nx_buf result; nx_buf_init(&result);
        nx_buf_put_u8(&result, 0x5a);
        NX_CHECK_OK(nx_bsi_filter(index, (nx_compare)op, key, &result));
        NX_CHECK_EQ_U(result.data[0], 0x5a);
        nx_bitmap b;
        NX_CHECK_OK(nx_bitmap_open(nx_slice_sub(nx_buf_slice(&result), 1, result.len - 1u), &b));
        uint64_t count = 0;
        for (uint32_t row = 0; row < index->rows; row++) {
            bool expected = (!present || present[row]) && compare_value(values[row], (nx_compare)op, key);
            NX_CHECK(nx_bitmap_contains(&b, row) == expected);
            count += expected ? 1u : 0u;
        }
        NX_CHECK(!nx_bitmap_contains(&b, index->rows));
        NX_CHECK_EQ_U(b.cardinality, count);
        nx_buf_free(&result);
    }
}

static void test_bsi_extremes_holes_empty(void) {
    int64_t values[] = {INT64_MIN, INT64_MIN + 1, -1, 0, 1, INT64_MAX - 1, INT64_MAX, 1999};
    uint8_t present[] = {1, 1, 1, 0, 1, 1, 1, 0};
    for (size_t n = 0; n <= NX_ARRAY_LEN(values); n++) {
        for (size_t holes = 0; holes < 2; holes++) {
            nx_buf bytes; nx_buf_init(&bytes);
            NX_CHECK_OK(nx_bsi_build(values, holes ? present : NULL, n, &bytes));
            nx_bsi index;
            nx_mem_fail_after(0);
            nx_status st = nx_bsi_open(nx_buf_slice(&bytes), &index);
            nx_mem_fail_after(-1);
            NX_CHECK_OK(st);
            NX_CHECK_EQ_U(index.rows, n);
            for (size_t row = 0; row < n; row++) {
                int64_t got = 88; bool has = false;
                NX_CHECK_OK(nx_bsi_get(&index, (uint32_t)row, &got, &has));
                NX_CHECK(has == (!holes || present[row] != 0));
                NX_CHECK_EQ_I(got, has ? values[row] : 0);
            }
            for (size_t i = 0; i < NX_ARRAY_LEN(values); i++)
                compare_index(&index, values, holes ? present : NULL, values[i]);
            nx_buf_free(&bytes);
        }
    }
    nx_buf out; nx_buf_init(&out);
    NX_CHECK(nx_bsi_build(NULL, NULL, 1, &out) == NX_ERR_INVALID);
    NX_CHECK(nx_bsi_build(values, NULL, (size_t)NX_BSI_MAX_ROWS + 1u, &out) == NX_ERR_LIMIT);
    uint8_t invalid[] = {2};
    NX_CHECK(nx_bsi_build(values, invalid, 1, &out) == NX_ERR_INVALID);
    NX_CHECK_EQ_U(out.len, 0);
    nx_bsi empty;
    NX_CHECK_OK(nx_bsi_build(NULL, NULL, 0, &out));
    NX_CHECK_OK(nx_bsi_open(nx_buf_slice(&out), &empty));
    int64_t got = 88; bool has = true;
    NX_CHECK(nx_bsi_get(&empty, 0, &got, &has) == NX_ERR_INVALID);
    NX_CHECK_EQ_I(got, 0); NX_CHECK(!has);
    nx_buf_free(&out);
}

static void test_bsi_random_oracle(void) {
    nx_rng rng = nx_test_rng(880);
    enum { N = 769 };
    int64_t values[N]; uint8_t present[N];
    for (size_t iter = 0; iter < nx_test_iters(50); iter++) {
        size_t n = (size_t)(nx_rng_u64(&rng) % N);
        for (size_t i = 0; i < n; i++) {
            uint64_t v = nx_rng_u64(&rng);
            memcpy(&values[i], &v, sizeof v);
            if (i % 3u == 0) values[i] = (int64_t)(nx_rng_u64(&rng) % 20u) - 10;
            present[i] = (uint8_t)(nx_rng_u64(&rng) % 5u != 0);
        }
        nx_buf bytes; nx_buf_init(&bytes);
        NX_CHECK_OK(nx_bsi_build(values, present, n, &bytes));
        nx_bsi index; NX_CHECK_OK(nx_bsi_open(nx_buf_slice(&bytes), &index));
        for (size_t i = 0; i < n; i++) {
            int64_t got = 0; bool has = false;
            NX_CHECK_OK(nx_bsi_get(&index, (uint32_t)i, &got, &has));
            NX_CHECK(has == (present[i] != 0));
            NX_CHECK_EQ_I(got, has ? values[i] : 0);
        }
        const int64_t keys[] = {INT64_MIN, -10, -1, 0, 1, 9, INT64_MAX};
        for (size_t i = 0; i < NX_ARRAY_LEN(keys); i++) compare_index(&index, values, present, keys[i]);
        if (n) compare_index(&index, values, present, values[n / 2u]);
        nx_buf_free(&bytes);
    }
}

static void test_bsi_roaring_boundaries(void) {
    enum { N = 131101 };
    int64_t *values = NX_NEW_ARRAY(int64_t, N);
    uint8_t *present = NX_NEW_ARRAY(uint8_t, N);
    NX_REQUIRE(values && present);
    for (size_t i = 0; i < N; i++) { values[i] = (int64_t)(i % 7u) - 3; present[i] = (uint8_t)(i % 13u != 0); }
    nx_buf bytes; nx_buf_init(&bytes);
    NX_CHECK_OK(nx_bsi_build(values, present, N, &bytes));
    nx_bsi index; NX_CHECK_OK(nx_bsi_open(nx_buf_slice(&bytes), &index));
    compare_index(&index, values, present, 0);
    compare_index(&index, values, present, INT64_MIN);
    compare_index(&index, values, present, INT64_MAX);
    nx_buf_free(&bytes); nx_free(values); nx_free(present);
}

static void patch_crc(nx_buf *bytes) {
    uint32_t crc = nx_crc32c(0, bytes->data, 28);
    crc = nx_crc32c(crc, bytes->data + 32, bytes->len - 32u);
    nx_st32(bytes->data + 28, crc);
}

static void test_bsi_corruption(void) {
    int64_t values[] = {INT64_MIN, 0, INT64_MAX};
    uint8_t present[] = {1, 0, 1};
    nx_buf bytes; nx_buf_init(&bytes);
    NX_CHECK_OK(nx_bsi_build(values, present, 3, &bytes));
    nx_bsi index;
    for (size_t n = 0; n < bytes.len; n++) {
        NX_CHECK(nx_bsi_open(nx_slice_make(bytes.data, n), &index) != NX_OK);
        NX_CHECK(index.bytes.p == NULL);
    }
    nx_rng rng = nx_test_rng(881);
    for (size_t i = 0; i < nx_test_iters(1000); i++) {
        size_t off = (size_t)(nx_rng_u64(&rng) % bytes.len);
        uint8_t old = bytes.data[off]; bytes.data[off] ^= (uint8_t)(1u << (nx_rng_u64(&rng) % 8u));
        NX_CHECK(nx_bsi_open(nx_buf_slice(&bytes), &index) != NX_OK);
        bytes.data[off] = old;
    }
    nx_buf mutated; nx_buf_init(&mutated);
    for (size_t i = 0; i < nx_test_iters(150); i++) {
        nx_buf_clear(&mutated); nx_buf_put(&mutated, bytes.data, bytes.len);
        size_t destination = (size_t)(nx_rng_u64(&rng) % (bytes.len - 8u));
        size_t source = (size_t)(nx_rng_u64(&rng) % (bytes.len - 8u));
        memcpy(mutated.data + destination, bytes.data + source, 8);
        patch_crc(&mutated);
        nx_status st = nx_bsi_open(nx_buf_slice(&mutated), &index);
        if (st == NX_OK) {
            /* This fixed-length fixture can hold at most one 64-row word. */
            NX_CHECK(index.rows <= 64);
            int64_t recovered[64]; uint8_t has[64];
            for (uint32_t row = 0; row < index.rows; row++) {
                bool exists = false;
                NX_CHECK_OK(nx_bsi_get(&index, row, &recovered[row], &exists));
                has[row] = exists ? 1u : 0u;
            }
            compare_index(&index, recovered, has, 0);
        }
    }
    nx_buf_free(&mutated);
    /* Recompute checksum so the canonical bit constraints, not only CRC,
     * reject missing-row value bits and out-of-universe tail bits. */
    bytes.data[40] |= 2u; patch_crc(&bytes);
    NX_CHECK(nx_bsi_open(nx_buf_slice(&bytes), &index) == NX_ERR_CORRUPT);
    bytes.data[40] &= (uint8_t)~2u; patch_crc(&bytes);
    bytes.data[32] |= 128u; patch_crc(&bytes);
    NX_CHECK(nx_bsi_open(nx_buf_slice(&bytes), &index) == NX_ERR_CORRUPT);
    bytes.data[32] &= (uint8_t)~128u; patch_crc(&bytes);
    NX_CHECK_OK(nx_bsi_open(nx_buf_slice(&bytes), &index));
    nx_buf_put_u8(&bytes, 0);
    NX_CHECK(nx_bsi_open(nx_buf_slice(&bytes), &index) == NX_ERR_CORRUPT);
    nx_buf_free(&bytes);
}

static nx_status bsi_oom(void *ctx) {
    (void)ctx;
    const int64_t values[] = {INT64_MIN, 4, 8, -9, INT64_MAX};
    nx_buf bytes, result; nx_buf_init(&bytes); nx_buf_init(&result);
    nx_status st = nx_bsi_build(values, NULL, 5, &bytes);
    nx_bsi index;
    if (st == NX_OK) st = nx_bsi_open(nx_buf_slice(&bytes), &index);
    if (st == NX_OK) st = nx_bsi_filter(&index, NX_CMP_GE, -9, &result);
    nx_bitmap b;
    if (st == NX_OK) st = nx_bitmap_open(nx_buf_slice(&result), &b);
    nx_buf_free(&bytes); nx_buf_free(&result);
    return st;
}

static void test_bsi_oom_transaction(void) {
    nx_test_oom_sweep(bsi_oom, NULL, 100);
    int64_t values[70];
    for (size_t i = 0; i < NX_ARRAY_LEN(values); i++) values[i] = (int64_t)i;
    for (int fail = 0; fail < 12; fail++) {
        nx_buf out; nx_buf_init(&out); nx_buf_put_str(&out, "prefix");
        nx_mem_fail_after(fail);
        nx_status st = nx_bsi_build(values, NULL, NX_ARRAY_LEN(values), &out);
        nx_mem_fail_after(-1);
        if (st != NX_OK) { NX_CHECK(st == NX_ERR_NOMEM); NX_CHECK_EQ_U(out.len, 6); }
        NX_CHECK(memcmp(out.data, "prefix", 6) == 0);
        nx_buf_free(&out);
    }
    nx_buf bytes; nx_buf_init(&bytes);
    NX_CHECK_OK(nx_bsi_build(values, NULL, NX_ARRAY_LEN(values), &bytes));
    nx_bsi index; NX_CHECK_OK(nx_bsi_open(nx_buf_slice(&bytes), &index));
    for (int fail = 0; fail < 12; fail++) {
        nx_buf out; nx_buf_init(&out); nx_buf_put_str(&out, "prefix");
        nx_mem_fail_after(fail);
        nx_status st = nx_bsi_filter(&index, NX_CMP_GE, INT64_MIN, &out);
        nx_mem_fail_after(-1);
        if (st != NX_OK) { NX_CHECK(st == NX_ERR_NOMEM); NX_CHECK_EQ_U(out.len, 6); }
        NX_CHECK(memcmp(out.data, "prefix", 6) == 0);
        nx_buf_free(&out);
    }
    nx_buf_free(&bytes);
}

typedef struct reader_context { const nx_bsi *index; bool ok; } reader_context;
static void concurrent_read(void *ctx) {
    reader_context *c = (reader_context *)ctx; c->ok = true;
    for (size_t i = 0; i < 1000; i++) {
        uint32_t row = (uint32_t)(i % c->index->rows);
        int64_t value; bool present;
        if (nx_bsi_get(c->index, row, &value, &present) != NX_OK || !present || value != (int64_t)row - 10)
            c->ok = false;
    }
}
static void test_bsi_concurrent_reads(void) {
    int64_t values[32]; for (size_t i = 0; i < 32; i++) values[i] = (int64_t)i - 10;
    nx_buf bytes; nx_buf_init(&bytes);
    NX_CHECK_OK(nx_bsi_build(values, NULL, 32, &bytes));
    nx_bsi index; NX_CHECK_OK(nx_bsi_open(nx_buf_slice(&bytes), &index));
    nx_thread threads[4]; reader_context ctx[4]; bool started[4];
    for (size_t i = 0; i < 4; i++) {
        ctx[i].index = &index; ctx[i].ok = false;
        started[i] = nx_thread_start(&threads[i], concurrent_read, &ctx[i]); NX_CHECK(started[i]);
    }
    for (size_t i = 0; i < 4; i++) if (started[i]) { nx_thread_join(&threads[i]); NX_CHECK(ctx[i].ok); }
    nx_buf_free(&bytes);
}

int main(void) {
    NX_RUN(test_bsi_extremes_holes_empty);
    NX_RUN(test_bsi_random_oracle);
    NX_RUN(test_bsi_roaring_boundaries);
    NX_RUN(test_bsi_corruption);
    NX_RUN(test_bsi_oom_transaction);
    NX_RUN(test_bsi_concurrent_reads);
    return nx_test_summary();
}
