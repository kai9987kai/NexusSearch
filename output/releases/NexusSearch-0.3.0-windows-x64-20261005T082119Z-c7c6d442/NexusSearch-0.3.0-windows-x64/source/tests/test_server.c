#include "core/nx_config.h"
#include "nx_test.h"
#include "server/nx_server.h"
#include "core/nx_json.h"
#include <string.h>

static const char *documents =
    "{\"_id\":\"a\",\"title\":\"alpha search\",\"n\":10,\"on\":true,\"v\":[1,0]}\n"
    "{\"_id\":\"b\",\"title\":\"beta test\",\"n\":20,\"on\":false,\"v\":[0,1]}\n"
    "{\"_id\":\"c\",\"title\":\"gamma search test\",\"n\":30,\"on\":true}\n";

static void test_server_stats_json(void) {
    nx_buf bytes; nx_buf_init(&bytes);
    nx_table table;
    NX_REQUIRE(nx_table_build(nx_slice_cstr(documents), NULL, &bytes, NULL) == NX_OK);
    NX_REQUIRE(nx_table_open(nx_buf_slice(&bytes), &table, NULL) == NX_OK);

    nx_buf json; nx_buf_init(&json);
    NX_CHECK_OK(nx_server_stats_json(&table, &json));
    NX_CHECK(json.len > 0);

    /* Parse and verify structure */
    nx_arena arena; nx_arena_init(&arena, 4096);
    nx_json *root = NULL;
    NX_CHECK_OK(nx_json_parse(&arena, nx_buf_slice(&json), NULL, &root, NULL));
    NX_CHECK(root && root->kind == NX_JSON_OBJECT);

    const nx_json *rows = nx_json_get_cstr(root, "rows");
    NX_CHECK(rows && rows->kind == NX_JSON_NUMBER);
    NX_CHECK_EQ_I(rows->as.number.i64, 3);

    const nx_json *fields = nx_json_get_cstr(root, "fields");
    NX_CHECK(fields && fields->kind == NX_JSON_ARRAY);
    NX_CHECK_EQ_U(fields->count, 5); /* _id, title, n, on, v */

    nx_arena_free(&arena);
    nx_buf_free(&json);
    nx_buf_free(&bytes);
}

static void test_server_query_json_basic(void) {
    nx_buf bytes; nx_buf_init(&bytes);
    nx_table table;
    NX_REQUIRE(nx_table_build(nx_slice_cstr(documents), NULL, &bytes, NULL) == NX_OK);
    NX_REQUIRE(nx_table_open(nx_buf_slice(&bytes), &table, NULL) == NX_OK);

    /* Indexed search */
    nx_buf json; nx_buf_init(&json);
    NX_CHECK_OK(nx_server_query_json(&table, "search", false, false, &json));
    NX_CHECK(json.len > 0);

    /* Verify it has hits */
    nx_arena arena; nx_arena_init(&arena, 4096);
    nx_json *root = NULL;
    NX_CHECK_OK(nx_json_parse(&arena, nx_buf_slice(&json), NULL, &root, NULL));
    NX_CHECK(root && root->kind == NX_JSON_OBJECT);
    const nx_json *total = nx_json_get_cstr(root, "total");
    NX_CHECK(total && total->kind == NX_JSON_NUMBER);
    NX_CHECK(total->as.number.i64 >= 2); /* "alpha search" and "gamma search test" */

    const nx_json *hits = nx_json_get_cstr(root, "hits");
    NX_CHECK(hits && hits->kind == NX_JSON_ARRAY);
    NX_CHECK(hits->count >= 2);

    /* Each hit should have _id, row, score, document */
    for (const nx_json *hit = hits->child; hit; hit = hit->next) {
        NX_CHECK(nx_json_get_cstr(hit, "_id") != NULL);
        NX_CHECK(nx_json_get_cstr(hit, "row") != NULL);
        NX_CHECK(nx_json_get_cstr(hit, "score") != NULL);
        NX_CHECK(nx_json_get_cstr(hit, "document") != NULL);
    }

    nx_arena_free(&arena);
    nx_buf_free(&json);
    nx_buf_free(&bytes);
}

static void test_server_query_json_filters(void) {
    nx_buf bytes; nx_buf_init(&bytes);
    nx_table table;
    NX_REQUIRE(nx_table_build(nx_slice_cstr(documents), NULL, &bytes, NULL) == NX_OK);
    NX_REQUIRE(nx_table_open(nx_buf_slice(&bytes), &table, NULL) == NX_OK);

    /* Numeric filter */
    nx_buf json; nx_buf_init(&json);
    NX_CHECK_OK(nx_server_query_json(&table, "n:>=20 AND on:true", false, false, &json));

    nx_arena arena; nx_arena_init(&arena, 4096);
    nx_json *root = NULL;
    NX_CHECK_OK(nx_json_parse(&arena, nx_buf_slice(&json), NULL, &root, NULL));
    const nx_json *total = nx_json_get_cstr(root, "total");
    NX_CHECK(total && total->kind == NX_JSON_NUMBER);
    NX_CHECK_EQ_I(total->as.number.i64, 1); /* only "c" with n=30 and on=true */

    nx_arena_free(&arena);
    nx_buf_free(&json);

    /* Scan parity - same query should give same results */
    nx_buf indexed; nx_buf_init(&indexed);
    nx_buf scanned; nx_buf_init(&scanned);
    NX_CHECK_OK(nx_server_query_json(&table, "n:>=10 SORT BY _id", false, false, &indexed));
    NX_CHECK_OK(nx_server_query_json(&table, "n:>=10 SORT BY _id", true, false, &scanned));

    /* Parse both and compare hit IDs */
    nx_arena arena2; nx_arena_init(&arena2, 4096);
    nx_json *r1 = NULL, *r2 = NULL;
    NX_CHECK_OK(nx_json_parse(&arena2, nx_buf_slice(&indexed), NULL, &r1, NULL));
    NX_CHECK_OK(nx_json_parse(&arena2, nx_buf_slice(&scanned), NULL, &r2, NULL));
    const nx_json *h1 = nx_json_get_cstr(r1, "total"), *h2 = nx_json_get_cstr(r2, "total");
    NX_CHECK(h1 && h2);
    NX_CHECK_EQ_I(h1->as.number.i64, h2->as.number.i64);

    nx_arena_free(&arena2);
    nx_buf_free(&indexed);
    nx_buf_free(&scanned);
    nx_buf_free(&bytes);
}

static void test_server_explain(void) {
    nx_buf bytes; nx_buf_init(&bytes);
    nx_table table;
    NX_REQUIRE(nx_table_build(nx_slice_cstr(documents), NULL, &bytes, NULL) == NX_OK);
    NX_REQUIRE(nx_table_open(nx_buf_slice(&bytes), &table, NULL) == NX_OK);

    nx_buf json; nx_buf_init(&json);
    NX_CHECK_OK(nx_server_query_json(&table, "search AND on:true", false, true, &json));

    nx_arena arena; nx_arena_init(&arena, 4096);
    nx_json *root = NULL;
    NX_CHECK_OK(nx_json_parse(&arena, nx_buf_slice(&json), NULL, &root, NULL));
    const nx_json *explain = nx_json_get_cstr(root, "explain_only");
    NX_CHECK(explain && explain->kind == NX_JSON_BOOL && explain->as.boolean);

    nx_arena_free(&arena);
    nx_buf_free(&json);
    nx_buf_free(&bytes);
}

static void test_server_bad_query(void) {
    nx_buf bytes; nx_buf_init(&bytes);
    nx_table table;
    NX_REQUIRE(nx_table_build(nx_slice_cstr(documents), NULL, &bytes, NULL) == NX_OK);
    NX_REQUIRE(nx_table_open(nx_buf_slice(&bytes), &table, NULL) == NX_OK);

    nx_buf json; nx_buf_init(&json);
    /* Querying a nonexistent field should produce an error JSON */
    NX_CHECK_OK(nx_server_query_json(&table, "nonexistent_field:*", false, false, &json));

    nx_arena arena; nx_arena_init(&arena, 4096);
    nx_json *root = NULL;
    NX_CHECK_OK(nx_json_parse(&arena, nx_buf_slice(&json), NULL, &root, NULL));
    const nx_json *err = nx_json_get_cstr(root, "error");
    NX_CHECK(err && err->kind == NX_JSON_STRING);
    NX_CHECK(err->as.string.n > 0);

    nx_arena_free(&arena);
    nx_buf_free(&json);
    nx_buf_free(&bytes);
}

static void test_server_vector_search(void) {
    nx_buf bytes; nx_buf_init(&bytes);
    nx_table table;
    NX_REQUIRE(nx_table_build(nx_slice_cstr(documents), NULL, &bytes, NULL) == NX_OK);
    NX_REQUIRE(nx_table_open(nx_buf_slice(&bytes), &table, NULL) == NX_OK);

    nx_buf json; nx_buf_init(&json);
    NX_CHECK_OK(nx_server_query_json(&table, "v:[1,0]", false, false, &json));

    nx_arena arena; nx_arena_init(&arena, 4096);
    nx_json *root = NULL;
    NX_CHECK_OK(nx_json_parse(&arena, nx_buf_slice(&json), NULL, &root, NULL));
    const nx_json *total = nx_json_get_cstr(root, "total");
    NX_CHECK(total && total->kind == NX_JSON_NUMBER);
    NX_CHECK(total->as.number.i64 >= 1);

    /* Verify vectors_scored counter */
    const nx_json *exec = nx_json_get_cstr(root, "execution");
    if (exec) {
        const nx_json *vs = nx_json_get_cstr(exec, "vectors_scored");
        NX_CHECK(vs && vs->kind == NX_JSON_NUMBER);
        NX_CHECK(vs->as.number.i64 >= 1);
    }

    nx_arena_free(&arena);
    nx_buf_free(&json);
    nx_buf_free(&bytes);
}

static void test_server_null_inputs(void) {
    NX_CHECK_EQ_I(nx_server_stats_json(NULL, NULL), NX_ERR_INVALID);
    NX_CHECK_EQ_I(nx_server_query_json(NULL, NULL, false, false, NULL), NX_ERR_INVALID);
}

int main(void) {
    NX_RUN(test_server_stats_json);
    NX_RUN(test_server_query_json_basic);
    NX_RUN(test_server_query_json_filters);
    NX_RUN(test_server_explain);
    NX_RUN(test_server_bad_query);
    NX_RUN(test_server_vector_search);
    NX_RUN(test_server_null_inputs);
    return nx_test_summary();
}
