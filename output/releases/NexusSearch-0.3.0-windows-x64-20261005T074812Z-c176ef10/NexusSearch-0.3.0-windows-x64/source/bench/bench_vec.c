#include "bench_common.h"
#include "index/nx_vec.h"
#include "core/nx_mem.h"
#include <stdio.h>
#include <stdlib.h>

#define ITERS 20000

static void bench_metric_dims(size_t dims, nx_vec_metric metric, const char *metric_name) {
    float *a = (float *)nx_malloc(dims * sizeof(float));
    float *b = (float *)nx_malloc(dims * sizeof(float));
    for (size_t i = 0; i < dims; ++i) {
        a[i] = (float)((i % 17) - 8) * 0.125f;
        b[i] = (float)((i % 23) - 11) * 0.0625f;
    }

    /* Benchmark dispatched (SIMD when available) */
    nx_bench_stats s_disp;
    nx_bench_stats_init(&s_disp, ITERS);
    double out = 0;
    uint64_t t0 = nx_bench_time_now();
    for (size_t i = 0; i < ITERS; ++i) {
        uint64_t t_start = nx_bench_time_now();
        (void)nx_vec_score(a, b, dims, metric, &out);
        uint64_t t_end = nx_bench_time_now();
        nx_bench_stats_add(&s_disp, nx_bench_elapsed_us(t_start, t_end));
    }
    uint64_t t1 = nx_bench_time_now();
    nx_bench_stats_finalize(&s_disp, nx_bench_elapsed_sec(t0, t1));

    /* Benchmark scalar */
    nx_bench_stats s_scal;
    nx_bench_stats_init(&s_scal, ITERS);
    t0 = nx_bench_time_now();
    for (size_t i = 0; i < ITERS; ++i) {
        uint64_t t_start = nx_bench_time_now();
        (void)nx_vec_score_scalar(a, b, dims, metric, &out);
        uint64_t t_end = nx_bench_time_now();
        nx_bench_stats_add(&s_scal, nx_bench_elapsed_us(t_start, t_end));
    }
    t1 = nx_bench_time_now();
    nx_bench_stats_finalize(&s_scal, nx_bench_elapsed_sec(t0, t1));

    char title_disp[64], title_scal[64];
    snprintf(title_disp, sizeof(title_disp), "%s %zu-dim (%s)", metric_name, dims, nx_vec_impl_name());
    snprintf(title_scal, sizeof(title_scal), "%s %zu-dim (scalar)", metric_name, dims);

    nx_bench_report(title_disp, &s_disp);
    nx_bench_report(title_scal, &s_scal);

    nx_bench_stats_free(&s_disp);
    nx_bench_stats_free(&s_scal);
    nx_free(a);
    nx_free(b);
}

static void bench_sq8(size_t dims) {
    float *v = (float *)nx_malloc(dims * sizeof(float));
    float *decoded = (float *)nx_malloc(dims * sizeof(float));
    uint8_t *codes = (uint8_t *)nx_malloc(dims);
    for (size_t i = 0; i < dims; ++i) {
        v[i] = (float)((i % 31) - 15) * 0.25f;
    }
    nx_sq8 meta;

    nx_bench_stats s_enc;
    nx_bench_stats_init(&s_enc, ITERS);
    uint64_t t0 = nx_bench_time_now();
    for (size_t i = 0; i < ITERS; ++i) {
        uint64_t t_start = nx_bench_time_now();
        (void)nx_vec_sq8_encode(v, dims, codes, &meta);
        uint64_t t_end = nx_bench_time_now();
        nx_bench_stats_add(&s_enc, nx_bench_elapsed_us(t_start, t_end));
    }
    uint64_t t1 = nx_bench_time_now();
    nx_bench_stats_finalize(&s_enc, nx_bench_elapsed_sec(t0, t1));

    nx_bench_stats s_dec;
    nx_bench_stats_init(&s_dec, ITERS);
    t0 = nx_bench_time_now();
    for (size_t i = 0; i < ITERS; ++i) {
        uint64_t t_start = nx_bench_time_now();
        (void)nx_vec_sq8_decode(codes, dims, meta, decoded);
        uint64_t t_end = nx_bench_time_now();
        nx_bench_stats_add(&s_dec, nx_bench_elapsed_us(t_start, t_end));
    }
    t1 = nx_bench_time_now();
    nx_bench_stats_finalize(&s_dec, nx_bench_elapsed_sec(t0, t1));

    char title_enc[64], title_dec[64];
    snprintf(title_enc, sizeof(title_enc), "SQ8 Encode %zu-dim", dims);
    snprintf(title_dec, sizeof(title_dec), "SQ8 Decode %zu-dim", dims);

    nx_bench_report(title_enc, &s_enc);
    nx_bench_report(title_dec, &s_dec);

    nx_bench_stats_free(&s_enc);
    nx_bench_stats_free(&s_dec);
    nx_free(v);
    nx_free(decoded);
    nx_free(codes);
}

int main(void) {
    puts("==========================================================================================");
    printf("NexusSearch Vector SIMD & Quantization Benchmark (Active: %s)\n", nx_vec_impl_name());
    puts("==========================================================================================");

    size_t test_dims[] = {128, 384, 768, 1536};
    for (size_t i = 0; i < sizeof(test_dims) / sizeof(test_dims[0]); ++i) {
        size_t d = test_dims[i];
        printf("\n--- Dimension: %zu ---\n", d);
        bench_metric_dims(d, NX_VEC_COSINE, "Cosine Similarity");
        bench_metric_dims(d, NX_VEC_DOT, "Dot Product");
        bench_metric_dims(d, NX_VEC_L2SQ, "L2 Squared");
        bench_sq8(d);
    }

    puts("\n==========================================================================================");
    return 0;
}
