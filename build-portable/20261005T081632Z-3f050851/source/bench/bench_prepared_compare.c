/* Reproducible warm-query comparison for the optional exact text preparation.
 * Usage: bench_prepared_compare > result.json (Release, NX_MEM_DEBUG=OFF).
 * Snapshot/index construction and parity checks are outside the timed query. */
#include "bench_common.h"
#include "engine/nx_search_index.h"
#include "nexus/nexus.h"
#include <float.h>
#include <math.h>

#define DOCUMENTS 10000u
#define WARMUPS 3u
#define PAIRS 21u

typedef struct generated_row { bool sparse, phrase, needle; } generated_row;
typedef struct measurement { uint64_t ns; size_t work, scanned_cells; } measurement;
typedef struct experiment {
    const char *name, *query;
    size_t expected, oracle_kind;
    measurement scan[PAIRS], prepared[PAIRS];
} experiment;

static bool has_expected(const generated_row *row, size_t which) {
    return which == 0 ? row->sparse : which == 1 ? row->phrase :
        which == 2 ? row->needle : true;
}

static bool check_result(const nx_search_result *result, const generated_row *rows,
                         const experiment *e, size_t which, bool prepared) {
    if (result->count != e->expected || result->total != e->expected ||
        !result->has_lexical || result->has_vector || result->explain_only ||
        result->prepared_text != prepared || result->scanned_text == prepared ||
        result->vectors_scored != 0) return false;
    bool seen[DOCUMENTS] = {false};
    for (size_t i = 0; i < result->count; ++i) {
        const nx_search_hit *hit = &result->hits[i];
        if (hit->row >= DOCUMENTS || seen[hit->row] || !has_expected(&rows[hit->row], which)) return false;
        char id[32];
        int n = snprintf(id, sizeof(id), "doc-%05" PRIu32, hit->row);
        if (n < 0 || (size_t)n >= sizeof(id) || !nx_slice_eq(hit->id, nx_slice_cstr(id)) ||
            !(hit->score >= -DBL_MAX && hit->score <= DBL_MAX)) return false;
        seen[hit->row] = true;
    }
    for (uint32_t row = 0; row < DOCUMENTS; ++row)
        if (seen[row] != has_expected(&rows[row], which)) return false;
    return true;
}

static bool run_one(const nx_table *table, const nx_search_index *index,
                    const generated_row *rows, const experiment *e, size_t which,
                    bool prepared, measurement *m, nx_search_result *result) {
    measurement ignored = {0};
    if (!m) m = &ignored;
    nx_search_options options = nx_search_default_options();
    options.max_hits = DOCUMENTS;
    nx_error error; nx_error_clear(&error);
    uint64_t begin = nx_bench_time_now();
    nx_status status = prepared ?
        nx_search_index_search(index, nx_slice_cstr(e->query), &options, result, &error) :
        nx_search(table, nx_slice_cstr(e->query), &options, result, &error);
    uint64_t end = nx_bench_time_now();
    m->ns = (uint64_t)(nx_bench_elapsed_sec(begin, end) * 1e9 + 0.5);
    if (status != NX_OK) {
        fprintf(stderr, "%s/%s failed: %s: %s\n", e->name, prepared ? "prepared" : "scan",
                nx_status_str(status), error.msg);
        return false;
    }
    m->work = result->work;
    m->scanned_cells = result->scanned_cells;
    if (!check_result(result, rows, e, which, prepared)) {
        fprintf(stderr, "%s/%s disagrees with generated membership oracle or execution path\n",
                e->name, prepared ? "prepared" : "scan");
        return false;
    }
    return true;
}

static bool same_results(const nx_search_result *a, const nx_search_result *b) {
    if (a->count != b->count || a->total != b->total) return false;
    for (size_t i = 0; i < a->count; ++i)
        if (a->hits[i].row != b->hits[i].row || !nx_slice_eq(a->hits[i].id, b->hits[i].id) ||
            fabs(a->hits[i].score - b->hits[i].score) > 1e-13) return false;
    return true;
}

static bool run_pair(const nx_table *table, const nx_search_index *index,
                     const generated_row *rows, const experiment *e, size_t which,
                     bool prepared_first, measurement *prepared, measurement *scan) {
    nx_search_result a = {0}, b = {0};
    bool valid = prepared_first ?
        run_one(table, index, rows, e, which, true, prepared, &a) &&
        run_one(table, index, rows, e, which, false, scan, &b) :
        run_one(table, index, rows, e, which, false, scan, &b) &&
        run_one(table, index, rows, e, which, true, prepared, &a);
    if (valid && !same_results(&a, &b)) {
        fprintf(stderr, "%s scan and prepared hit IDs/order/scores differ\n", e->name);
        valid = false;
    }
    nx_search_result_free(&a); nx_search_result_free(&b);
    return valid;
}

static int compare_ns(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void json_string(const char *text) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 0x20) printf("\\u%04x", (unsigned)*p);
        else putchar(*p);
    }
    putchar('"');
}

static void print_arm(const measurement *samples) {
    uint64_t sorted[PAIRS];
    for (size_t i = 0; i < PAIRS; ++i) sorted[i] = samples[i].ns;
    qsort(sorted, PAIRS, sizeof(sorted[0]), compare_ns);
    printf("{\"p50_ns\":%" PRIu64 ",\"p95_ns\":%" PRIu64 ",\"samples\":[", sorted[10], sorted[19]);
    for (size_t i = 0; i < PAIRS; ++i) {
        if (i) putchar(',');
        printf("{\"pair\":%zu,\"ns\":%" PRIu64 ",\"work\":%zu,\"scanned_cells\":%zu}",
               i, samples[i].ns, samples[i].work, samples[i].scanned_cells);
    }
    printf("]}");
}

int main(void) {
    generated_row rows[DOCUMENTS];
    experiment cases[] = {
        {"sparse-term", "title:rarebeacon LIMIT 10000", 0, 0, {{0}}, {{0}}},
        {"common-term", "title:common LIMIT 10000", DOCUMENTS, 3, {{0}}, {{0}}},
        {"ordered-phrase", "title:\"cedar quartz\" LIMIT 10000", 0, 1, {{0}}, {{0}}},
        {"substring", "body:substr(\"needle-marker\") LIMIT 10000", 0, 2, {{0}}, {{0}}}
    };
    nx_buf jsonl, bytes; nx_buf_init(&jsonl); nx_buf_init(&bytes);
    nx_table table; nx_search_index *index = NULL;
    uint64_t build_start = 0, build_end = 0;
    size_t memory_bytes = 0;
    int exit_code = 1;
    for (uint32_t row = 0; row < DOCUMENTS; ++row) {
        rows[row].sparse = row % 97u == 0;
        rows[row].phrase = row % 10u == 0;
        rows[row].needle = row % 101u == 0;
        if (rows[row].sparse) cases[0].expected++;
        if (rows[row].phrase) cases[2].expected++;
        if (rows[row].needle) cases[3].expected++;
        nx_buf_printf(&jsonl, "{\"_id\":\"doc-%05" PRIu32 "\",\"title\":\"common%s%s%s\",\"body\":\"shared document%s\"}\n",
                      row, rows[row].sparse ? " rarebeacon" : "",
                      rows[row].phrase ? " cedar quartz" : row % 10u == 1u ? " quartz cedar" : "",
                      row % 13u == 0 ? " common" : "",
                      rows[row].needle ? " needle-marker" : " steady-text");
    }
    if (jsonl.oom) { fputs("Corpus allocation failed\n", stderr); goto done; }
    nx_error error; nx_error_clear(&error);
    nx_status status = nx_table_build(nx_buf_slice(&jsonl), NULL, &bytes, &error);
    if (status == NX_OK) status = nx_table_open(nx_buf_slice(&bytes), &table, &error);
    if (status != NX_OK) {
        fprintf(stderr, "Snapshot failed: %s: %s\n", nx_status_str(status), error.msg);
        goto done;
    }
    build_start = nx_bench_time_now();
    status = nx_search_index_build(&table, &index, &error);
    build_end = nx_bench_time_now();
    if (status != NX_OK) {
        fprintf(stderr, "Prepared index build failed: %s: %s\n", nx_status_str(status), error.msg);
        goto done;
    }
    memory_bytes = nx_search_index_memory_bytes(index);
    for (size_t c = 0; c < NX_ARRAY_LEN(cases); ++c) {
        for (size_t i = 0; i < WARMUPS; ++i)
            if (!run_pair(&table, index, rows, &cases[c], cases[c].oracle_kind,
                          i % 2u == 0, NULL, NULL)) goto done;
        for (size_t i = 0; i < PAIRS; ++i)
            if (!run_pair(&table, index, rows, &cases[c], cases[c].oracle_kind, i % 2u == 0,
                          &cases[c].prepared[i], &cases[c].scan[i])) goto done;
    }
    printf("{\"schema\":1,\"benchmark\":\"snapshot-prepared-text-vs-scan\","
           "\"documents\":%u,\"generator\":\"fixed modulo schedule: sparse term every 97 rows, ordered phrase every 10 rows, substring every 101 rows\","
           "\"snapshot_bytes\":%zu,\"prepared_memory_bytes\":%zu,"
           "\"prepared_build_ns\":%" PRIu64 ",\"warmup_pairs_per_case\":%u,"
           "\"measured_pairs_per_case\":%u,\"order\":\"even pair prepared first; odd pair scan first\","
           "\"timing_scope\":\"nx_search only; text-index build reported separately; validation/result-free outside timing\","
           "\"validation\":\"Every result checked against generated membership; paired IDs, order and BM25 scores compared\","
           "\"limitations\":\"Single-process warm-cache synthetic text workload; not cold-cache, concurrency, memory-residency or cross-platform evidence\","
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
           "\"cases\":[", DOCUMENTS, bytes.len, memory_bytes,
           (uint64_t)(nx_bench_elapsed_sec(build_start, build_end) * 1e9 + 0.5), WARMUPS, PAIRS);
    for (size_t c = 0; c < NX_ARRAY_LEN(cases); ++c) {
        const experiment *e = &cases[c];
        if (c) putchar(',');
        printf("{\"name\":"); json_string(e->name);
        printf(",\"query\":"); json_string(e->query);
        printf(",\"expected_total\":%zu,\"prepared\":", e->expected);
        print_arm(e->prepared);
        printf(",\"scan\":"); print_arm(e->scan); putchar('}');
    }
    puts("]}");
    exit_code = ferror(stdout) ? 1 : 0;
done:
    nx_search_index_free(index); nx_buf_free(&bytes); nx_buf_free(&jsonl);
    return exit_code;
}
