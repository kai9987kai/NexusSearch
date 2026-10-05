#include "bench_common.h"
#include "nexus/nexus.h"
#include "core/nx_mem.h"
#include <stdio.h>
#include <stdlib.h>

#define DOC_COUNT 2000
#define SEARCH_ITERS 500

static const char *sample_words[] = {
    "search", "engine", "vector", "database", "retrieval",
    "index", "high", "performance", "embedded", "memory",
    "snapshot", "filter", "query", "ranking", "bm25",
    "cosine", "similarity", "document", "atomic", "storage"
};

static void generate_synthetic_corpus(nx_buf *jsonl, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const char *w1 = sample_words[i % 20];
        const char *w2 = sample_words[(i * 7 + 3) % 20];
        const char *w3 = sample_words[(i * 13 + 5) % 20];
        int64_t year = 2000 + (int64_t)(i % 30);
        double rating = 1.0 + (double)(i % 40) * 0.1;
        bool active = (i % 2 == 0);
        float v0 = (float)((int)(i % 10) - 5) * 0.2f;
        float v1 = (float)((int)((i / 10) % 10) - 5) * 0.2f;
        float v2 = (float)((int)((i / 100) % 10) - 5) * 0.2f;

        nx_buf_printf(jsonl,
            "{\"_id\":\"doc-%05zu\",\"title\":\"%s %s\",\"body\":\"A %s engine for %s with %s\",\"year\":%" PRId64 ",\"rating\":%.2f,\"active\":%s,\"embedding\":[%.2f,%.2f,%.2f]}\n",
            i, w1, w2, w1, w2, w3, year, rating, active ? "true" : "false", (double)v0, (double)v1, (double)v2);
    }
}

static void run_search_bench(const nx_table *table, const char *name, const char *query_str, bool scan, size_t iters) {
    nx_search_options opts = nx_search_default_options();
    opts.indexed = !scan;

    nx_slice q_slice = nx_slice_cstr(query_str);
    nx_search_result res = {0};

    /* Warmup query */
    nx_status st = nx_search(table, q_slice, &opts, &res, NULL);
    if (st != NX_OK) {
        printf("Query '%s' failed: %s\n", query_str, nx_status_str(st));
        return;
    }
    size_t hits_found = res.count;
    size_t total_found = res.total;
    size_t work_done = res.work;
    nx_search_result_free(&res);

    nx_bench_stats stats;
    nx_bench_stats_init(&stats, iters);

    uint64_t t_all_start = nx_bench_time_now();
    for (size_t i = 0; i < iters; ++i) {
        uint64_t t_start = nx_bench_time_now();
        (void)nx_search(table, q_slice, &opts, &res, NULL);
        uint64_t t_end = nx_bench_time_now();
        nx_bench_stats_add(&stats, nx_bench_elapsed_us(t_start, t_end));
        nx_search_result_free(&res);
    }
    uint64_t t_all_end = nx_bench_time_now();
    nx_bench_stats_finalize(&stats, nx_bench_elapsed_sec(t_all_start, t_all_end));

    char title[64];
    snprintf(title, sizeof(title), "%s (%zu/%zu hits)", name, hits_found, total_found);
    nx_bench_report(title, &stats);
    (void)work_done;

    nx_bench_stats_free(&stats);
}

int main(void) {
    puts("==========================================================================================");
    puts("NexusSearch End-to-End Query Engine Benchmark");
    puts("==========================================================================================\n");

    printf("Generating %d synthetic documents...\n", DOC_COUNT);
    nx_buf jsonl;
    nx_buf_init(&jsonl);
    uint64_t t0 = nx_bench_time_now();
    generate_synthetic_corpus(&jsonl, DOC_COUNT);
    uint64_t t1 = nx_bench_time_now();
    printf("Generated %zu KB JSONL in %.1f ms (%.1f MB/s)\n",
           jsonl.len / 1024, nx_bench_elapsed_sec(t0, t1) * 1000.0,
           ((double)jsonl.len / (1024.0 * 1024.0)) / nx_bench_elapsed_sec(t0, t1));

    printf("Building immutable table snapshot...\n");
    nx_buf snapshot_bytes;
    nx_buf_init(&snapshot_bytes);
    nx_error err;
    nx_error_clear(&err);
    t0 = nx_bench_time_now();
    nx_status st = nx_table_build(nx_buf_slice(&jsonl), NULL, &snapshot_bytes, &err);
    t1 = nx_bench_time_now();
    if (st != NX_OK) {
        printf("Table build failed: %s (%s)\n", nx_status_str(st), err.msg);
        nx_buf_free(&jsonl);
        nx_buf_free(&snapshot_bytes);
        return 1;
    }
    printf("Built table snapshot: %zu KB in %.1f ms (%.1f docs/sec)\n\n",
           snapshot_bytes.len / 1024, nx_bench_elapsed_sec(t0, t1) * 1000.0,
           (double)DOC_COUNT / nx_bench_elapsed_sec(t0, t1));

    nx_table table;
    st = nx_table_open(nx_buf_slice(&snapshot_bytes), &table, &err);
    if (st != NX_OK) {
        printf("Table open failed: %s\n", nx_status_str(st));
        nx_buf_free(&jsonl);
        nx_buf_free(&snapshot_bytes);
        return 1;
    }

    printf("Query Engine Latency & Throughput (%d iters per query):\n", SEARCH_ITERS);
    puts("------------------------------------------------------------------------------------------");
    run_search_bench(&table, "BSI Numeric Filter (year:>=2020)", "year:>=2020", false, SEARCH_ITERS);
    run_search_bench(&table, "Scan Oracle (year:>=2020)", "year:>=2020", true, SEARCH_ITERS);
    run_search_bench(&table, "Numeric Range (year:2010..2025)", "year:2010..2025", false, SEARCH_ITERS);
    run_search_bench(&table, "Compound AND (active:true AND yr)", "active:true AND year:>=2020", false, SEARCH_ITERS);
    run_search_bench(&table, "BM25 Text Search (title:search)", "title:search", false, SEARCH_ITERS);
    run_search_bench(&table, "BM25 Multi-term (title:(search...))", "title:(search engine)", false, SEARCH_ITERS);
    run_search_bench(&table, "Substring (title:substr(\"retriev\"))", "title:substr(\"retriev\")", false, SEARCH_ITERS);
    run_search_bench(&table, "Regex (title:/engine/)", "title:/engine/", false, SEARCH_ITERS);
    run_search_bench(&table, "Vector Cosine (embedding:[1,0,0])", "embedding:[1,0,0] LIMIT 10", false, SEARCH_ITERS);
    run_search_bench(&table, "Hybrid RRF (BM25 + Vector)", "title:search AND embedding:[1,0,0] LIMIT 10", false, SEARCH_ITERS);
    run_search_bench(&table, "Sort + Pagination (active SORT BY)", "active:true SORT BY year DESC LIMIT 10", false, SEARCH_ITERS);

    puts("\n==========================================================================================");

    nx_buf_free(&jsonl);
    nx_buf_free(&snapshot_bytes);
    return 0;
}
