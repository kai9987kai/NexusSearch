#include "nx_test.h"
#include "index/nx_vec.h"
#include <float.h>

static void test_vector_oracle(void) {
    nx_rng rng = nx_test_rng(441);
    float a[1025], b[1025];
    for (size_t round = 0; round < nx_test_iters(200); round++) {
        size_t n = (size_t)(nx_rng_u64(&rng) % 1024) + 1;
        double dot = 0, l2 = 0, na = 0, nb = 0;
        for (size_t i = 0; i < n; i++) {
            a[i+1] = (float)((int32_t)(nx_rng_u64(&rng) % 20001) - 10000) / 100.0f;
            b[i+1] = (float)((int32_t)(nx_rng_u64(&rng) % 20001) - 10000) / 100.0f;
            double x = a[i+1], y = b[i+1]; dot += x*y; l2 += (x-y)*(x-y); na += x*x; nb += y*y;
        }
        double want[] = {dot, l2, na && nb ? dot / sqrt(na * nb) : 0};
        for (int m = NX_VEC_DOT; m <= NX_VEC_COSINE; m++) {
            double scalar = 0, dispatched = 0;
            NX_CHECK_OK(nx_vec_score_scalar(a+1,b+1,n,(nx_vec_metric)m,&scalar));
            NX_CHECK_OK(nx_vec_score(a+1,b+1,n,(nx_vec_metric)m,&dispatched));
            NX_CHECK_NEAR(scalar, want[m], 1e-10 * (1 + fabs(want[m])));
            NX_CHECK_NEAR(dispatched, want[m], 1e-10 * (1 + fabs(want[m])));
        }
    }
}
static void test_vector_extremes(void) {
    float a[] = {FLT_MAX, -FLT_MAX, 0, 0}, b[] = {-FLT_MAX, FLT_MAX, 0, 0};
    double score = 0;
    NX_CHECK_OK(nx_vec_score(a,b,4,NX_VEC_COSINE,&score)); NX_CHECK_NEAR(score,-1,1e-15);
    NX_CHECK_OK(nx_vec_score(a+2,b+2,2,NX_VEC_COSINE,&score)); NX_CHECK_EQ_I(score,0);
    NX_CHECK_EQ_I(nx_vec_score(a,b,0,NX_VEC_DOT,&score),NX_ERR_INVALID);
    NX_CHECK_EQ_I(nx_vec_score(a,b,NX_VEC_MAX_DIMS+1,NX_VEC_DOT,&score),NX_ERR_LIMIT);
    a[0] = NAN; NX_CHECK_EQ_I(nx_vec_score(a,b,4,NX_VEC_DOT,&score),NX_ERR_INVALID);
}
static void test_sq8_error_bound(void) {
    float values[513], restored[513]; uint8_t codes[513]; nx_sq8 meta;
    nx_rng rng = nx_test_rng(721);
    for (size_t r = 0; r < 100; r++) {
        for (size_t i = 0; i < 513; i++) values[i] = (float)((int32_t)(nx_rng_u64(&rng) % 100000) - 50000) / 100.0f;
        NX_CHECK_OK(nx_vec_sq8_encode(values,513,codes,&meta));
        NX_CHECK_OK(nx_vec_sq8_decode(codes,513,meta,restored));
        for (size_t i = 0; i < 513; i++) NX_CHECK_NEAR(values[i],restored[i],meta.step * 0.5 + 0.0001);
    }
    float extremes[] = {-FLT_MAX, FLT_MAX};
    NX_CHECK_OK(nx_vec_sq8_encode(extremes,2,codes,&meta));
    NX_CHECK_OK(nx_vec_sq8_decode(codes,2,meta,restored));
    NX_CHECK(restored[0] == extremes[0] && restored[1] == extremes[1]);
    values[0] = values[1] = 3.25f;
    NX_CHECK_OK(nx_vec_sq8_encode(values,2,codes,&meta)); NX_CHECK(meta.step == 0 && codes[0] == 0);
    NX_CHECK_OK(nx_vec_sq8_decode(codes,2,meta,restored)); NX_CHECK(restored[1] == 3.25f);
    codes[0] = 7; values[0] = NAN;
    NX_CHECK_EQ_I(nx_vec_sq8_encode(values,2,codes,&meta),NX_ERR_INVALID); NX_CHECK(codes[0] == 7);
    meta.minimum = 0; meta.step = -1;
    NX_CHECK_EQ_I(nx_vec_sq8_decode(codes,2,meta,restored),NX_ERR_INVALID);
}
int main(void) { NX_RUN(test_vector_oracle); NX_RUN(test_vector_extremes); NX_RUN(test_sq8_error_bound); return nx_test_summary(); }
