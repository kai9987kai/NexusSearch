/* Immutable Roaring array/bitset views of standard little-endian portable
 * no-run bytes (cookie 12346). Run-container input is currently unsupported.
 * open validates the entire section, then queries allocate nothing. Borrowed
 * bytes must stay unchanged/alive while the view and its iterators are used.
 * Builders append transactionally to caller-owned buffers; failures leave
 * previous bytes and length unchanged. No mutable global state. */
#ifndef NX_BITMAP_H
#define NX_BITMAP_H
#include "core/nx_config.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"

typedef struct nx_bitmap {
    nx_slice bytes;
    uint32_t containers;
    uint64_t cardinality;
} nx_bitmap;
typedef enum nx_bitmap_op { NX_BITMAP_AND, NX_BITMAP_OR, NX_BITMAP_XOR, NX_BITMAP_ANDNOT } nx_bitmap_op;
typedef struct nx_bitmap_iter {
    const nx_bitmap *bitmap;
    uint32_t container, low;
} nx_bitmap_iter;

/* ids is borrowed; duplicates and arbitrary ordering are accepted. */
NX_API nx_status nx_bitmap_build(const uint32_t *ids, size_t n, nx_buf *out);
/* On error, clears *out. Input must be a complete section (no trailing bytes). */
NX_API nx_status nx_bitmap_open(nx_slice bytes, nx_bitmap *out);
NX_API bool nx_bitmap_contains(const nx_bitmap *b, uint32_t id);
NX_API void nx_bitmap_iter_init(const nx_bitmap *b, nx_bitmap_iter *it);
NX_API bool nx_bitmap_next(nx_bitmap_iter *it, uint32_t *id);
NX_API nx_status nx_bitmap_combine(const nx_bitmap *a, const nx_bitmap *b, nx_bitmap_op op, nx_buf *out);
#endif
