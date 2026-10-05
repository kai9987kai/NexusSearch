/* Tiered Segment Merge & Compaction Policy for Multi-Segment Stores.
 *
 * Consolidates multiple sealed segments into a single optimized segment,
 * purging deleted / tombstoned documents and deduplicating updated records
 * (latest document version wins). Updates manifest atomically and unlinks
 * obsolete segment files.
 */
#ifndef NX_MERGE_H
#define NX_MERGE_H

#include "core/nx_config.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"
#include "store/nx_store.h"

typedef struct nx_merge_policy {
    uint32_t min_segments_to_merge;    /* minimum segments required to trigger merge (default: 2) */
    uint32_t max_segments_per_merge;   /* maximum segments merged in one pass (default: 10) */
    uint32_t max_total_docs_per_merge; /* max doc count merged in one pass (default: 1,000,000) */
} nx_merge_policy;

static inline nx_merge_policy nx_merge_default_policy(void) {
    nx_merge_policy p = { 2u, 10u, 1000000u };
    return p;
}

typedef struct nx_merge_result {
    uint32_t segments_merged;
    uint32_t docs_input;
    uint32_t docs_output;              /* surviving docs after deduplicating _id */
    uint64_t new_seg_id;
} nx_merge_result;

/* Run compaction merge on store:
 * Selects candidate segments according to policy, extracts unique live documents,
 * builds a fresh consolidated segment, updates the store manifest atomically,
 * deletes merged files, and updates in-memory segment handles.
 * Returns NX_OK with segments_merged=0 if no compaction was needed. */
NX_API nx_status nx_store_compact(nx_store *store, const nx_merge_policy *policy,
                                  nx_merge_result *out_res, nx_error *error);

#endif
