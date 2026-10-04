#include "bench_common.h"
#include "index/nx_bsi.h"
#include "core/nx_mem.h"
#include <stdio.h>
#include <stdlib.h>

#define NUM_ROWS 50000
#define QUERY_ITERS 1000

static void bench_bsi_suite(void) {
    int64_t *values = (int64_t *)nx_malloc(NUM_ROWS * sizeof(int64_t));
    uint8_t *present = (uint8_t *)nx_malloc(NUM_ROWS);

    /* Generate data: values from -25000 to 24999 */
    for (size_t i = 0; i < NUM_ROWS; ++i) {
        values[i] = (int64_t)i - (NUM_ROWS / 2);
        present[i] = (i % 20 != 0); /* 95% present */
    }

    /* 1. Build BSI index */
    nx_buf index_buf;
    nx_buf_init(&index_buf);
    uint64_t t0 = nx_bench_time_now();
    nx_status st = nx_bsi_build(values, present, NUM_ROWS, &index_buf);
    uint64_t t1 = nx_bench_time_now();
    double build_ms = nx_bench_elapsed_sec(t0, t1) * 1000.0;
    if (st != NX_OK) {
        printf("Failed to build BSI: %s\n", nx_status_str(st));
        goto cleanup;
    }

    printf("Built BSI Index for %d rows in %.2f ms (Size: %zu KB, %.2f bytes/row)\n\n",
           NUM_ROWS, build_ms, index_buf.len / 1024, (double)index_buf.len / NUM_ROWS);

    nx_bsi index;
    st = nx_bsi_open(nx_buf_slice(&index_buf), &index);
    if (st != NX_OK) {
        printf("Failed to open BSI: %s\n", nx_status_str(st));
        goto cleanup;
    }

    /* Benchmark comparison operators */
    struct {
        const char *name;
        nx_compare op;
        int64_t target;
    } ops[] = {
        {"EQ (point lookup)", NX_CMP_EQ, 0},
        {"NE (not equal)", NX_CMP_NE, 0},
        {"GT (50% selectivity)", NX_CMP_GT, 0},
        {"GE (90% selectivity)", NX_CMP_GE, -(int64_t)(NUM_ROWS * 0.4)},
        {"LT (10% selectivity)", NX_CMP_LT, -(int64_t)(NUM_ROWS * 0.4)},
        {"LE (50% selectivity)", NX_CMP_LE, 0},
    };

    puts("BSI Query Performance:");
    puts("------------------------------------------------------------------------------------------");
    for (size_t o = 0; o < sizeof(ops) / sizeof(ops[0]); ++o) {
        nx_bench_stats stats;
        nx_bench_stats_init(&stats, QUERY_ITERS);

        nx_buf res;
        nx_buf_init(&res);

        uint64_t t_all_start = nx_bench_time_now();
        for (size_t i = 0; i < QUERY_ITERS; ++i) {
            nx_buf_clear(&res);
            uint64_t t_start = nx_bench_time_now();
            (void)nx_bsi_filter(&index, ops[o].op, ops[o].target, &res);
            uint64_t t_end = nx_bench_time_now();
            nx_bench_stats_add(&stats, nx_bench_elapsed_us(t_start, t_end));
        }
        uint64_t t_all_end = nx_bench_time_now();
        nx_bench_stats_finalize(&stats, nx_bench_elapsed_sec(t_all_start, t_all_end));

        nx_bench_report(ops[o].name, &stats);

        nx_buf_free(&res);
        nx_bench_stats_free(&stats);
    }

    /* Benchmark direct scalar array scan for comparison */
    puts("\nReference Scalar Array Scan Comparison:");
    puts("------------------------------------------------------------------------------------------");
    {
        nx_bench_stats stats;
        nx_bench_stats_init(&stats, QUERY_ITERS);
        uint8_t *matches = (uint8_t *)nx_malloc(NUM_ROWS);

        uint64_t t_all_start = nx_bench_time_now();
        for (size_t i = 0; i < QUERY_ITERS; ++i) {
            uint64_t t_start = nx_bench_time_now();
            for (size_t r = 0; r < NUM_ROWS; ++r) {
                matches[r] = (present[r] && values[r] > 0) ? 1 : 0;
            }
            uint64_t t_end = nx_bench_time_now();
            nx_bench_stats_add(&stats, nx_bench_elapsed_us(t_start, t_end));
        }
        uint64_t t_all_end = nx_bench_time_now();
        nx_bench_stats_finalize(&stats, nx_bench_elapsed_sec(t_all_start, t_all_end));

        nx_bench_report("Scalar Array Scan (GT 50%)", &stats);

        nx_free(matches);
        nx_bench_stats_free(&stats);
    }

cleanup:
    nx_buf_free(&index_buf);
    nx_free(values);
    nx_free(present);
}

int main(void) {
    puts("==========================================================================================");
    puts("NexusSearch Bit-Sliced Index (BSI) Benchmark");
    puts("==========================================================================================\n");

    bench_bsi_suite();

    puts("\n==========================================================================================");
    return 0;
}
