#include "engine/nx_search.h"
#include "core/nx_json.h"
#include "nx_test.h"

static const char *documents =
    "{\"_id\":\"c\",\"title\":\"cat\",\"n\":2026,\"on\":true,\"f\":1.5,\"v\":[1,0]}\n"
    "{\"_id\":\"a\",\"title\":\"cat cat\",\"n\":-9223372036854775808,\"on\":false,\"f\":2.5,\"v\":[0,1]}\n"
    "{\"_id\":\"b\",\"title\":\"dog cat\",\"n\":9223372036854775807,\"on\":true,\"f\":3.5,\"v\":[-1,0]}\n"
    "{\"_id\":\"d\",\"title\":\"CAT\",\"n\":null,\"on\":true}\n"
    "{\"_id\":\"e\",\"title\":\"dog\",\"n\":0,\"on\":false,\"v\":[0,0]}\n"
    "{\"_id\":\"z\",\"title\":\"\",\"unused\":null}\n";

static nx_status table_from(nx_buf *bytes, nx_table *table, const char *jsonl) {
    nx_buf_init(bytes);
    nx_status st = nx_table_build(nx_slice_cstr(jsonl), NULL, bytes, NULL);
    if (st == NX_OK) st = nx_table_open(nx_buf_slice(bytes), table, NULL);
    return st;
}
static void expect_ids(const nx_table *table, const char *query, const char *ids) {
    nx_search_result r; nx_error error;
    nx_status st = nx_search(table, nx_slice_cstr(query), NULL, &r, &error);
    NX_CHECK_MSG(st == NX_OK, "%s: %s", query, error.msg);
    if (st != NX_OK) return;
    nx_buf result; nx_buf_init(&result);
    for (size_t i = 0; i < r.count; i++) nx_buf_put(&result, r.hits[i].id.p, r.hits[i].id.n);
    NX_CHECK_EQ_S(nx_buf_cstr(&result), ids);
    nx_buf_free(&result); nx_search_result_free(&r);
}
static void test_typed_boolean_and_exact(void) {
    nx_buf bytes; nx_table t; NX_REQUIRE(table_from(&bytes, &t, documents) == NX_OK);
    expect_ids(&t, "n=2026", "c");
    expect_ids(&t, "n=-9223372036854775808", "a");
    expect_ids(&t, "n:>=9223372036854775807", "b");
    expect_ids(&t, "n!=2026", "abe");
    expect_ids(&t, "NOT n=2026", "abdez");
    expect_ids(&t, "n:*", "abce");
    expect_ids(&t, "n:-1..2026", "ce");
    expect_ids(&t, "n:(0 OR 2026)", "ce");
    expect_ids(&t, "on:true AND (n<1 OR NOT n:*)", "d");
    expect_ids(&t, "f:>1.5", "ab");
    expect_ids(&t, "title=cat", "c");
    expect_ids(&t, "title=(cat OR dog)", "ce");
    expect_ids(&t, "title=\"\"", "z");
    expect_ids(&t, "unused:*", "");
    expect_ids(&t, "* SORT BY n DESC", "bceadz");
    expect_ids(&t, "* SORT BY n ASC LIMIT 2 OFFSET 1", "ec");
    nx_buf_free(&bytes);
}

static void test_bm25_full_corpus_and_phrases(void) {
    nx_buf bytes; nx_table t; NX_REQUIRE(table_from(&bytes, &t, documents) == NX_OK);
    nx_search_result r, filtered;
    NX_CHECK_OK(nx_search(&t, nx_slice_cstr("title:cat"), NULL, &r, NULL));
    NX_CHECK_EQ_U(r.total, 4);
    /* Field population=5, df(cat)=4, avgdl=7/5; uppercase folds identically. */
    for (size_t i = 0; i < r.count; i++) {
        double tf = r.hits[i].id.p[0] == 'a' ? 2 : 1;
        double dl = r.hits[i].id.p[0] == 'a' || r.hits[i].id.p[0] == 'b' ? 2 : 1;
        double expected = log(1.0 + 1.5 / 4.5) * tf / (tf + 1.2 * (.25 + .75 * dl / 1.4));
        NX_CHECK_NEAR(r.hits[i].score, expected, 1e-12);
    }
    NX_CHECK_OK(nx_search(&t, nx_slice_cstr("title:cat AND on:true"), NULL, &filtered, NULL));
    for (size_t i = 0; i < filtered.count; i++) for (size_t j = 0; j < r.count; j++)
        if (nx_slice_eq(filtered.hits[i].id, r.hits[j].id)) NX_CHECK_NEAR(filtered.hits[i].score, r.hits[j].score, 1e-14);
    nx_search_result_free(&r); nx_search_result_free(&filtered);
    expect_ids(&t, "title:\"dog cat\"", "b");
    expect_ids(&t, "title:prefix(ca) SORT BY _id", "abcd");
    expect_ids(&t, "title:fuzzy(cot,1) SORT BY _id", "abcd");
    expect_ids(&t, "title:substr(og)", "be");
    expect_ids(&t, "title:/^cat$/i", "cd");
    expect_ids(&t, "title:/c.t/ SORT BY _id", "abc");
    expect_ids(&t, "title:regex(\"c.t\") SORT BY _id", "abc");
    nx_buf_free(&bytes);
}

static void test_vectors_hybrid_and_branch_scores(void) {
    nx_buf bytes; nx_table t; NX_REQUIRE(table_from(&bytes, &t, documents) == NX_OK);
    nx_search_result r;
    NX_CHECK_OK(nx_search(&t, nx_slice_cstr("v:[1,0] AND on:true"), NULL, &r, NULL));
    NX_CHECK_EQ_U(r.count, 2); NX_CHECK_EQ_U(r.vectors_scored, 2);
    NX_CHECK_NEAR(r.hits[0].score, 1, 1e-15); NX_CHECK_NEAR(r.hits[1].score, -1, 1e-15);
    nx_search_result_free(&r);
    nx_search_result same;
    NX_CHECK_OK(nx_search(&t, nx_slice_cstr("title:* OR v:[1,0]"), NULL, &r, NULL));
    NX_CHECK_OK(nx_search(&t, nx_slice_cstr("title:(* OR *) OR v:[1,0]"), NULL, &same, NULL));
    NX_CHECK_EQ_U(r.count, same.count);
    for (size_t i = 0; i < r.count && i < same.count; i++) {
        NX_CHECK(nx_slice_eq(r.hits[i].id, same.hits[i].id));
        NX_CHECK_NEAR(r.hits[i].score, same.hits[i].score, 0);
    }
    nx_search_result_free(&r); nx_search_result_free(&same);
    NX_CHECK_OK(nx_search(&t, nx_slice_cstr("title:cat AND v:[1,0]"), NULL, &r, NULL));
    NX_CHECK_EQ_U(r.count, 3);
    for (size_t i = 0; i < r.count; i++) {
        char id = (char)r.hits[i].id.p[0];
        uint32_t lexical_rank = id == 'a' ? 1u : id == 'c' ? 2u : 3u;
        uint32_t vector_rank = id == 'c' ? 1u : id == 'a' ? 2u : 3u;
        NX_CHECK_NEAR(r.hits[i].score, 1.0 / (60 + lexical_rank) + 1.0 / (60 + vector_rank), 1e-15);
    }
    nx_search_result_free(&r);
    expect_ids(&t, "NOT NOT on:true", "bcd");
    NX_CHECK_OK(nx_search(&t, nx_slice_cstr("(on:false AND v:[1,0]) OR _id=c"), NULL, &r, NULL));
    NX_CHECK_EQ_U(r.vectors_scored, 2); /* c qualifies through a filter, not the vector branch. */
    nx_search_result_free(&r); nx_buf_free(&bytes);
}

static void test_bind_before_execute_limits_and_explain(void) {
    nx_buf bytes; nx_table t; NX_REQUIRE(table_from(&bytes, &t, documents) == NX_OK);
    const char *bad[] = {"n=123 AND missing:yes", "* OR n:abc", "EXPLAIN * OR n:abc",
        "title<cat", "WATCH *", "SOURCE(local) *", "* FACET on", "n:1MB", "n:2026-01-01",
        "v:[1]", "v:[1,0] OR v:[1]", "unused:1", "n~2", "* SORT BY v", "title:near(cat,2)",
        "title:regex(prefix(a))", "EXPLAIN title:regex(prefix(a))"};
    for (size_t i = 0; i < NX_ARRAY_LEN(bad); i++) {
        nx_search_result r; nx_error error;
        nx_status st = nx_search(&t, nx_slice_cstr(bad[i]), NULL, &r, &error);
        NX_CHECK_MSG(st != NX_OK, "Unexpected success: %s", bad[i]);
        NX_CHECK(r.hits == NULL && r.count == 0); NX_CHECK_EQ_I(error.code, st);
    }
    nx_search_result r;
    NX_CHECK_OK(nx_search(&t, nx_slice_cstr("EXPLAIN n:>=2026"), NULL, &r, NULL));
    NX_CHECK(r.explain_only); NX_CHECK_EQ_U(r.work, 0); NX_CHECK_EQ_U(r.numeric_indexes, 1);
    nx_search_result_free(&r);
    nx_search_options o = nx_search_default_options(); o.max_work = 2;
    NX_CHECK_EQ_I(nx_search(&t, nx_slice_cstr("title:cat"), &o, &r, NULL), NX_ERR_LIMIT);
    NX_CHECK(r.hits == NULL);
    nx_buf_free(&bytes);
}

static void test_random_numeric_independent_oracle(void) {
    nx_rng rng = nx_test_rng(3489); nx_buf docs; nx_buf_init(&docs);
    int64_t values[180]; bool present[180], enabled[180];
    for (size_t i = 0; i < 180; i++) {
        values[i] = (int64_t)(nx_rng_u64(&rng) % 20001) - 10000;
        present[i] = nx_rng_u64(&rng) % 4 != 0; enabled[i] = nx_rng_u64(&rng) % 2 != 0;
        nx_buf_printf(&docs, "{\"_id\":\"%03zu\",\"enabled\":%s,\"n\":", i, enabled[i] ? "true" : "false");
        if (present[i]) nx_buf_printf(&docs, "%lld", (long long)values[i]); else nx_buf_put_str(&docs, "null");
        nx_buf_put_str(&docs, "}\n");
    }
    nx_buf bytes; nx_table t; NX_REQUIRE(table_from(&bytes, &t, nx_buf_cstr(&docs)) == NX_OK);
    for (size_t iteration = 0; iteration < nx_test_iters(120); iteration++) {
        int64_t lo = (int64_t)(nx_rng_u64(&rng) % 20001) - 10000, hi = lo + 1000;
        char query[160]; (void)snprintf(query, sizeof(query), "(n:>=%lld AND n:<%lld AND enabled:true) OR NOT n:* LIMIT 1000", (long long)lo, (long long)hi);
        nx_search_options options = nx_search_default_options(); nx_search_result fast, scan;
        NX_CHECK_OK(nx_search(&t, nx_slice_cstr(query), &options, &fast, NULL));
        options.indexed = false;
        NX_CHECK_OK(nx_search(&t, nx_slice_cstr(query), &options, &scan, NULL));
        size_t n = 0;
        for (uint32_t row = 0; row < 180; row++) if (!present[row] || (enabled[row] && values[row] >= lo && values[row] < hi)) {
            NX_CHECK(n < fast.count && n < scan.count);
            if (n < fast.count && n < scan.count) {
                NX_CHECK_EQ_U(fast.hits[n].row, row); NX_CHECK_EQ_U(scan.hits[n].row, row);
                NX_CHECK_NEAR(fast.hits[n].score, scan.hits[n].score, 0);
            }
            n++;
        }
        NX_CHECK_EQ_U(fast.total, n); NX_CHECK_EQ_U(scan.total, n);
        nx_search_result_free(&fast); nx_search_result_free(&scan);
    }
    nx_buf_free(&bytes); nx_buf_free(&docs);
}

static nx_status search_oom(void *opaque) {
    nx_search_result result; nx_buf json; nx_buf_init(&json);
    nx_status st = nx_search((const nx_table *)opaque, nx_slice_cstr("(title:cat AND v:[1,0]) OR n<0"), NULL, &result, NULL);
    if (st == NX_OK) st = nx_search_json(&result, &json);
    nx_search_result_free(&result); nx_buf_free(&json); return st;
}
static void test_search_oom_and_json(void) {
    nx_buf bytes; nx_table t; NX_REQUIRE(table_from(&bytes, &t, documents) == NX_OK);
    nx_test_oom_sweep(search_oom, &t, 800);
    nx_search_result r; nx_buf json; nx_buf_init(&json);
    NX_CHECK_OK(nx_search(&t, nx_slice_cstr("title:cat"), NULL, &r, NULL));
    NX_CHECK_OK(nx_search_json(&r, &json));
    nx_arena arena; nx_arena_init(&arena, 0); nx_json *parsed = NULL;
    NX_CHECK_OK(nx_json_parse(&arena, nx_buf_slice(&json), NULL, &parsed, NULL));
    const nx_json *hits = nx_json_get_cstr(parsed, "hits");
    NX_CHECK(hits && hits->count == r.count);
    NX_CHECK(nx_json_get_cstr(nx_json_at(hits, 0), "score") != NULL);
    nx_arena_free(&arena); nx_buf_free(&json); nx_search_result_free(&r); nx_buf_free(&bytes);
}
int main(void) {
    NX_RUN(test_typed_boolean_and_exact);
    NX_RUN(test_bm25_full_corpus_and_phrases);
    NX_RUN(test_vectors_hybrid_and_branch_scores);
    NX_RUN(test_bind_before_execute_limits_and_explain);
    NX_RUN(test_random_numeric_independent_oracle);
    NX_RUN(test_search_oom_and_json);
    return nx_test_summary();
}
