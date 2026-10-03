#include "nx_test.h"
#include "index/nx_bitmap.h"

static void test_bitmap_boundaries(void) {
    const uint32_t ids[] = {UINT32_MAX, 65536, 0, 65535, 0, 2};
    const uint32_t sorted[] = {0, 2, 65535, 65536, UINT32_MAX};
    nx_buf bytes; nx_buf_init(&bytes);
    nx_bitmap b;
    NX_CHECK_OK(nx_bitmap_build(ids, 6, &bytes));
    NX_CHECK_OK(nx_bitmap_open(nx_buf_slice(&bytes), &b));
    NX_CHECK_EQ_U(b.cardinality, 5);
    nx_bitmap_iter it; nx_bitmap_iter_init(&b, &it);
    for (size_t i = 0; i < 5; i++) {
        uint32_t id = 1;
        NX_CHECK(nx_bitmap_next(&it, &id));
        NX_CHECK_EQ_U(id, sorted[i]);
        NX_CHECK(nx_bitmap_contains(&b, sorted[i]));
    }
    uint32_t ignored;
    NX_CHECK(!nx_bitmap_next(&it, &ignored));
    NX_CHECK(!nx_bitmap_contains(&b, 1));
    nx_buf_free(&bytes);
}

static void test_bitmap_set_oracle(void) {
    enum { N = 131100 };
    uint32_t *a = NX_NEW_ARRAY(uint32_t, N), *b = NX_NEW_ARRAY(uint32_t, N);
    NX_REQUIRE(a && b);
    size_t na = 0, nb = 0;
    for (uint32_t i = 0; i < N; i++) {
        if (i % 3 != 0) a[na++] = i;
        if (i % 5 == 0) b[nb++] = i;
    }
    nx_buf ba, bb; nx_buf_init(&ba); nx_buf_init(&bb);
    NX_CHECK_OK(nx_bitmap_build(a, na, &ba));
    NX_CHECK_OK(nx_bitmap_build(b, nb, &bb));
    nx_bitmap va, vb;
    NX_CHECK_OK(nx_bitmap_open(nx_buf_slice(&ba), &va));
    NX_CHECK_OK(nx_bitmap_open(nx_buf_slice(&bb), &vb));
    for (int op = NX_BITMAP_AND; op <= NX_BITMAP_ANDNOT; op++) {
        nx_buf out; nx_buf_init(&out);
        NX_CHECK_OK(nx_bitmap_combine(&va, &vb, (nx_bitmap_op)op, &out));
        nx_bitmap v;
        NX_CHECK_OK(nx_bitmap_open(nx_buf_slice(&out), &v));
        nx_bitmap_iter it; nx_bitmap_iter_init(&v, &it);
        uint64_t count = 0;
        for (uint32_t i = 0; i < N; i++) {
            bool x = i % 3 != 0, y = i % 5 == 0;
            bool expected = op == NX_BITMAP_AND ? x && y : op == NX_BITMAP_OR ? x || y :
                            op == NX_BITMAP_XOR ? x != y : x && !y;
            NX_CHECK(nx_bitmap_contains(&v, i) == expected);
            if (expected) {
                uint32_t got = 0;
                NX_CHECK(nx_bitmap_next(&it, &got));
                NX_CHECK_EQ_U(got, i);
                count++;
            }
        }
        NX_CHECK_EQ_U(v.cardinality, count);
        uint32_t got;
        NX_CHECK(!nx_bitmap_next(&it, &got));
        nx_buf_free(&out);
    }
    nx_buf_free(&ba); nx_buf_free(&bb); nx_free(a); nx_free(b);
}

static void test_bitmap_random_corruption(void) {
    nx_rng rng = nx_test_rng(12);
    uint32_t ids[500];
    for (size_t i = 0; i < 500; i++) ids[i] = (uint32_t)nx_rng_u64(&rng);
    nx_buf bytes; nx_buf_init(&bytes);
    NX_CHECK_OK(nx_bitmap_build(ids, 500, &bytes));
    nx_bitmap view;
    for (size_t n = 0; n < bytes.len; n++) {
        NX_CHECK(nx_bitmap_open(nx_slice_make(bytes.data, n), &view) != NX_OK);
        NX_CHECK_EQ_U(view.cardinality, 0);
    }
    for (size_t n = 0; n < nx_test_iters(1000); n++) {
        size_t where = (size_t)(nx_rng_u64(&rng) % bytes.len);
        uint8_t old = bytes.data[where];
        bytes.data[where] ^= (uint8_t)(1u << (nx_rng_u64(&rng) % 8));
        nx_status st = nx_bitmap_open(nx_buf_slice(&bytes), &view);
        if (st == NX_OK) {
            nx_bitmap_iter it; nx_bitmap_iter_init(&view, &it);
            uint64_t count = 0;
            uint32_t id = 0, previous = 0;
            while (nx_bitmap_next(&it, &id)) {
                NX_CHECK(count == 0 || id > previous);
                NX_CHECK(nx_bitmap_contains(&view, id));
                previous = id; count++;
            }
            NX_CHECK_EQ_U(count, view.cardinality);
        }
        bytes.data[where] = old;
    }
    /* Portable golden vector: keys 0 and 1, values [1,2] and [3]. */
    const uint8_t golden[] = {58,48,0,0,2,0,0,0,0,0,1,0,1,0,0,0,
                             24,0,0,0,28,0,0,0,1,0,2,0,3,0};
    NX_CHECK_OK(nx_bitmap_open(nx_slice_make(golden, sizeof golden), &view));
    NX_CHECK(nx_bitmap_contains(&view, 65539));
    uint32_t values[] = {1,2,65539};
    nx_buf_clear(&bytes);
    NX_CHECK_OK(nx_bitmap_build(values, 3, &bytes));
    NX_CHECK(nx_slice_eq(nx_buf_slice(&bytes), nx_slice_make(golden, sizeof golden)));
    nx_buf_free(&bytes);
}

static nx_status bitmap_oom(void *ctx) {
    (void)ctx;
    uint32_t ids[] = {1,2,3,4,65536,65540,UINT32_MAX};
    nx_buf bytes, out; nx_buf_init(&bytes); nx_buf_init(&out);
    nx_status st = nx_bitmap_build(ids, 7, &bytes);
    nx_bitmap b;
    if (st == NX_OK) st = nx_bitmap_open(nx_buf_slice(&bytes), &b);
    if (st == NX_OK) st = nx_bitmap_combine(&b, &b, NX_BITMAP_OR, &out);
    if (st == NX_OK) st = nx_bitmap_open(nx_buf_slice(&out), &b);
    nx_buf_free(&bytes); nx_buf_free(&out);
    return st;
}
static void test_bitmap_oom(void) { nx_test_oom_sweep(bitmap_oom, NULL, 100); }

static void test_bitmap_empty_threshold(void) {
    uint32_t ids[4097];
    for (uint32_t i = 0; i < 4097; i++) ids[i] = i;
    const size_t counts[] = {0,1,4096,4097};
    for (size_t j = 0; j < 4; j++) {
        nx_buf b; nx_buf_init(&b);
        NX_CHECK_OK(nx_bitmap_build(ids, counts[j], &b));
        nx_bitmap v; NX_CHECK_OK(nx_bitmap_open(nx_buf_slice(&b), &v));
        NX_CHECK_EQ_U(v.cardinality, counts[j]);
        for (uint32_t i = 0; i <= 4097; i++) NX_CHECK(nx_bitmap_contains(&v, i) == (i < counts[j]));
        nx_buf_free(&b);
    }
}

int main(void) {
    NX_RUN(test_bitmap_boundaries);
    NX_RUN(test_bitmap_set_oracle);
    NX_RUN(test_bitmap_random_corruption);
    NX_RUN(test_bitmap_oom);
    NX_RUN(test_bitmap_empty_threshold);
    return nx_test_summary();
}
