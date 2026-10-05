/* Reusable exact search preparation for an immutable validated table.
 * The object owns its normalized text, term postings and trigram postings but
 * borrows table bytes, which must remain unchanged/alive until it is freed.
 * A completed index is immutable and can be searched concurrently with separate
 * results. No object is published on failure. Build limits: 64 MiB source text,
 * 4 million tokens, 16 million raw trigrams per field, 256 MiB live index/build
 * allocations (plus bounded per-document analyzer scratch). No disk writes.
 * options.indexed=false uses the independent existing scan implementation.
 * Plain terms use exact uint32 TF and double-precision collection statistics;
 * phrase/substring candidates are always verified. Other clauses retain exact
 * scan behavior. Preparation time is separate from warm-query execution work.
 */
#ifndef NX_SEARCH_INDEX_H
#define NX_SEARCH_INDEX_H
#include "engine/nx_search.h"
typedef struct nx_search_index nx_search_index;
NX_API nx_status nx_search_index_build(const nx_table *table, nx_search_index **out,
                                        nx_error *error);
NX_API void nx_search_index_free(nx_search_index *index);
NX_API size_t nx_search_index_memory_bytes(const nx_search_index *index);
NX_API nx_status nx_search_index_search(const nx_search_index *index, nx_slice query,
                                        const nx_search_options *options,
                                        nx_search_result *out, nx_error *error);
#endif
