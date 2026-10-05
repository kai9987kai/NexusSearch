#include "nx_test.h"
#include "index/nx_fuzzy.h"
static uint32_t oracle(const char *a, size_t n, const char *b, size_t m) {
    uint32_t d[25][25] = {{0}};
    for (size_t i = 0; i <= n; i++) d[i][0] = (uint32_t)i;
    for (size_t j = 0; j <= m; j++) d[0][j] = (uint32_t)j;
    for (size_t i = 1; i <= n; i++) for (size_t j = 1; j <= m; j++) {
        uint32_t v = d[i-1][j-1] + (a[i-1] != b[j-1]);
        if (d[i-1][j] + 1 < v) v = d[i-1][j] + 1;
        if (d[i][j-1] + 1 < v) v = d[i][j-1] + 1;
        d[i][j] = v;
    }
    return d[n][m];
}
static void test_fuzzy_oracle(void) {
    nx_rng rng = nx_test_rng(198);
    for (size_t round = 0; round < nx_test_iters(5000); round++) {
        char a[24], b[24]; size_t n = (size_t)(nx_rng_u64(&rng) % 24), m = (size_t)(nx_rng_u64(&rng) % 24);
        for (size_t i = 0; i < n; i++) a[i] = "abcd"[nx_rng_u64(&rng) % 4];
        for (size_t i = 0; i < m; i++) b[i] = "abcd"[nx_rng_u64(&rng) % 4];
        uint32_t exact = oracle(a,n,b,m), cap = (uint32_t)(nx_rng_u64(&rng) % 16), got = 0;
        NX_CHECK_OK(nx_fuzzy_distance(nx_slice_make(a,n),nx_slice_make(b,m),cap,&got));
        NX_CHECK_EQ_U(got, exact > cap ? cap + 1 : exact);
    }
    uint32_t got;
    NX_CHECK_OK(nx_fuzzy_distance(nx_slice_cstr("caf\xc3\xa9"),nx_slice_cstr("cafe"),2,&got)); NX_CHECK_EQ_U(got,1);
    NX_CHECK_OK(nx_fuzzy_distance(nx_slice_cstr("\xf0\x9f\x98\x80"),nx_slice_cstr(""),2,&got)); NX_CHECK_EQ_U(got,1);
    NX_CHECK_EQ_I(nx_fuzzy_distance(nx_slice_make("\xff",1),nx_slice_cstr(""),2,&got),NX_ERR_CORRUPT);
    NX_CHECK_EQ_I(nx_fuzzy_distance(nx_slice_cstr("a"),nx_slice_cstr("b"),65,&got),NX_ERR_LIMIT);
}
static nx_status scenario(void *ctx) {
    (void)ctx; uint32_t distance;
    return nx_fuzzy_distance(nx_slice_cstr("kitten"),nx_slice_cstr("sitting"),3,&distance);
}
static void test_fuzzy_oom(void) { nx_test_oom_sweep(scenario,NULL,20); }
int main(void) { NX_RUN(test_fuzzy_oracle); NX_RUN(test_fuzzy_oom); return nx_test_summary(); }
