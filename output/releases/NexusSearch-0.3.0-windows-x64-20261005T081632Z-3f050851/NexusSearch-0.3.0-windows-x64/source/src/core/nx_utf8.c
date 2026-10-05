#include "core/nx_config.h"
#include "core/nx_utf8.h"

nx_status nx_utf8_decode(nx_slice input, size_t *offset, uint32_t *cp) {
    if (!offset || !cp || (!input.p && input.n) || *offset > input.n) return NX_ERR_INVALID;
    if (*offset == input.n) return NX_ERR_NOT_FOUND;
    nx_cursor cur = nx_cursor_make(input);
    (void)nx_rd_seek(&cur, *offset);
    uint8_t first = nx_rd_u8(&cur);
    uint32_t value, minimum;
    unsigned extra;
    if (first < 0x80) { value = first; minimum = 0; extra = 0; }
    else if (first >= 0xc2 && first <= 0xdf) { value = first & 0x1fu; minimum = 0x80; extra = 1; }
    else if (first >= 0xe0 && first <= 0xef) { value = first & 0x0fu; minimum = 0x800; extra = 2; }
    else if (first >= 0xf0 && first <= 0xf4) { value = first & 0x07u; minimum = 0x10000; extra = 3; }
    else return NX_ERR_CORRUPT;
    if (nx_cursor_left(&cur) < extra) return NX_ERR_CORRUPT;
    for (unsigned i = 0; i < extra; ++i) {
        uint8_t next = nx_rd_u8(&cur);
        if ((next & 0xc0u) != 0x80u) return NX_ERR_CORRUPT;
        value = (value << 6) | (next & 0x3fu);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return NX_ERR_CORRUPT;
    *offset = cur.pos; *cp = value;
    return NX_OK;
}

nx_status nx_utf8_encode(uint32_t cp, uint8_t out[4], size_t *length) {
    if (length) *length = 0;
    if (!out || !length) return NX_ERR_INVALID;
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return NX_ERR_CORRUPT;
    if (cp < 0x80) { out[0] = (uint8_t)cp; *length = 1; }
    else if (cp < 0x800) {
        out[0] = (uint8_t)(0xc0u | (cp >> 6)); out[1] = (uint8_t)(0x80u | (cp & 0x3fu)); *length = 2;
    } else if (cp < 0x10000) {
        out[0] = (uint8_t)(0xe0u | (cp >> 12)); out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
        out[2] = (uint8_t)(0x80u | (cp & 0x3fu)); *length = 3;
    } else {
        out[0] = (uint8_t)(0xf0u | (cp >> 18)); out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3fu));
        out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu)); out[3] = (uint8_t)(0x80u | (cp & 0x3fu)); *length = 4;
    }
    return NX_OK;
}

nx_status nx_utf8_validate(nx_slice input, size_t *bad_offset) {
    if (bad_offset) *bad_offset = 0;
    if (!input.p && input.n) return NX_ERR_INVALID;
    size_t pos = 0;
    while (pos < input.n) {
        uint32_t cp; size_t start = pos;
        nx_status st = nx_utf8_decode(input, &pos, &cp);
        if (st != NX_OK) { if (bad_offset) *bad_offset = start; return st; }
    }
    if (bad_offset) *bad_offset = input.n;
    return NX_OK;
}

static bool flags_valid(unsigned flags) { return (flags & ~(unsigned)(NX_UTF8_FOLD_ASCII | NX_UTF8_FOLD_WIDTH)) == 0; }
static uint32_t fold_scalar(uint32_t cp, unsigned flags) {
    if ((flags & NX_UTF8_FOLD_WIDTH) && cp >= 0xff01 && cp <= 0xff5e) cp -= 0xfee0;
    if ((flags & NX_UTF8_FOLD_WIDTH) && cp == 0x3000) cp = 0x20;
    if ((flags & NX_UTF8_FOLD_ASCII) && cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
    return cp;
}

nx_status nx_utf8_normalize(nx_arena *arena, nx_slice input, unsigned flags, size_t max_bytes, nx_slice *out) {
    if (out) *out = nx_slice_make(NULL, 0);
    if (!arena || !out || !flags_valid(flags) || (!input.p && input.n)) return NX_ERR_INVALID;
    size_t allocation;
    if (input.n > max_bytes || nx_add_overflow(input.n, 1, &allocation)) return NX_ERR_LIMIT;
    nx_status st = nx_utf8_validate(input, NULL);
    if (st != NX_OK) return st;
    uint8_t *data = (uint8_t *)nx_arena_alloc(arena, allocation, 1);
    if (!data) return NX_ERR_NOMEM;
    size_t pos = 0, used = 0;
    while (pos < input.n) {
        uint32_t cp; uint8_t bytes[4]; size_t count;
        (void)nx_utf8_decode(input, &pos, &cp);
        (void)nx_utf8_encode(fold_scalar(cp, flags), bytes, &count);
        memcpy(data + used, bytes, count); used += count;
    }
    data[used] = 0; *out = nx_slice_make(data, used);
    return NX_OK;
}

nx_utf8_limits nx_utf8_default_limits(void) {
    nx_utf8_limits limits = {16 * 1024 * 1024, 100000, 1024 * 1024, NX_UTF8_FOLD_ASCII | NX_UTF8_FOLD_WIDTH};
    return limits;
}

static bool token_scalar(uint32_t cp, unsigned flags) {
    cp = fold_scalar(cp, flags);
    if (cp < 0x80) return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
                           (cp >= '0' && cp <= '9') || cp == '_';
    if (cp <= 0x9f || cp == 0xa0 || cp == 0x1680 || cp == 0x180e || cp == 0xfeff ||
        (cp >= 0x2000 && cp <= 0x206f) || (cp >= 0x2e00 && cp <= 0x2e7f) ||
        (cp >= 0x3000 && cp <= 0x303f)) return false;
    return true;
}

/* The first pass validates and sizes before allocating. The second pass keeps
 * original byte positions even when fullwidth characters shrink during folding. */
static nx_status tokens_pass(nx_arena *arena, nx_slice input, const nx_utf8_limits *limits,
                             nx_utf8_token *items, size_t *count) {
    size_t pos = 0, start = 0, total = 0; bool in_token = false;
    while (pos < input.n || in_token) {
        size_t at = pos; bool word = false;
        if (pos < input.n) {
            uint32_t cp; nx_status st = nx_utf8_decode(input, &pos, &cp);
            if (st != NX_OK) return st;
            word = token_scalar(cp, limits->flags);
        }
        if (word && !in_token) { start = at; in_token = true; }
        if (word && pos - start > limits->max_token_bytes) return NX_ERR_LIMIT;
        if (!word && in_token) {
            if (total == limits->max_tokens) return NX_ERR_LIMIT;
            if (items) {
                items[total].offset = start; items[total].length = at - start;
                nx_status st = nx_utf8_normalize(arena, nx_slice_sub(input, start, at - start),
                                                 limits->flags, limits->max_token_bytes, &items[total].text);
                if (st != NX_OK) return st;
            }
            ++total; in_token = false;
        }
    }
    *count = total;
    return NX_OK;
}

nx_status nx_utf8_tokenize(nx_arena *arena, nx_slice input, const nx_utf8_limits *limits, nx_utf8_tokens *out) {
    if (out) { out->items = NULL; out->count = 0; }
    if (!arena || !out || (!input.p && input.n)) return NX_ERR_INVALID;
    nx_utf8_limits defaults = nx_utf8_default_limits();
    if (!limits) limits = &defaults;
    if (!flags_valid(limits->flags)) return NX_ERR_INVALID;
    if (input.n > limits->max_input_bytes) return NX_ERR_LIMIT;
    size_t count = 0, bytes;
    nx_status st = tokens_pass(arena, input, limits, NULL, &count);
    if (st != NX_OK || count == 0) return st;
    if (nx_mul_overflow(count, sizeof(nx_utf8_token), &bytes)) return NX_ERR_LIMIT;
    nx_arena_mark mark = nx_arena_save(arena);
    nx_utf8_token *items = (nx_utf8_token *)nx_arena_alloc(arena, bytes, _Alignof(nx_utf8_token));
    if (!items) return NX_ERR_NOMEM;
    st = tokens_pass(arena, input, limits, items, &count);
    if (st != NX_OK) { nx_arena_restore(arena, mark); return st; }
    out->items = items; out->count = count;
    return NX_OK;
}
