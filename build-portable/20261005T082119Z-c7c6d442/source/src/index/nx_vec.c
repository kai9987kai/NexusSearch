#include "index/nx_vec.h"
#include "core/nx_simd.h"
#include <math.h>
#include <float.h>
#if defined(NX_X86_64) && defined(NX_GNUC)
#include <immintrin.h>
#define NX_VEC_AVX2 1
#endif
static bool finite_f(float x) { return x >= -FLT_MAX && x <= FLT_MAX; }
static nx_status validate(const float *v, size_t n) {
    if (!v || !n) return NX_ERR_INVALID;
    if (n > NX_VEC_MAX_DIMS) return NX_ERR_LIMIT;
    for (size_t i = 0; i < n; i++) if (!finite_f(v[i])) return NX_ERR_INVALID;
    return NX_OK;
}
static void scalar_sums(const float *a, const float *b, size_t n, double *s) {
    for (size_t i = 0; i < n; i++) {
        double x = a[i], y = b[i], d = x - y;
        s[0] += x*y; s[1] += d*d; s[2] += x*x; s[3] += y*y;
    }
}
#ifdef NX_VEC_AVX2
NX_TARGET("avx2")
static void avx_sums(const float *a, const float *b, size_t n, double *s) {
    __m256d dot = _mm256_setzero_pd(), l2 = dot, na = dot, nb = dot;
    size_t i = 0;
    for (; n - i >= 4; i += 4) {
        __m256d x = _mm256_cvtps_pd(_mm_loadu_ps(a+i)), y = _mm256_cvtps_pd(_mm_loadu_ps(b+i));
        __m256d d = _mm256_sub_pd(x,y);
        dot = _mm256_add_pd(dot,_mm256_mul_pd(x,y)); l2 = _mm256_add_pd(l2,_mm256_mul_pd(d,d));
        na = _mm256_add_pd(na,_mm256_mul_pd(x,x)); nb = _mm256_add_pd(nb,_mm256_mul_pd(y,y));
    }
    double lanes[4];
    _mm256_storeu_pd(lanes,dot); for (size_t j = 0; j < 4; j++) s[0] += lanes[j];
    _mm256_storeu_pd(lanes,l2); for (size_t j = 0; j < 4; j++) s[1] += lanes[j];
    _mm256_storeu_pd(lanes,na); for (size_t j = 0; j < 4; j++) s[2] += lanes[j];
    _mm256_storeu_pd(lanes,nb); for (size_t j = 0; j < 4; j++) s[3] += lanes[j];
    scalar_sums(a+i,b+i,n-i,s);
}
#endif
static bool use_avx(void) {
#ifdef NX_VEC_AVX2
    return nx_cpu()->avx2 && nx_simd_level_get() >= NX_SIMD_AVX2;
#else
    return false;
#endif
}
const char *nx_vec_impl_name(void) { return use_avx() ? "avx2-f64" : "scalar-f64"; }
static nx_status score(const float *a, const float *b, size_t n, nx_vec_metric m, double *out, bool dispatch) {
    if (!out || m < NX_VEC_DOT || m > NX_VEC_COSINE) return NX_ERR_INVALID;
    *out = 0;
    nx_status st = validate(a,n); if (st != NX_OK) return st;
    st = validate(b,n); if (st != NX_OK) return st;
    double s[4] = {0};
#ifdef NX_VEC_AVX2
    if (dispatch && use_avx()) avx_sums(a,b,n,s); else
#else
    (void)dispatch;
#endif
    scalar_sums(a,b,n,s);
    if (m == NX_VEC_DOT) *out = s[0];
    else if (m == NX_VEC_L2SQ) *out = s[1];
    else if (s[2] && s[3]) {
        *out = s[0] / (sqrt(s[2]) * sqrt(s[3]));
        if (*out > 1) *out = 1;
        if (*out < -1) *out = -1;
    }
    return NX_OK;
}
nx_status nx_vec_score(const float *a, const float *b, size_t n, nx_vec_metric metric, double *out) { return score(a,b,n,metric,out,true); }
nx_status nx_vec_score_scalar(const float *a, const float *b, size_t n, nx_vec_metric metric, double *out) { return score(a,b,n,metric,out,false); }
nx_status nx_vec_sq8_encode(const float *v, size_t n, uint8_t *codes, nx_sq8 *metadata) {
    if (metadata) memset(metadata,0,sizeof *metadata);
    if (!codes || !metadata) return NX_ERR_INVALID;
    nx_status st = validate(v,n); if (st != NX_OK) return st;
    float lo = v[0], hi = v[0];
    for (size_t i = 1; i < n; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    metadata->minimum = lo; metadata->step = ((double)hi - lo) / 255.0;
    for (size_t i = 0; i < n; i++) {
        double q = metadata->step ? floor(((double)v[i] - lo) / metadata->step + 0.5) : 0;
        codes[i] = (uint8_t)(q > 255 ? 255 : q < 0 ? 0 : q);
    }
    return NX_OK;
}
nx_status nx_vec_sq8_decode(const uint8_t *codes, size_t n, nx_sq8 metadata, float *v) {
    if (!codes || !v || !n || !finite_f(metadata.minimum) || !(metadata.step >= 0 && metadata.step <= DBL_MAX)) return NX_ERR_INVALID;
    if (n > NX_VEC_MAX_DIMS) return NX_ERR_LIMIT;
    double highest = (double)metadata.minimum + metadata.step * 255;
    if (!(highest <= FLT_MAX && highest >= -FLT_MAX)) return NX_ERR_INVALID;
    for (size_t i = 0; i < n; i++) v[i] = (float)((double)metadata.minimum + metadata.step * codes[i]);
    return NX_OK;
}
