#include "core/nx_config.h"
#include "nx_test.h"
#include "core/nx_json.h"

static void test_json_typed_binary_and_numbers(void) {
    char source[] = " {\"x\":[null,true,false,-9223372036854775808,9223372036854775807,"
                    "9223372036854775808,1.234567890123456789e400],\"a\\u0000b\":\"A\\u0000B\\ud83d\\ude00\"} ";
    nx_arena arena; nx_arena_init(&arena, 128); nx_json *root = NULL; nx_error error;
    NX_CHECK_OK(nx_json_parse(&arena, nx_slice_cstr(source), NULL, &root, &error));
    NX_REQUIRE(root != NULL);
    NX_CHECK_EQ_I(root->kind, NX_JSON_OBJECT); NX_CHECK_EQ_U(root->offset, 1); NX_CHECK_EQ_U(root->count, 2);
    const nx_json *arr = nx_json_get_cstr(root, "x"); NX_REQUIRE(arr != NULL);
    NX_CHECK_EQ_I(nx_json_at(arr, 0)->kind, NX_JSON_NULL);
    NX_CHECK(nx_json_at(arr, 1)->as.boolean); NX_CHECK(!nx_json_at(arr, 2)->as.boolean);
    int64_t number = 0;
    NX_CHECK_OK(nx_json_get_i64(nx_json_at(arr, 3), &number)); NX_CHECK_EQ_I(number, INT64_MIN);
    NX_CHECK_OK(nx_json_get_i64(nx_json_at(arr, 4), &number)); NX_CHECK_EQ_I(number, INT64_MAX);
    NX_CHECK_EQ_I(nx_json_get_i64(nx_json_at(arr, 5), &number), NX_ERR_TYPE);
    NX_CHECK(nx_slice_eq(nx_json_at(arr, 6)->as.number.raw, nx_slice_cstr("1.234567890123456789e400")));
    const nx_json *str = nx_json_get(root, nx_slice_make("a\0b", 3)); NX_REQUIRE(str != NULL);
    NX_CHECK(nx_slice_eq(str->as.string, nx_slice_make("A\0B\xf0\x9f\x98\x80", 7)));
    memset(source, '?', sizeof source - 1);
    NX_CHECK(nx_slice_eq(nx_json_at(arr, 6)->as.number.raw, nx_slice_cstr("1.234567890123456789e400")));
    nx_buf output; nx_buf_init(&output);
    NX_CHECK_OK(nx_json_serialize(root, NULL, &output, &error));
    NX_CHECK_EQ_S(nx_buf_cstr(&output), "{\"x\":[null,true,false,-9223372036854775808,9223372036854775807,"
        "9223372036854775808,1.234567890123456789e400],\"a\\u0000b\":\"A\\u0000B\xf0\x9f\x98\x80\"}");
    nx_buf_free(&output); nx_arena_free(&arena);
}

static void test_json_rejects_malformed(void) {
    const char *bad[] = {"", " ", "nul", "true false", "01", "-", "+1", "1.", "1e", "NaN", "Infinity",
        "[1,]", "{\"x\":1,}", "{x:1}", "{\"x\" 1}", "[1 2]", "\"a\n\"", "\"\\x12\"", "\"\\uD800\"",
        "\"\\uDC00\"", "\"\\uD800\\u0000\"", "\"\\uD800x\"", "\"\\uZZZZ\"", "\"\xc0\xaf\"",
        "\"\xed\xa0\x80\"", "{\"a\":1,\"\\u0061\":2}", "{\"a\\u0000\":1,\"a\\u0000\":2}", "/*x*/null"};
    nx_arena arena; nx_arena_init(&arena, 32);
    NX_REQUIRE(nx_arena_alloc(&arena, 12, 1) != NULL);
    for (size_t i = 0; i < NX_ARRAY_LEN(bad); ++i) {
        nx_json *root = NULL; nx_error error; size_t before = arena.total;
        nx_mem_stats mem = nx_mem_get_stats();
        NX_CHECK_MSG(nx_json_parse(&arena, nx_slice_cstr(bad[i]), NULL, &root, &error) == NX_ERR_PARSE,
                     "accepted malformed case %zu", i);
        NX_CHECK(root == NULL); NX_CHECK_EQ_U(arena.total, before);
        NX_CHECK_EQ_U(nx_mem_get_stats().live_allocs, mem.live_allocs);
        NX_CHECK(error.code == NX_ERR_PARSE && error.pos >= 0 && error.msg[0] != 0);
    }
    nx_arena_free(&arena);
}

static void test_json_bounds_and_rollback(void) {
    nx_arena arena; nx_arena_init(&arena, 64); nx_json *root = NULL; nx_error error;
    nx_json_limits limits = nx_json_default_limits(); limits.max_depth = 2;
    NX_CHECK_OK(nx_json_parse(&arena, nx_slice_cstr("[[]]"), &limits, &root, &error));
    NX_CHECK_EQ_I(nx_json_parse(&arena, nx_slice_cstr("[[[]]]"), &limits, &root, &error), NX_ERR_LIMIT);
    limits = nx_json_default_limits(); limits.max_nodes = 2;
    NX_CHECK_EQ_I(nx_json_parse(&arena, nx_slice_cstr("[1,2]"), &limits, &root, &error), NX_ERR_LIMIT);
    limits = nx_json_default_limits(); limits.max_string_bytes = 3;
    NX_CHECK_EQ_I(nx_json_parse(&arena, nx_slice_cstr("[\"ab\",\"cd\"]"), &limits, &root, &error), NX_ERR_LIMIT);
    limits = nx_json_default_limits(); limits.max_input_bytes = 2;
    NX_CHECK_EQ_I(nx_json_parse(&arena, nx_slice_cstr("null"), &limits, &root, &error), NX_ERR_LIMIT);
    limits = nx_json_default_limits(); limits.max_work = 1;
    NX_CHECK_EQ_I(nx_json_parse(&arena, nx_slice_cstr("{\"x\":1}"), &limits, &root, &error), NX_ERR_LIMIT);
    NX_CHECK_OK(nx_json_parse(&arena, nx_slice_cstr("{\"x\":[1,2]}"), NULL, &root, &error));
    nx_buf out; nx_buf_init(&out); nx_buf_put_str(&out, "prefix");
    limits = nx_json_default_limits(); limits.max_input_bytes = 3;
    NX_CHECK_EQ_I(nx_json_serialize(root, &limits, &out, &error), NX_ERR_LIMIT);
    NX_CHECK_EQ_S(nx_buf_cstr(&out), "prefix");
    nx_buf_free(&out); nx_arena_free(&arena);
}

static void test_json_integer_oracle_roundtrip(void) {
    nx_rng rng = nx_test_rng(91);
    for (size_t i = 0; i < nx_test_iters(2500); ++i) {
        uint64_t bits = nx_rng_u64(&rng); int64_t expected; memcpy(&expected, &bits, sizeof expected);
        char source[80]; (void)snprintf(source, sizeof source, "[ %lld, true, \"sample\\n\" ]", (long long)expected);
        nx_arena arena; nx_arena_init(&arena, 0); nx_json *first = NULL, *second = NULL;
        nx_buf out; nx_buf_init(&out); int64_t got = 0;
        NX_CHECK_OK(nx_json_parse(&arena, nx_slice_cstr(source), NULL, &first, NULL));
        NX_CHECK_OK(nx_json_get_i64(nx_json_at(first, 0), &got)); NX_CHECK_EQ_I(got, expected);
        NX_CHECK_OK(nx_json_serialize(first, NULL, &out, NULL));
        NX_CHECK_OK(nx_json_parse(&arena, nx_buf_slice(&out), NULL, &second, NULL));
        NX_CHECK_OK(nx_json_get_i64(nx_json_at(second, 0), &got)); NX_CHECK_EQ_I(got, expected);
        NX_CHECK(nx_slice_eq(nx_json_at(second, 2)->as.string, nx_slice_cstr("sample\n")));
        nx_buf_free(&out); nx_arena_free(&arena);
    }
}

static void test_json_mutation_fuzz(void) {
    const char valid[] = "{\"a\":[1,-2.5e+3,true,null,\"\\uD83D\\uDE00\"],\"b\":{\"q\":\"x\"}}";
    nx_rng rng = nx_test_rng(62);
    for (size_t i = 0; i < nx_test_iters(5000); ++i) {
        char input[sizeof valid * 2]; size_t n = sizeof valid - 1; memcpy(input, valid, n);
        switch (i % 3) {
        case 0: input[nx_rng_below(&rng, n)] = (char)nx_rng_below(&rng, 256); break;
        case 1: n = (size_t)nx_rng_below(&rng, n); break;
        default: { size_t at = (size_t)nx_rng_below(&rng, n);
            memmove(input + at + 3, input + at, n - at); memcpy(input + at, "[]}", 3); n += 3; break; }
        }
        nx_arena arena; nx_arena_init(&arena, 64); nx_json *root = NULL;
        nx_status st = nx_json_parse(&arena, nx_slice_make(input, n), NULL, &root, NULL);
        NX_CHECK(st == NX_OK || st == NX_ERR_PARSE || st == NX_ERR_LIMIT);
        if (st == NX_OK) {
            nx_buf out; nx_buf_init(&out); nx_json *copy = NULL;
            NX_CHECK_OK(nx_json_serialize(root, NULL, &out, NULL));
            NX_CHECK_OK(nx_json_parse(&arena, nx_buf_slice(&out), NULL, &copy, NULL));
            nx_buf_free(&out);
        } else NX_CHECK(root == NULL);
        nx_arena_free(&arena);
    }
}

static nx_status json_oom(void *unused) {
    (void)unused; nx_arena arena; nx_arena_init(&arena, 32); nx_json *root = NULL;
    nx_buf out; nx_buf_init(&out);
    nx_status st = nx_json_parse(&arena, nx_slice_cstr("{\"x\":[1,2,3],\"y\":\"hello\\nworld\",\"z\":{\"q\":true}}"),
                                  NULL, &root, NULL);
    if (st == NX_OK) st = nx_json_serialize(root, NULL, &out, NULL);
    nx_buf_free(&out); nx_arena_free(&arena); return st;
}
static void test_json_oom(void) { nx_test_oom_sweep(json_oom, NULL, 200); }
int main(void) {
    NX_RUN(test_json_typed_binary_and_numbers); NX_RUN(test_json_rejects_malformed);
    NX_RUN(test_json_bounds_and_rollback); NX_RUN(test_json_integer_oracle_roundtrip);
    NX_RUN(test_json_mutation_fuzz); NX_RUN(test_json_oom); return nx_test_summary();
}
