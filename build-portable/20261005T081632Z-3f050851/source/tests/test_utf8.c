#include "core/nx_config.h"
#include "nx_test.h"
#include "core/nx_utf8.h"

static void test_utf8_scalar_boundaries(void) {
    static const struct { uint32_t cp; const char *bytes; size_t n; } cases[] = {
        {0, "\0", 1}, {0x7f, "\x7f", 1}, {0x80, "\xc2\x80", 2},
        {0x7ff, "\xdf\xbf", 2}, {0x800, "\xe0\xa0\x80", 3},
        {0xd7ff, "\xed\x9f\xbf", 3}, {0xe000, "\xee\x80\x80", 3},
        {0xffff, "\xef\xbf\xbf", 3}, {0x10000, "\xf0\x90\x80\x80", 4},
        {0x10ffff, "\xf4\x8f\xbf\xbf", 4}
    };
    for (size_t i = 0; i < NX_ARRAY_LEN(cases); ++i) {
        uint8_t encoded[4] = {0}; size_t n = 0, pos = 0; uint32_t cp = UINT32_MAX;
        NX_CHECK_OK(nx_utf8_encode(cases[i].cp, encoded, &n));
        NX_CHECK_EQ_U(n, cases[i].n);
        NX_CHECK(memcmp(encoded, cases[i].bytes, n) == 0);
        NX_CHECK_OK(nx_utf8_decode(nx_slice_make(cases[i].bytes, cases[i].n), &pos, &cp));
        NX_CHECK_EQ_U(cp, cases[i].cp); NX_CHECK_EQ_U(pos, n);
    }
    size_t n = 8; uint8_t out[4];
    NX_CHECK_EQ_I(nx_utf8_encode(0xd800, out, &n), NX_ERR_CORRUPT); NX_CHECK_EQ_U(n, 0);
    NX_CHECK_EQ_I(nx_utf8_encode(0x110000, out, &n), NX_ERR_CORRUPT);
}

static void test_utf8_invalid_and_truncated(void) {
    const char *bad[] = {"\x80", "\xc0\x80", "\xc1\xbf", "\xe0\x80\xaf", "\xed\xa0\x80",
        "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff", "\xe2(\xa1"};
    for (size_t i = 0; i < NX_ARRAY_LEN(bad); ++i) {
        size_t pos = 0, at = 88; uint32_t cp = 9;
        NX_CHECK_EQ_I(nx_utf8_validate(nx_slice_cstr(bad[i]), &at), NX_ERR_CORRUPT);
        NX_CHECK_EQ_U(at, 0);
        NX_CHECK_EQ_I(nx_utf8_decode(nx_slice_cstr(bad[i]), &pos, &cp), NX_ERR_CORRUPT);
        NX_CHECK_EQ_U(pos, 0);
    }
    for (size_t i = 1; i < 4; ++i)
        NX_CHECK_EQ_I(nx_utf8_validate(nx_slice_make("\xf0\x9f\x98\x80", i), NULL), NX_ERR_CORRUPT);
    NX_CHECK_OK(nx_utf8_validate(nx_slice_make(NULL, 0), NULL));
    NX_CHECK_EQ_I(nx_utf8_validate(nx_slice_make(NULL, 1), NULL), NX_ERR_INVALID);
}

static void test_utf8_random_scalar_roundtrip(void) {
    nx_rng rng = nx_test_rng(28);
    for (size_t i = 0; i < nx_test_iters(15000); ++i) {
        uint32_t cp = (uint32_t)nx_rng_below(&rng, 0x110000);
        if (cp >= 0xd800 && cp <= 0xdfff) continue;
        uint8_t buf[4]; size_t n = 0, pos = 0; uint32_t got = 0;
        NX_CHECK_OK(nx_utf8_encode(cp, buf, &n));
        NX_CHECK_OK(nx_utf8_validate(nx_slice_make(buf, n), NULL));
        NX_CHECK_OK(nx_utf8_decode(nx_slice_make(buf, n), &pos, &got));
        NX_CHECK_EQ_U(cp, got); NX_CHECK_EQ_U(pos, n);
    }
}

static void test_utf8_normalization_and_spans(void) {
    nx_arena arena; nx_arena_init(&arena, 64);
    const char source[] = "HELLO, \xef\xbc\xa1\xef\xbc\xa2 CAFE\xcc\x81 \xe4\xb8\xad\xe6\x96\x87";
    nx_utf8_tokens tokens;
    NX_CHECK_OK(nx_utf8_tokenize(&arena, nx_slice_cstr(source), NULL, &tokens));
    NX_REQUIRE(tokens.count == 4);
    NX_CHECK(nx_slice_eq(tokens.items[0].text, nx_slice_cstr("hello")));
    NX_CHECK(nx_slice_eq(tokens.items[1].text, nx_slice_cstr("ab")));
    NX_CHECK_EQ_U(tokens.items[1].offset, 7); NX_CHECK_EQ_U(tokens.items[1].length, 6);
    NX_CHECK(nx_slice_eq(tokens.items[2].text, nx_slice_cstr("cafe\xcc\x81")));
    NX_CHECK(nx_slice_eq(tokens.items[3].text, nx_slice_cstr("\xe4\xb8\xad\xe6\x96\x87")));
    nx_slice norm; const char binary[] = {'A', 0, 'Z'};
    NX_CHECK_OK(nx_utf8_normalize(&arena, nx_slice_make(binary, 3), NX_UTF8_FOLD_ASCII, 3, &norm));
    NX_CHECK(norm.n == 3 && memcmp(norm.p, "a\0z", 3) == 0 && norm.p[3] == 0);
    size_t before = arena.total;
    NX_CHECK_EQ_I(nx_utf8_normalize(&arena, nx_slice_cstr("abcd"), 0, 3, &norm), NX_ERR_LIMIT);
    NX_CHECK_EQ_U(arena.total, before);
    nx_utf8_limits limits = nx_utf8_default_limits(); limits.max_tokens = 1;
    NX_CHECK_EQ_I(nx_utf8_tokenize(&arena, nx_slice_cstr("one two"), &limits, &tokens), NX_ERR_LIMIT);
    NX_CHECK(tokens.count == 0 && tokens.items == NULL); NX_CHECK_EQ_U(arena.total, before);
    nx_arena_free(&arena);
}

static nx_status utf8_oom(void *unused) {
    (void)unused; nx_arena arena; nx_arena_init(&arena, 32); nx_utf8_tokens tokens;
    nx_status st = nx_utf8_tokenize(&arena, nx_slice_cstr("ONE TWO THREE FOUR FIVE SIX SEVEN"), NULL, &tokens);
    if (st == NX_OK && tokens.count != 7) st = NX_ERR_INTERNAL;
    nx_arena_free(&arena); return st;
}
static void test_utf8_oom(void) { nx_test_oom_sweep(utf8_oom, NULL, 100); }

int main(void) {
    NX_RUN(test_utf8_scalar_boundaries); NX_RUN(test_utf8_invalid_and_truncated);
    NX_RUN(test_utf8_random_scalar_roundtrip); NX_RUN(test_utf8_normalization_and_spans); NX_RUN(test_utf8_oom);
    return nx_test_summary();
}
