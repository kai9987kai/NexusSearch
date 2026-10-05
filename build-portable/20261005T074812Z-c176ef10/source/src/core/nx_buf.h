/* nx_buf.h - growable byte buffer (writer), borrowed byte slices and a
 * bounds-checked cursor (reader).
 *
 * Writers use a STICKY-OOM flag so builder code can append freely and check
 * `buf.oom` once at the end. Readers use a STICKY-ERROR flag so parsers of
 * untrusted bytes (segment files, model headers, network input) can read freely
 * and check `cur.err` once; out-of-range reads return 0 and never touch memory
 * outside the slice.
 */
#ifndef NX_BUF_H
#define NX_BUF_H

#include "nx_config.h"

/* ------------------------------------------------------------------ slices */
typedef struct nx_slice { const uint8_t *p; size_t n; } nx_slice;

NX_ALWAYS_INLINE nx_slice nx_slice_make(const void *p, size_t n) { nx_slice s = { (const uint8_t *)p, n }; return s; }
NX_ALWAYS_INLINE nx_slice nx_slice_cstr(const char *s) { return nx_slice_make(s, s ? strlen(s) : 0); }
/* sub-slice [off, off+len); returns empty slice if out of range */
NX_ALWAYS_INLINE nx_slice nx_slice_sub(nx_slice s, size_t off, size_t len) {
    if (off > s.n || len > s.n - off) return nx_slice_make(NULL, 0);
    return nx_slice_make(s.p + off, len);
}
NX_ALWAYS_INLINE bool nx_slice_eq(nx_slice a, nx_slice b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }

/* ------------------------------------------------------------------ buffer */
typedef struct nx_buf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    bool     oom;     /* sticky: once set every put is a no-op */
} nx_buf;

NX_API void   nx_buf_init(nx_buf *b);
NX_API void   nx_buf_free(nx_buf *b);
NX_API void   nx_buf_clear(nx_buf *b);                      /* len = 0, keeps capacity; clears oom */
NX_API bool   nx_buf_reserve(nx_buf *b, size_t extra);      /* ensure room for `extra` more bytes */
NX_API uint8_t *nx_buf_grow(nx_buf *b, size_t n);           /* extend by n (uninitialised), returns ptr to new bytes or NULL */
NX_API void   nx_buf_put(nx_buf *b, const void *p, size_t n);
NX_API void   nx_buf_put_zeros(nx_buf *b, size_t n);
NX_API void   nx_buf_put_u8(nx_buf *b, uint8_t v);
NX_API void   nx_buf_put_u16(nx_buf *b, uint16_t v);
NX_API void   nx_buf_put_u32(nx_buf *b, uint32_t v);
NX_API void   nx_buf_put_u64(nx_buf *b, uint64_t v);
NX_API void   nx_buf_put_f32(nx_buf *b, float v);
NX_API void   nx_buf_put_f64(nx_buf *b, double v);
NX_API void   nx_buf_put_varint(nx_buf *b, uint64_t v);     /* LEB128, 1..10 bytes */
NX_API void   nx_buf_put_str(nx_buf *b, const char *s);     /* raw bytes, no terminator */
NX_API void   nx_buf_put_cstr(nx_buf *b, const char *s);    /* including the terminator */
NX_API void   nx_buf_printf(nx_buf *b, const char *fmt, ...) NX_PRINTF(2, 3);
NX_API void   nx_buf_pad_to(nx_buf *b, size_t align);       /* zero-pad len up to a multiple of align (power of 2) */
NX_API void   nx_buf_patch_u32(nx_buf *b, size_t off, uint32_t v);
NX_API void   nx_buf_patch_u64(nx_buf *b, size_t off, uint64_t v);
/* Make the buffer NUL-terminated without counting the terminator in len. */
NX_API const char *nx_buf_cstr(nx_buf *b);
/* Take ownership of the bytes (caller must nx_free). Leaves buf empty. NULL on oom. */
NX_API uint8_t *nx_buf_detach(nx_buf *b, size_t *len_out);

NX_ALWAYS_INLINE nx_slice nx_buf_slice(const nx_buf *b) { return nx_slice_make(b->data, b->len); }

/* LEB128 helpers usable without a buffer. nx_varint_size() gives encoded length. */
NX_API size_t nx_varint_encode(uint8_t *out /* >= 10 bytes */, uint64_t v);
NX_ALWAYS_INLINE size_t nx_varint_size(uint64_t v) { return v ? (size_t)((nx_bit_width64(v) + 6) / 7) : 1; }

/* ------------------------------------------------------------------ cursor */
typedef struct nx_cursor {
    const uint8_t *p;
    size_t n;        /* total length of the slice */
    size_t pos;      /* current offset */
    bool   err;      /* sticky: a read ran past the end / malformed varint */
} nx_cursor;

NX_ALWAYS_INLINE nx_cursor nx_cursor_make(nx_slice s) { nx_cursor c = { s.p, s.n, 0, false }; return c; }
NX_ALWAYS_INLINE size_t nx_cursor_left(const nx_cursor *c) { return c->n - c->pos; }
NX_API bool     nx_rd_skip(nx_cursor *c, size_t n);
NX_API bool     nx_rd_seek(nx_cursor *c, size_t pos);
NX_API uint8_t  nx_rd_u8(nx_cursor *c);
NX_API uint16_t nx_rd_u16(nx_cursor *c);
NX_API uint32_t nx_rd_u32(nx_cursor *c);
NX_API uint64_t nx_rd_u64(nx_cursor *c);
NX_API float    nx_rd_f32(nx_cursor *c);
NX_API double   nx_rd_f64(nx_cursor *c);
NX_API uint64_t nx_rd_varint(nx_cursor *c);                 /* rejects >10 bytes / overlong overflow */
NX_API nx_slice nx_rd_bytes(nx_cursor *c, size_t n);        /* zero-copy view; empty + err on overrun */

#endif /* NX_BUF_H */
