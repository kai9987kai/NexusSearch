/* test_merge.c - Unit tests for Tiered Segment Merge & Compaction */
#include "nx_test.h"
#include "store/nx_store.h"
#include "store/nx_merge.h"
#include "seg/nx_table.h"
#include "core/nx_mem.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef NX_WINDOWS
#  define nx_clean_dir(p) do { \
       char cmd[512]; snprintf(cmd, sizeof cmd, "rmdir /s /q \"%s\" 2>nul", (p)); \
       system(cmd); } while(0)
#else
#  define nx_clean_dir(p) do { \
       char cmd[512]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", (p)); \
       system(cmd); } while(0)
#endif

#ifdef NX_USE_SHARED
static const char *test_dir = "test_store_merge_shared_tmp";
#else
static const char *test_dir = "test_store_merge_tmp";
#endif

static void test_merge_compaction(void) {
    nx_mem_stats pre = nx_mem_get_stats();

    nx_clean_dir(test_dir);

    nx_store store;
    memset(&store, 0, sizeof(store));
    nx_error err;
    memset(&err, 0, sizeof(err));

    NX_CHECK_OK(nx_store_open(test_dir, &store, &err));

    /* Batch 1: docs 1, 2 */
    NX_CHECK_OK(nx_store_upsert(&store, "{\"_id\":\"doc1\",\"val\":\"v1_initial\"}", &err));
    NX_CHECK_OK(nx_store_upsert(&store, "{\"_id\":\"doc2\",\"val\":\"v2_initial\"}", &err));
    NX_CHECK_OK(nx_store_flush(&store, &err));
    NX_CHECK(nx_store_seg_count(&store) == 1);

    /* Batch 2: doc 3, and update doc 1 with new val */
    NX_CHECK_OK(nx_store_upsert(&store, "{\"_id\":\"doc3\",\"val\":\"v3_initial\"}", &err));
    NX_CHECK_OK(nx_store_upsert(&store, "{\"_id\":\"doc1\",\"val\":\"v1_updated\"}", &err));
    NX_CHECK_OK(nx_store_flush(&store, &err));
    NX_CHECK(nx_store_seg_count(&store) == 2);

    /* Batch 3: doc 4, and update doc 2 with new val */
    NX_CHECK_OK(nx_store_upsert(&store, "{\"_id\":\"doc4\",\"val\":\"v4_initial\"}", &err));
    NX_CHECK_OK(nx_store_upsert(&store, "{\"_id\":\"doc2\",\"val\":\"v2_updated\"}", &err));
    NX_CHECK_OK(nx_store_flush(&store, &err));
    NX_CHECK(nx_store_seg_count(&store) == 3);

    /* Total input documents: 2 + 2 + 2 = 6 docs across 3 segments.
     * Unique docs: doc1, doc2, doc3, doc4 = 4 surviving docs. */
    nx_merge_policy pol = nx_merge_default_policy();
    pol.min_segments_to_merge = 2;
    pol.max_segments_per_merge = 10;

    nx_merge_result res;
    NX_CHECK_OK(nx_store_compact(&store, &pol, &res, &err));

    NX_CHECK(res.segments_merged == 3);
    NX_CHECK(res.docs_input == 6);
    NX_CHECK(res.docs_output == 4);
    NX_CHECK(nx_store_seg_count(&store) == 1);

    /* Verify the single consolidated segment has doc_count == 4 */
    const nx_segment *cseg = nx_store_seg(&store, 0);
    NX_REQUIRE(cseg != NULL);
    NX_CHECK(cseg->doc_count == 4);

    /* Verify that the surviving documents contain the updated values */
    nx_table table;
    NX_CHECK_OK(nx_table_open(cseg->table_bytes, &table, &err));
    NX_CHECK(table.rows == 4);

    /* Look for doc1 and verify it has v1_updated */
    bool found_doc1_updated = false;
    for (uint32_t r = 0; r < table.rows; r++) {
        nx_slice id = nx_table_id(&table, r);
        if (id.n == 4 && memcmp(id.p, "doc1", 4) == 0) {
            nx_cell cell;
            uint32_t val_idx = 0;
            if (nx_table_find(&table, nx_slice_cstr("val"), &val_idx) == NX_OK &&
                nx_table_get(&table, r, val_idx, &cell) == NX_OK && cell.present) {
                if (cell.as.text.n == 10 && memcmp(cell.as.text.p, "v1_updated", 10) == 0) {
                    found_doc1_updated = true;
                }
            }
        }
    }
    NX_CHECK(found_doc1_updated);

    nx_store_close(&store);

    /* Reopen store and verify persistence of the compacted manifest */
    nx_store store2;
    memset(&store2, 0, sizeof(store2));
    NX_CHECK_OK(nx_store_open(test_dir, &store2, &err));
    NX_CHECK(nx_store_seg_count(&store2) == 1);
    NX_CHECK(nx_store_doc_count(&store2) == 4);

    nx_store_close(&store2);
    nx_clean_dir(test_dir);

    nx_mem_stats post = nx_mem_get_stats();
    NX_CHECK(post.live_allocs == pre.live_allocs);
}

int main(void) {
    NX_RUN(test_merge_compaction);
    return nx_test_summary();
}
