#include "query/nx_date.h"
#include "query/nx_parse.h"
#include "engine/nx_search.h"
#include "nx_test.h"

static const char *source =
    "{\"_id\":\"a\",\"when\":\"2024-02-28T23:00:00Z\"}\n"
    "{\"_id\":\"b\",\"when\":\"2024-02-29\"}\n"
    "{\"_id\":\"c\",\"when\":\"2024-03-01T00:00:00+01:00\"}\n"
    "{\"_id\":\"d\",\"when\":\"2024-03-01T00:00:00Z\"}\n"
    "{\"_id\":\"e\",\"when\":\"not-a-date\"}\n"
    "{\"_id\":\"f\",\"when\":null}\n"
    "{\"_id\":\"g\",\"when\":\"2023-12-31\"}\n";

static void test_calendar_and_relative_ranges(void) {
    nx_date_range range;
    nx_datetime dt = {0}; dt.year = 2024; dt.month = 2; dt.prec = NX_DT_PREC_MONTH;
    NX_CHECK_OK(nx_date_resolve(&dt, 0, &range));
    NX_CHECK_EQ_I(range.lo.seconds, 1706745600LL);
    NX_CHECK_EQ_I(range.end.seconds - range.lo.seconds, 29 * 86400);
    dt.year = 2023;
    NX_CHECK_OK(nx_date_resolve(&dt, 0, &range));
    NX_CHECK_EQ_I(range.end.seconds - range.lo.seconds, 28 * 86400);

    nx_instant utc, offset;
    NX_CHECK_OK(nx_date_read(nx_slice_cstr("2024-02-29T23:00:00Z"), &utc));
    NX_CHECK_OK(nx_date_read(nx_slice_cstr("2024-03-01T00:00:00+01:00"), &offset));
    NX_CHECK_EQ_I(utc.seconds, offset.seconds);
    NX_CHECK_EQ_U(utc.nanos, offset.nanos);
    NX_CHECK_OK(nx_date_read(nx_slice_cstr("2024-02-29T23:59:59.123456789Z"), &utc));
    NX_CHECK_EQ_U(utc.nanos, 123456789);
    NX_CHECK_EQ_I(nx_date_read(nx_slice_cstr("2023-02-29"), &utc), NX_ERR_TYPE);
    NX_CHECK_EQ_I(nx_date_read(nx_slice_cstr("2024-01-01T24:00:00Z"), &utc), NX_ERR_TYPE);
    NX_CHECK_EQ_I(nx_date_read(nx_slice_cstr("2024-01-01T00:00:00.1234567890Z"), &utc), NX_ERR_TYPE);

    dt = (nx_datetime){0}; dt.year = 2024; dt.month = 2; dt.day = 29; dt.prec = NX_DT_PREC_DAY;
    NX_CHECK_OK(nx_date_resolve(&dt, 0, &range));
    NX_CHECK(nx_date_matches((nx_instant){range.lo.seconds - 1, 999999999}, &range, NX_OP_LT));
    NX_CHECK(nx_date_matches(range.lo, &range, NX_OP_EQ));
    NX_CHECK(nx_date_matches((nx_instant){range.end.seconds - 1, 999999999}, &range, NX_OP_LE));
    NX_CHECK(!nx_date_matches(range.end, &range, NX_OP_LE));
    NX_CHECK(nx_date_matches(range.end, &range, NX_OP_GT));
    NX_CHECK(!nx_date_matches(range.end, &range, NX_OP_EQ));

    NX_CHECK_OK(nx_date_read(nx_slice_cstr("2024-02-29T12:30:00Z"), &utc));
    dt = (nx_datetime){0}; dt.base_rel = NX_REL_TODAY;
    dt.rel_unit = NX_UNIT_MONTH; dt.rel_amount = 1;
    NX_CHECK_OK(nx_date_resolve(&dt, utc.seconds, &range));
    NX_CHECK_EQ_I(range.lo.seconds, 1711670400LL); /* 2024-03-29 00:00 UTC */
    NX_CHECK_EQ_I(range.end.seconds - range.lo.seconds, 86400);
    dt = (nx_datetime){0}; dt.base_rel = NX_REL_NOW; dt.rel_unit = NX_UNIT_SECOND; dt.rel_amount = -30;
    NX_CHECK_OK(nx_date_resolve(&dt, utc.seconds, &range));
    NX_CHECK_EQ_I(range.lo.seconds, utc.seconds - 30);
    NX_CHECK_EQ_I(range.end.seconds - range.lo.seconds, 1);
    NX_CHECK(nx_date_matches((nx_instant){range.lo.seconds, 0}, &range, NX_OP_EQ));
    NX_CHECK(!nx_date_matches(range.end, &range, NX_OP_EQ));
}

static void expect_ids(const nx_table *table, const char *query, const char *expected) {
    nx_search_result result = {0}; nx_error error;
    nx_status st = nx_search(table, nx_slice_cstr(query), NULL, &result, &error);
    NX_CHECK_MSG(st == NX_OK, "date query failed: %s", query);
    if (st != NX_OK) return;
    nx_buf ids; nx_buf_init(&ids);
    for (size_t i = 0; i < result.count; i++) nx_buf_put(&ids, result.hits[i].id.p, result.hits[i].id.n);
    if (!nx_slice_eq(nx_buf_slice(&ids), nx_slice_cstr(expected)))
        fprintf(stderr, "    date result %.80s got %.40s expected %.40s\n",
                query, nx_buf_cstr(&ids), expected);
    NX_CHECK(nx_slice_eq(nx_buf_slice(&ids), nx_slice_cstr(expected)));
    nx_buf_free(&ids); nx_search_result_free(&result);
}

static void test_date_queries_on_text_columns(void) {
    nx_buf bytes; nx_buf_init(&bytes); nx_table table;
    NX_REQUIRE(nx_table_build(nx_slice_cstr(source), NULL, &bytes, NULL) == NX_OK);
    NX_REQUIRE(nx_table_open(nx_buf_slice(&bytes), &table, NULL) == NX_OK);
    expect_ids(&table, "when:2024", "abcd");
    expect_ids(&table, "when:2024-02", "abc");
    expect_ids(&table, "when:>=2024-02-29", "bcd");
    expect_ids(&table, "when:>2024-02-29", "d");
    expect_ids(&table, "when:<2024-02-29", "ag");
    expect_ids(&table, "when:<=2024-02-29", "abcg");
    expect_ids(&table, "when:>=2024-03-01", "d");
    expect_ids(&table, "when:<2024-03", "abcg");
    expect_ids(&table, "when:2024-02-29..2024-03-01", "bcd");
    expect_ids(&table, "2024-02-29", "bc");

    nx_search_result result = {0};
    NX_CHECK(nx_search(&table, nx_slice_cstr("when:2023-02-29"), NULL, &result, NULL) != NX_OK);
    NX_CHECK(result.hits == NULL && result.count == 0);
    /* Relative date math is parsed and bound against one captured query clock. */
    NX_CHECK_OK(nx_search(&table, nx_slice_cstr("when:now-9000d"), NULL, &result, NULL));
    NX_CHECK_EQ_U(result.total, 0);
    nx_search_result_free(&result);
    NX_CHECK_OK(nx_search(&table, nx_slice_cstr("when:today+1mo"), NULL, &result, NULL));
    nx_search_result_free(&result);
    nx_buf_free(&bytes);
}

int main(void) {
    NX_RUN(test_calendar_and_relative_ranges);
    NX_RUN(test_date_queries_on_text_columns);
    return nx_test_summary();
}
