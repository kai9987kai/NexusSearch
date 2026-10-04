/* Search immutable table snapshots. Results own their hit array (free with
 * nx_search_result_free); IDs/documents still borrow the table bytes. Searches
 * are thread-safe with separate results. No result is published on failure.
 * BM25 uses whole-table per-field stats, k1=1.2,b=.75, no (k1+1) multiplier.
 * Vector clauses use exhaustive cosine scoring; embeddings are caller-provided.
 * Indexed=false selects the exact scalar filter oracle. Supported syntax and
 * ranking semantics are documented in docs/SEARCH.md. */
#ifndef NX_SEARCH_H
#define NX_SEARCH_H
#include "seg/nx_table.h"
typedef struct nx_search_options {
    bool indexed;
    size_t max_work, max_hits;
    double bm25_k1, bm25_b;
} nx_search_options;
typedef struct nx_search_hit { uint32_t row; double score; nx_slice id, document; } nx_search_hit;
typedef struct nx_search_result {
    nx_search_hit *hits;
    size_t count, total, work, numeric_indexes, scanned_cells, vectors_scored;
    bool explain_only;
    bool indexed, has_lexical, has_vector;
} nx_search_result;
NX_API nx_search_options nx_search_default_options(void);
NX_API nx_status nx_search(const nx_table *table, nx_slice query, const nx_search_options *options,
                          nx_search_result *out, nx_error *error);
NX_API void nx_search_result_free(nx_search_result *result);
/* Appends JSON with hits, total, execution details; transactional on failure. */
NX_API nx_status nx_search_json(const nx_search_result *result, nx_buf *out);
#endif
