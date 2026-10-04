/* Immutable Inverted Postings Index with Lexical BM25 Scoring.
 * Based on Lucene 10.3.1 BM25Similarity, PISA, and Section 04 research.
 *
 * Implements term dictionary, per-term Roaring document bitmaps, document
 * frequencies (df), term frequencies (tf), document lengths (dl), and field
 * collection statistics (N, avgdl).
 *
 * Format v1:
 * - 32-byte LE header: magic "NXPOST1\0", version=1, term_count, doc_count,
 *   field_doc_count (N), total_doc_length, avgdl (float32), CRC32C.
 * - Dictionary bytes: serialized nx_dict
 * - Document lengths: uint32_t[doc_count] (compressed/exact token count per doc)
 * - Term metadata: uint32_t[term_count] (document frequency df per term)
 * - Postings offsets: uint32_t[term_count + 1] relative to postings payload start
 * - Postings payload: Roaring bitmap (doc IDs) + tf stream per term
 */
#ifndef NX_POSTINGS_H
#define NX_POSTINGS_H

#include "core/nx_config.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"
#include "index/nx_dict.h"
#include "index/nx_bitmap.h"

#define NX_POSTINGS_MAGIC "NXPOST1\0"
#define NX_POSTINGS_MAX_TERMS (1000000u)
#define NX_POSTINGS_MAX_DOCS  (1000000u)

typedef struct nx_postings_index {
    nx_slice bytes;
    uint32_t term_count;
    uint32_t doc_count;
    uint32_t field_doc_count; /* N: documents containing at least one term */
    uint64_t total_doc_len;
    float avgdl;
    nx_dict dict;
    const uint32_t *doc_lengths;   /* doc_count items */
    const uint32_t *term_dfs;      /* term_count items */
    const uint32_t *offsets;       /* term_count + 1 items */
    size_t postings_base;
} nx_postings_index;

typedef struct nx_postings_doc {
    const nx_slice *tokens;
    size_t token_count;
} nx_postings_doc;

/* Build an inverted postings index from tokenized documents:
 * docs is an array of doc_count documents, each with an array of token slices.
 * Appends serialized postings index transactionally to out buffer. */
NX_API nx_status nx_postings_build(const nx_postings_doc *docs, size_t doc_count, nx_buf *out);

/* Open an immutable postings index from serialized bytes.
 * Validates CRC32C, header, dictionary, and bounds. Zero allocation. */
NX_API nx_status nx_postings_open(nx_slice bytes, nx_postings_index *out);

/* Exact term lookup:
 * Returns NX_OK if term is in dictionary, populating out_df and out_doc_bitmap.
 * Returns NX_ERR_NOT_FOUND if term is absent. */
NX_API nx_status nx_postings_lookup(const nx_postings_index *index, nx_slice term,
                                    uint32_t *out_df, nx_bitmap *out_doc_bitmap);

/* Compute Lucene-style BM25 score for a given term and document ID:
 * idf = ln(1 + (N - df + 0.5) / (df + 0.5))
 * score = idf * tf / (tf + k1 * (1 - b + b * dl / avgdl))
 * Returns 0 if document does not contain the term. */
NX_API double nx_postings_bm25_score(const nx_postings_index *index, nx_slice term,
                                     uint32_t doc_id, double k1, double b);

#endif
