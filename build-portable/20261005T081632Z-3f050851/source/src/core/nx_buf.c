#include "nx_buf.h"
#include "nx_mem.h"
#include <stdarg.h>
#include <stdio.h>

void nx_buf_init(nx_buf *b) { b->data = NULL; b->len = 0; b->cap = 0; b->oom = false; }
void nx_buf_free(nx_buf *b) { nx_free(b->data); nx_buf_init(b); }
void nx_buf_clear(nx_buf *b) { b->len = 0; b->oom = false; }

bool nx_buf_reserve(nx_buf *b, size_t extra) {
    if (b->oom) return false;
    size_t need;
    if (nx_add_overflow(b->len, extra, &need)) { b->oom = true; return false; }
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
        size_t next;
        if (nx_mul_overflow(cap, 2, &next)) { cap = need; break; }
        cap = next;
    }
    uint8_t *d = (uint8_t *)nx_realloc(b->data, cap);
    if (!d) { b->oom = true; return false; }
    b->data = d; b->cap = cap;
    return true;
}

uint8_t *nx_buf_grow(nx_buf *b, size_t n) {
    if (!nx_buf_reserve(b, n)) return NULL;
    uint8_t *p = b->data + b->len;
    b->len += n;
    return p;
}

void nx_buf_put(nx_buf *b, const void *p, size_t n) {
    if (!n) return;
    uint8_t *d = nx_buf_grow(b, n);
    if (d) memcpy(d, p, n);
}
void nx_buf_put_zeros(nx_buf *b, size_t n) {
    if (!n) return;
    uint8_t *d = nx_buf_grow(b, n);
    if (d) memset(d, 0, n);
}
void nx_buf_put_u8(nx_buf *b, uint8_t v) { nx_buf_put(b, &v, 1); }
void nx_buf_put_u16(nx_buf *b, uint16_t v) { uint8_t t[2]; nx_st16(t, v); nx_buf_put(b, t, 2); }
void nx_buf_put_u32(nx_buf *b, uint32_t v) { uint8_t t[4]; nx_st32(t, v); nx_buf_put(b, t, 4); }
void nx_buf_put_u64(nx_buf *b, uint64_t v) { uint8_t t[8]; nx_st64(t, v); nx_buf_put(b, t, 8); }
void nx_buf_put_f32(nx_buf *b, float v) { uint8_t t[4]; nx_stf32(t, v); nx_buf_put(b, t, 4); }
void nx_buf_put_f64(nx_buf *b, double v) { uint8_t t[8]; nx_stf64(t, v); nx_buf_put(b, t, 8); }

size_t nx_varint_encode(uint8_t *out, uint64_t v) {
    size_t i = 0;
    while (v >= 0x80) { out[i++] = (uint8_t)(v | 0x80); v >>= 7; }
    out[i++] = (uint8_t)v;
    return i;
}
void nx_buf_put_varint(nx_buf *b, uint64_t v) {
    uint8_t t[10];
    size_t n = nx_varint_encode(t, v);
    nx_buf_put(b, t, n);
}
void nx_buf_put_str(nx_buf *b, const char *s) { if (s) nx_buf_put(b, s, strlen(s)); }
void nx_buf_put_cstr(nx_buf *b, const char *s) { nx_buf_put(b, s ? s : "", (s ? strlen(s) : 0) + 1); }

void nx_buf_printf(nx_buf *b, const char *fmt, ...) {
    if (b->oom) return;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    if (!nx_buf_reserve(b, (size_t)n + 1)) { va_end(ap2); return; }
    vsnprintf((char *)b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void nx_buf_pad_to(nx_buf *b, size_t align) {
    if (!nx_is_pow2(align)) { b->oom = true; return; }
    size_t pad = nx_align_up(b->len, align) - b->len;
    nx_buf_put_zeros(b, pad);
}
void nx_buf_patch_u32(nx_buf *b, size_t off, uint32_t v) {
    if (b->oom) return;
    if (off > b->len || b->len - off < 4) nx_panic("nx_buf_patch_u32 out of range");
    nx_st32(b->data + off, v);
}
void nx_buf_patch_u64(nx_buf *b, size_t off, uint64_t v) {
    if (b->oom) return;
    if (off > b->len || b->len - off < 8) nx_panic("nx_buf_patch_u64 out of range");
    nx_st64(b->data + off, v);
}

const char *nx_buf_cstr(nx_buf *b) {
    if (!nx_buf_reserve(b, 1)) return "";
    b->data[b->len] = 0;
    return (const char *)b->data;
}

uint8_t *nx_buf_detach(nx_buf *b, size_t *len_out) {
    if (b->oom) { if (len_out) *len_out = 0; nx_buf_free(b); return NULL; }
    uint8_t *d = b->data;
    if (len_out) *len_out = b->len;
    nx_buf_init(b);
    return d;
}

/* ------------------------------------------------------------------ cursor */
static bool cur_need(nx_cursor *c, size_t n) {
    if (c->err) return false;
    if (n > c->n - c->pos) { c->err = true; return false; }
    return true;
}
bool nx_rd_skip(nx_cursor *c, size_t n) {
    if (!cur_need(c, n)) return false;
    c->pos += n;
    return true;
}
bool nx_rd_seek(nx_cursor *c, size_t pos) {
    if (c->err || pos > c->n) { c->err = true; return false; }
    c->pos = pos;
    return true;
}
uint8_t nx_rd_u8(nx_cursor *c) { if (!cur_need(c, 1)) return 0; return c->p[c->pos++]; }
uint16_t nx_rd_u16(nx_cursor *c) { if (!cur_need(c, 2)) return 0; uint16_t v = nx_ld16(c->p + c->pos); c->pos += 2; return v; }
uint32_t nx_rd_u32(nx_cursor *c) { if (!cur_need(c, 4)) return 0; uint32_t v = nx_ld32(c->p + c->pos); c->pos += 4; return v; }
uint64_t nx_rd_u64(nx_cursor *c) { if (!cur_need(c, 8)) return 0; uint64_t v = nx_ld64(c->p + c->pos); c->pos += 8; return v; }
float nx_rd_f32(nx_cursor *c) { if (!cur_need(c, 4)) return 0; float v = nx_ldf32(c->p + c->pos); c->pos += 4; return v; }
double nx_rd_f64(nx_cursor *c) { if (!cur_need(c, 8)) return 0; double v = nx_ldf64(c->p + c->pos); c->pos += 8; return v; }

uint64_t nx_rd_varint(nx_cursor *c) {
    uint64_t v = 0;
    for (int shift = 0; shift < 70; shift += 7) {
        if (!cur_need(c, 1)) return 0;
        uint8_t byte = c->p[c->pos++];
        if (shift == 63 && (byte & 0x7E)) { c->err = true; return 0; }  /* would overflow 64 bits */
        v |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return v;
    }
    c->err = true;                    /* more than 10 bytes */
    return 0;
}

nx_slice nx_rd_bytes(nx_cursor *c, size_t n) {
    if (!cur_need(c, n)) return nx_slice_make(NULL, 0);
    nx_slice s = nx_slice_make(c->p + c->pos, n);
    c->pos += n;
    return s;
}
