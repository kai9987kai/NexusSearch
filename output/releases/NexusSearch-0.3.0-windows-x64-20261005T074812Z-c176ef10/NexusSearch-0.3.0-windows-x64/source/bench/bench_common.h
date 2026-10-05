/* NexusSearch Benchmark Common Utilities.
 * High-precision timing, percentile statistics, and formatted reports. */
#ifndef NX_BENCH_COMMON_H
#define NX_BENCH_COMMON_H

#include "core/nx_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static inline uint64_t nx_bench_time_now(void) {
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (uint64_t)count.QuadPart;
}
static inline double nx_bench_elapsed_sec(uint64_t start, uint64_t end) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return (double)(end - start) / (double)freq.QuadPart;
}
#else
#include <time.h>
static inline uint64_t nx_bench_time_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline double nx_bench_elapsed_sec(uint64_t start, uint64_t end) {
    return (double)(end - start) / 1000000000.0;
}
#endif

static inline double nx_bench_elapsed_us(uint64_t start, uint64_t end) {
    return nx_bench_elapsed_sec(start, end) * 1000000.0;
}

typedef struct nx_bench_stats {
    double *samples_us;
    size_t count;
    size_t capacity;
    double total_sec;
    double min_us;
    double max_us;
    double mean_us;
    double p50_us;
    double p95_us;
    double p99_us;
    double qps;
} nx_bench_stats;

static inline void nx_bench_stats_init(nx_bench_stats *s, size_t capacity) {
    memset(s, 0, sizeof(*s));
    s->capacity = capacity;
    s->samples_us = (double *)malloc(capacity * sizeof(double));
}

static inline void nx_bench_stats_free(nx_bench_stats *s) {
    if (s->samples_us) {
        free(s->samples_us);
        s->samples_us = NULL;
    }
}

static inline void nx_bench_stats_add(nx_bench_stats *s, double us) {
    if (s->count < s->capacity) {
        s->samples_us[s->count++] = us;
    }
}

static int double_cmp(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static inline void nx_bench_stats_finalize(nx_bench_stats *s, double total_sec) {
    s->total_sec = total_sec;
    if (s->count == 0) return;
    qsort(s->samples_us, s->count, sizeof(double), double_cmp);
    s->min_us = s->samples_us[0];
    s->max_us = s->samples_us[s->count - 1];
    s->p50_us = s->samples_us[s->count * 50 / 100];
    s->p95_us = s->samples_us[s->count * 95 / 100];
    s->p99_us = s->samples_us[s->count * 99 / 100];
    double sum = 0;
    for (size_t i = 0; i < s->count; ++i) sum += s->samples_us[i];
    s->mean_us = sum / (double)s->count;
    s->qps = total_sec > 0 ? (double)s->count / total_sec : 0;
}

static inline void nx_bench_report(const char *name, const nx_bench_stats *s) {
    printf("%-38s | %8.1f QPS | mean %6.1f us | p50 %6.1f us | p95 %6.1f us | p99 %6.1f us\n",
           name, s->qps, s->mean_us, s->p50_us, s->p95_us, s->p99_us);
}

#endif
