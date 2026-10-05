/* Immutable Trigram Inverted Index for Substring, Regex, and Code Search.
 * Based on Google Code Search (Russ Cox, 2012) and Zoekt architecture.
 *
 * Maps 24-bit 3-byte sequences ((c0 << 16) | (c1 << 8) | c2) to Roaring bitmaps
 * of document IDs. Substring queries intersect trigram posting bitmaps using
 * boolean AND, pruning non-matching documents in microseconds before evaluating
 * exact string matching or regex DFA engines.
 *
 * Format v1:
 * - 32-byte LE header: magic "NXTRG1\0\0", version=1, trigram_count, doc_count,
 *   header_size=32, reserved=0, CRC32C covering [0:28] and payload.
 * - Sorted unique trigrams: uint32_t[trigram_count] (high byte zero, sorted ascending)
 * - Posting offsets: uint32_t[trigram_count + 1] relative to postings data
 * - Roaring bitmaps: standard portable Roaring bitmaps per trigram.
 */
#ifndef NX_TRIGRAM_H
#define NX_TRIGRAM_H

#include "core/nx_config.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"
#include "core/nx_arena.h"
#include "index/nx_bitmap.h"

#define NX_TRIGRAM_MAGIC "NXTRG1\0\0"
#define NX_TRIGRAM_MAX_COUNT (16777216u) /* 2^24 maximum possible 3-byte combinations */
#define NX_TRIGRAM_MAX_DOCS (1000000u)

typedef struct nx_trigram_index {
    nx_slice bytes;
    uint32_t trigram_count;
    uint32_t doc_count;
    const uint32_t *trigrams;     /* points into bytes */
    const uint32_t *offsets;      /* points into bytes */
    size_t postings_base;         /* offset in bytes */
} nx_trigram_index;

/* Helper to convert 3 bytes to a canonical (case-folded) 24-bit trigram integer */
static inline uint32_t nx_trigram_pack(uint8_t c0, uint8_t c1, uint8_t c2) {
    if (c0 >= 'A' && c0 <= 'Z') c0 = (uint8_t)(c0 + 32);
    if (c1 >= 'A' && c1 <= 'Z') c1 = (uint8_t)(c1 + 32);
    if (c2 >= 'A' && c2 <= 'Z') c2 = (uint8_t)(c2 + 32);
    return ((uint32_t)c0 << 16) | ((uint32_t)c1 << 8) | (uint32_t)c2;
}

/* Extract unique canonical trigrams from a text slice.
 * out_trigrams must have capacity for at least max_trigrams.
 * Returns number of unique trigrams extracted in *out_count. */
NX_API nx_status nx_trigram_extract(nx_slice text, uint32_t *out_trigrams, size_t max_trigrams, size_t *out_count);

/* Document-level builder:
 * docs is an array of slices, one per doc_id = 0 .. doc_count - 1.
 * Appends serialized trigram index transactionally to out buffer. */
NX_API nx_status nx_trigram_build(const nx_slice *docs, size_t doc_count, nx_buf *out);

/* Open an immutable trigram index from serialized bytes.
 * Validates CRC32C, header, ordering, and bitmap sections. Zero allocation. */
NX_API nx_status nx_trigram_open(nx_slice bytes, nx_trigram_index *out);

/* Lookup a single trigram's document bitmap.
 * Returns NX_OK and populates *out_bitmap if found.
 * Returns NX_ERR_NOT_FOUND if trigram is absent from corpus. */
NX_API nx_status nx_trigram_lookup(const nx_trigram_index *index, uint32_t trigram, nx_bitmap *out_bitmap);

/* Filter candidate documents for a substring query:
 * Extracts all trigrams from needle, intersects their posting bitmaps into *out_bitmap_buf.
 * If needle has < 3 bytes, *has_candidates is set to false (fallback to exhaustive scan).
 * If all trigrams are present, *has_candidates is set to true and *out_bitmap_buf contains
 * the candidate doc IDs. If any trigram is missing from the corpus, *has_candidates is true
 * and candidate count is 0 (guaranteed zero hits without scanning). */
NX_API nx_status nx_trigram_query_substring(const nx_trigram_index *index, nx_slice needle,
                                           nx_buf *out_bitmap_buf, bool *has_candidates);

#endif
