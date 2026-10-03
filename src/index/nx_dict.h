/* Immutable sorted unique byte dictionary. Binary strings (including empty
 * strings and embedded NUL) compare lexicographically by unsigned bytes; this
 * module performs no Unicode normalization. Terms are assigned ordinals in
 * that order, independent of input order and duplicates. No mutable state.
 *
 * Format v1: 32-byte LE header (magic, version, count, data_length,
 * header_size=32, reserved=0, data_offset, CRC32C), count+1 LE uint32 offsets
 * relative to the data start, then concatenated term bytes. CRC covers header
 * [0:28] and bytes[32:end]. Offsets are canonical, without gaps or trailers.
 *
 * build borrows input slices for the call and appends transactionally: on
 * failure prior output bytes and length stay unchanged, though sticky OOM may
 * be set. open allocates nothing; it validates the entire section and clears
 * *out on failure. An opened view and slices from term borrow unchanged bytes
 * for their lifetime. Concurrent reads are safe. */
#ifndef NX_DICT_H
#define NX_DICT_H
#include "core/nx_buf.h"
#include "core/nx_status.h"

#define NX_DICT_MAX_TERMS 1000000u
#define NX_DICT_MAX_TERM_BYTES 65536u
#define NX_DICT_MAX_DATA_BYTES (64u * 1024u * 1024u)
typedef struct nx_dict { nx_slice bytes; uint32_t count, data_offset; } nx_dict;

NX_API nx_status nx_dict_build(const nx_slice *terms, size_t count, nx_buf *out);
NX_API nx_status nx_dict_open(nx_slice bytes, nx_dict *out);
/* term returns INVALID for an out-of-range ordinal and clears *out. */
NX_API nx_status nx_dict_term(const nx_dict *dict, uint32_t ordinal, nx_slice *out);
/* Exact lookup. NOT_FOUND clears *ordinal to UINT32_MAX. */
NX_API nx_status nx_dict_lookup(const nx_dict *dict, nx_slice term, uint32_t *ordinal);
/* Return [begin,end) matching prefix; both outputs are cleared on error.
 * NX_ERR_LIMIT means the full range exceeds max_expansion (never truncates).
 * Empty prefix matches all terms. Empty match succeeds even with limit=0.
 * Work is O(log count) comparisons, independent of expansion size. */
NX_API nx_status nx_dict_prefix_range(const nx_dict *dict, nx_slice prefix, uint32_t max_expansion,
                                     uint32_t *begin, uint32_t *end);
#endif
