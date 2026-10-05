/* Reproducible numeric-filter experiment. Timing covers nx_search only, including
 * its allocations and sorting; corpus construction/opening and result validation
 * are outside the timed region. All hits are checked against generated inputs,
 * not against values read back through the table or index under test.
 * Usage: bench_search_compare > result.json (Release, NX_MEM_DEBUG=OFF preferred).
 * This is a warm-cache, single-threaded microbenchmark, not a capacity claim. */
#include "bench_common.h"
#include "nexus/nexus.h"

#define DOCUMENTS 10000u
#define WARMUPS 3u
#define PAIRS 21u
#define SEED UINT32_C(0x4e585331)

typedef struct generated_row { int64_t value; bool present; } generated_row;
typedef struct measurement {
    uint64_t ns;
    size_t work, numeric_indexes, scanned_cells;
} measurement;
typedef struct experiment {
    const char *name, *query;
    int64_t minimum;
    size_t expected;
    measurement indexed[PAIRS], scan[PAIRS];
} experiment;

static uint32_t next_random(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *state = x;
}

static bool check_result(const nx_search_result *result, const generated_row *rows,
                         const experiment *e, bool indexed) {
    if (result->count != e->expected || result->total != e->expected ||
        result->indexed != indexed || result->has_lexical || result->has_vector ||
        result->explain_only || result->vectors_scored != 0 ||
        result->numeric_indexes != (indexed ? 1u : 0u) ||
        result->scanned_cells != (indexed ? 0u : DOCUMENTS)) return false;
    size_t hit = 0;
    for (uint32_t row = 0; row < DOCUMENTS; ++row) {
        if (!rows[row].present || rows[row].value < e->minimum) continue;
        char expected_id[32];
        int n = snprintf(expected_id, sizeof(expected_id), "doc-%05" PRIu32, row);
        if (n < 0 || (size_t)n >= sizeof(expected_id) ||
            result->hits[hit].row != row || result->hits[hit].score != 0.0 ||
            !nx_slice_eq(result->hits[hit].id, nx_slice_cstr(expected_id))) return false;
        ++hit;
    }
    return hit == e->expected;
}

static bool run_one(const nx_table *table, const generated_row *rows,
                    const experiment *e, bool indexed, measurement *m,
                    nx_search_result *result) {
    nx_search_options options = nx_search_default_options();
    options.indexed = indexed;
    options.max_hits = DOCUMENTS;
    nx_error error;
    nx_error_clear(&error);
    uint64_t begin = nx_bench_time_now();
    nx_status status = nx_search(table, nx_slice_cstr(e->query), &options, result, &error);
    uint64_t end = nx_bench_time_now();
    m->ns = (uint64_t)(nx_bench_elapsed_sec(begin, end) * 1e9 + 0.5);
    if (status != NX_OK) {
        fprintf(stderr, "%s/%s failed: %s: %s\n", e->name, indexed ? "indexed" : "scan",
                nx_status_str(status), error.msg);
        return false;
    }
    m->work = result->work;
    m->numeric_indexes = result->numeric_indexes;
    m->scanned_cells = result->scanned_cells;
    if (!check_result(result, rows, e, indexed)) {
        fprintf(stderr, "%s/%s disagrees with independent generated-value oracle\n",
                e->name, indexed ? "indexed" : "scan");
        return false;
    }
    return true;
}

static bool run_pair(const nx_table *table, const generated_row *rows,
                     const experiment *e, bool indexed_first,
                     measurement *indexed, measurement *scan) {
    nx_search_result a = {0}, b = {0};
    bool valid;
    if (indexed_first) {
        valid = run_one(table, rows, e, true, indexed, &a) &&
                run_one(table, rows, e, false, scan, &b);
    } else {
        valid = run_one(table, rows, e, false, scan, &b) &&
                run_one(table, rows, e, true, indexed, &a);
    }
    if (valid) {
        valid = a.count == b.count && a.total == b.total;
        for (size_t i = 0; valid && i < a.count; ++i)
            valid = a.hits[i].row == b.hits[i].row && a.hits[i].score == b.hits[i].score &&
                    nx_slice_eq(a.hits[i].id, b.hits[i].id);
    }
    nx_search_result_free(&a);
    nx_search_result_free(&b);
    return valid;
}

static int compare_ns(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void print_arm(const measurement *samples) {
    uint64_t sorted[PAIRS];
    for (size_t i = 0; i < PAIRS; ++i) sorted[i] = samples[i].ns;
    qsort(sorted, PAIRS, sizeof(sorted[0]), compare_ns);
    printf("{\"p50_ns\":%" PRIu64 ",\"p95_ns\":%" PRIu64 ",\"samples\":[",
           sorted[10], sorted[19]); /* Nearest-rank percentiles for 21 samples. */
    for (size_t i = 0; i < PAIRS; ++i) {
        if (i) putchar(',');
        printf("{\"pair\":%zu,\"ns\":%" PRIu64 ",\"work\":%zu,"
               "\"numeric_indexes\":%zu,\"scanned_cells\":%zu}",
               i, samples[i].ns, samples[i].work, samples[i].numeric_indexes,
               samples[i].scanned_cells);
    }
    printf("]}");
}

int main(void) {
    generated_row rows[DOCUMENTS];
    experiment cases[] = {
        {"selective", "value:>=49000 LIMIT 10000", INT64_C(49000), 0, {{0}}, {{0}}},
        {"broad", "value:>=-40000 LIMIT 10000", INT64_C(-40000), 0, {{0}}, {{0}}}
    };
    nx_buf jsonl, bytes;
    nx_buf_init(&jsonl); nx_buf_init(&bytes);
    uint32_t state = SEED;
    size_t missing = 0, explicit_null = 0;
    int exit_code = 1;
    for (uint32_t row = 0; row < DOCUMENTS; ++row) {
        rows[row].value = (int64_t)(next_random(&state) % 100000u) - INT64_C(50000);
        uint32_t kind = next_random(&state) % 8u;
        rows[row].present = kind > 1u;
        if (kind == 0) {
            ++missing;
            nx_buf_printf(&jsonl, "{\"_id\":\"doc-%05" PRIu32 "\"}\n", row);
        } else if (kind == 1) {
            ++explicit_null;
            nx_buf_printf(&jsonl, "{\"_id\":\"doc-%05" PRIu32 "\",\"value\":null}\n", row);
        } else {
            nx_buf_printf(&jsonl, "{\"_id\":\"doc-%05" PRIu32 "\",\"value\":%" PRId64 "}\n",
                          row, rows[row].value);
        }
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c)
            if (rows[row].present && rows[row].value >= cases[c].minimum) ++cases[c].expected;
    }
    if (jsonl.oom) { fputs("Corpus allocation failed\n", stderr); goto done; }
    nx_error error;
    nx_error_clear(&error);
    nx_table table;
    nx_status status = nx_table_build(nx_buf_slice(&jsonl), NULL, &bytes, &error);
    if (status == NX_OK) status = nx_table_open(nx_buf_slice(&bytes), &table, &error);
    if (status != NX_OK) {
        fprintf(stderr, "Snapshot failed: %s: %s\n", nx_status_str(status), error.msg);
        goto done;
    }
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        experiment *e = &cases[c];
        for (size_t i = 0; i < WARMUPS; ++i) {
            measurement a = {0}, b = {0};
            if (!run_pair(&table, rows, e, i % 2 == 0, &a, &b)) goto done;
        }
        for (size_t i = 0; i < PAIRS; ++i)
            if (!run_pair(&table, rows, e, i % 2 == 0, &e->indexed[i], &e->scan[i])) goto done;
    }
    printf("{\"schema\":1,\"benchmark\":\"snapshot-numeric-index-vs-scan\","
           "\"seed\":%" PRIu32 ",\"generator\":\"xorshift32\",\"documents\":%u,"
           "\"missing\":%zu,\"explicit_null\":%zu,\"snapshot_bytes\":%zu,"
           "\"warmup_pairs_per_case\":%u,\"measured_pairs_per_case\":%u,"
           "\"order\":\"even pair indexed first; odd pair scan first\","
           "\"timing_scope\":\"nx_search only, including allocation and sorting; no build, open, validation, serialization or result-free\","
           "\"percentile_method\":\"nearest rank\","
           "\"validation\":\"Every warmup and measured search: full membership, IDs, order, zero scores and execution path against original generated values; paired results identical\","
           "\"limitations\":\"Single-process warm-cache synthetic numeric workload; not ANN, text, cold-cache, concurrency, memory-use or cross-platform performance evidence\","
#if defined(NX_MEM_DEBUG)
           "\"memory_debug\":true,"
#else
           "\"memory_debug\":false,"
#endif
#if defined(NX_X86_64)
           "\"binary_arch\":\"x86_64\","
#elif defined(NX_ARM64)
           "\"binary_arch\":\"arm64\","
#else
           "\"binary_arch\":\"other\","
#endif
           "\"cases\":[",
           SEED, DOCUMENTS, missing, explicit_null, bytes.len, WARMUPS, PAIRS);
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        const experiment *e = &cases[c];
        if (c) putchar(',');
        printf("{\"name\":\"%s\",\"query\":\"%s\",\"expected_total\":%zu,"
               "\"selectivity\":%.8f,\"indexed\":", e->name, e->query, e->expected,
               (double)e->expected / DOCUMENTS);
        print_arm(e->indexed);
        printf(",\"scan\":"); print_arm(e->scan);
        putchar('}');
    }
    puts("]}");
    exit_code = ferror(stdout) ? 1 : 0;
done:
    nx_buf_free(&bytes); nx_buf_free(&jsonl);
    return exit_code;
}
