#include "engine/nx_search.h"
#include "engine/nx_search_index.h"
#include "core/nx_json.h"
#include "nx_test.h"

static const char *source =
    "{\"_id\":\"a\",\"title\":\"Cat cat café\",\"body\":\"the quick brown fox\",\"n\":2}\n"
    "{\"_id\":\"b\",\"title\":\"CAT catalog\",\"body\":\"quick red fox\",\"n\":1}\n"
    "{\"_id\":\"c\",\"title\":\"dog and Cat\",\"body\":\"brown fox jumps\",\"n\":2}\n"
    "{\"_id\":\"d\",\"title\":\"fullwidth ＣＡＴ\",\"body\":\"a fox\",\"n\":null}\n"
    "{\"_id\":\"e\",\"title\":\"\",\"body\":null,\"n\":0}\n";

static nx_status make_table(nx_buf *bytes, nx_table *table) {
    nx_buf_init(bytes);
    nx_status st = nx_table_build(nx_slice_cstr(source), NULL, bytes, NULL);
    return st == NX_OK ? nx_table_open(nx_buf_slice(bytes), table, NULL) : st;
}

static bool same_hits(const nx_search_result *a, const nx_search_result *b) {
    if (a->total != b->total || a->count != b->count) return false;
    for (size_t i = 0; i < a->count; i++) {
        if (a->hits[i].row != b->hits[i].row || !nx_slice_eq(a->hits[i].id, b->hits[i].id) ||
            fabs(a->hits[i].score - b->hits[i].score) > 1e-13) return false;
    }
    return true;
}

static void test_prepared_exact_parity(void) {
    nx_buf bytes; nx_table table;
    NX_REQUIRE(make_table(&bytes, &table) == NX_OK);
    nx_search_index *index = NULL;
    NX_CHECK_OK(nx_search_index_build(&table, &index, NULL));
    NX_REQUIRE(index != NULL);
    NX_CHECK(nx_search_index_memory_bytes(index) > 0);
    const char *queries[] = {
        "title:cat", "title:cat cat", "title:\"cat cat\"", "title:\"cat café\"",
        "title:prefix(cat)", "title:cat*", "title:substr(atal)", "body:substr(quick)",
        "title:substr(ZZZ)", "title:prefix(full)", "title:fuzzy(cot,1)",
        "title:/^cat/i", "cat", "title:cat AND n:>=2", "title:cat OR body:fox",
        "title:substr(at) SORT BY _id DESC", "* SORT BY n DESC LIMIT 4"
    };
    const bool expect_prepared[] = {true, true, true, true, true, true, true, true, true,
                                    true, false, false, true, true, true, false, false};
    for (size_t i = 0; i < NX_ARRAY_LEN(queries); i++) {
        nx_search_result scan = {0}, prepared = {0}; nx_error error;
        nx_status scan_status = nx_search(&table, nx_slice_cstr(queries[i]), NULL, &scan, &error);
        NX_CHECK_MSG(scan_status == NX_OK, "scan query: %s", queries[i]);
        if (scan_status != NX_OK) continue;
        nx_status prepared_status = nx_search_index_search(index, nx_slice_cstr(queries[i]), NULL, &prepared, &error);
        NX_CHECK_MSG(prepared_status == NX_OK, "prepared query: %s", queries[i]);
        if (prepared_status != NX_OK) { nx_search_result_free(&scan); continue; }
        NX_CHECK_MSG(same_hits(&scan, &prepared), "prepared parity: %s", queries[i]);
        NX_CHECK_MSG(prepared.prepared_text == expect_prepared[i], "mode query: %s", queries[i]);
        nx_search_result_free(&scan); nx_search_result_free(&prepared);
    }
    nx_search_options options = nx_search_default_options(); options.indexed = false;
    nx_search_result scan = {0};
    NX_CHECK_OK(nx_search_index_search(index, nx_slice_cstr("title:cat"), &options, &scan, NULL));
    NX_CHECK(!scan.prepared_text);
    nx_search_result_free(&scan);
    nx_search_index_free(index); nx_buf_free(&bytes);
}

typedef struct index_build_ctx { nx_table *table; } index_build_ctx;
static nx_status build_index_oom(void *opaque) {
    index_build_ctx *ctx = (index_build_ctx *)opaque;
    nx_search_index *index = NULL;
    nx_status st = nx_search_index_build(ctx->table, &index, NULL);
    nx_search_index_free(index);
    return st;
}

static void test_prepared_limits_and_oom(void) {
    nx_buf bytes; nx_table table;
    NX_REQUIRE(make_table(&bytes, &table) == NX_OK);
    index_build_ctx ctx = {&table};
    nx_test_oom_sweep(build_index_oom, &ctx, 256);
    nx_search_index *index = NULL;
    NX_REQUIRE(nx_search_index_build(&table, &index, NULL) == NX_OK);
    nx_search_options options = nx_search_default_options(); options.max_work = 1;
    nx_search_result result = {0};
    NX_CHECK_EQ_I(nx_search_index_search(index, nx_slice_cstr("title:cat"), &options, &result, NULL), NX_ERR_LIMIT);
    NX_CHECK(result.hits == NULL && result.count == 0);
    NX_CHECK_EQ_I(nx_search_index_search(NULL, nx_slice_cstr("*"), NULL, &result, NULL), NX_ERR_INVALID);
    nx_search_index_free(index); nx_buf_free(&bytes);
}

int main(void) {
    NX_RUN(test_prepared_exact_parity);
    NX_RUN(test_prepared_limits_and_oom);
    return nx_test_summary();
}
