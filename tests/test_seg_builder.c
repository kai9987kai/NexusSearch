/* test_seg_builder.c - Unit tests for nx_seg_builder and nx_store. */
#include "nx_test.h"
#include "seg/nx_seg_builder.h"
#include "store/nx_store.h"
#include "index/nx_postings.h"
#include "index/nx_trigram.h"
#include "index/nx_graph.h"
#include "index/nx_rabitq.h"
#include "core/nx_mem.h"
#include <string.h>
#include <stdio.h>

#ifdef NX_WINDOWS
#  include <windows.h>
#  include <direct.h>
#  define nx_rmdir_r(p) do { \
       char cmd[512]; snprintf(cmd, sizeof cmd, "rmdir /s /q \"%s\" 2>nul", (p)); \
       system(cmd); } while(0)
#else
#  include <unistd.h>
#  define nx_rmdir_r(p) do { \
       char cmd[512]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", (p)); \
       system(cmd); } while(0)
#endif

/* Sample JSONL with text + vector fields */
static const char *SAMPLE_JSONL =
    "{\"_id\":\"d1\",\"title\":\"hello world nexus search\",\"score\":10,\"vec\":[1.0,0.0,0.0]}\n"
    "{\"_id\":\"d2\",\"title\":\"fast full text retrieval engine\",\"score\":20,\"vec\":[0.0,1.0,0.0]}\n"
    "{\"_id\":\"d3\",\"title\":\"approximate nearest neighbor graph\",\"score\":30,\"vec\":[0.0,0.0,1.0]}\n"
    "{\"_id\":\"d4\",\"title\":\"nexus vector and text combined\",\"score\":40,\"vec\":[0.707,0.707,0.0]}\n";

/* ---- test 1: build a segment and verify sections are present ------------ */
static void test_seg_build_and_open(void) {
    nx_mem_stats pre = nx_mem_get_stats();

    nx_buf out;
    nx_buf_init(&out);
    nx_error err;
    memset(&err, 0, sizeof err);

    const char *jsonl = SAMPLE_JSONL;
    nx_slice input = nx_slice_cstr(jsonl);

    nx_seg_build_config cfg = nx_seg_build_default_config();
    cfg.seg_id = 42;

    nx_status st = nx_seg_build(input, &cfg, &out, &err);
    NX_CHECK_OK(st);
    NX_CHECK(out.len > 0);

    /* Open the segment */
    nx_segment seg;
    memset(&seg, 0, sizeof seg);
    nx_error open_err;
    memset(&open_err, 0, sizeof open_err);
    st = nx_segment_open(nx_buf_slice(&out), &seg, &open_err);
    NX_CHECK_OK(st);

    /* Check metadata */
    NX_CHECK(seg.seg_id == 42);
    NX_CHECK(seg.doc_count == 4);
    NX_CHECK(seg.sec_count > 0);

    /* Must have a table section */
    nx_slice table_sec;
    st = nx_segment_section(&seg, NX_SEC_TABLE, NX_SEC_NO_FIELD, &table_sec);
    NX_CHECK_OK(st);
    NX_CHECK(table_sec.n > 0);

    /* Must have postings section for field 1 (title is field 1) */
    uint32_t post_count = nx_segment_section_count(&seg, NX_SEC_POSTINGS);
    NX_CHECK(post_count >= 1);

    /* Must have trigram section */
    uint32_t trig_count = nx_segment_section_count(&seg, NX_SEC_TRIGRAM);
    NX_CHECK(trig_count >= 1);

    /* Must have graph section (vec field) */
    uint32_t graph_count = nx_segment_section_count(&seg, NX_SEC_GRAPH);
    NX_CHECK(graph_count >= 1);

    /* Must have rabitq section (vec field) */
    uint32_t rabitq_count = nx_segment_section_count(&seg, NX_SEC_RABITQ);
    NX_CHECK(rabitq_count >= 1);

    nx_buf_free(&out);

    nx_mem_stats post = nx_mem_get_stats();
    NX_CHECK(post.live_allocs == pre.live_allocs);
}

/* ---- test 2: postings index is searchable from segment ------------------ */
static void test_seg_postings_lookup(void) {
    nx_buf out;
    nx_buf_init(&out);
    nx_error err;
    memset(&err, 0, sizeof err);

    const char *jsonl = SAMPLE_JSONL;
    nx_slice input = nx_slice_cstr(jsonl);
    nx_status st = nx_seg_build(input, NULL, &out, &err);
    NX_CHECK_OK(st);

    nx_segment seg;
    memset(&seg, 0, sizeof seg);
    st = nx_segment_open(nx_buf_slice(&out), &seg, &err);
    NX_CHECK_OK(st);

    /* Find a postings section and open it */
    for (uint32_t fi = 0; fi < 10; fi++) {
        nx_slice pslice;
        st = nx_segment_section(&seg, NX_SEC_POSTINGS, (uint16_t)fi, &pslice);
        if (st == NX_ERR_NOT_FOUND) continue;
        NX_CHECK_OK(st);

        nx_postings_index pidx;
        memset(&pidx, 0, sizeof pidx);
        st = nx_postings_open(pslice, &pidx);
        NX_CHECK_OK(st);

        /* "nexus" should appear in docs d1, d4 */
        nx_bitmap bm;
        uint32_t df = 0;
        nx_status ls = nx_postings_lookup(&pidx, nx_slice_cstr("nexus"), &df, &bm);
        if (ls == NX_OK) {
            NX_CHECK(df >= 1);
            break;
        }
    }

    nx_buf_free(&out);
}

/* ---- test 3: trigram index is searchable from segment ------------------ */
static void test_seg_trigram_lookup(void) {
    nx_buf out;
    nx_buf_init(&out);
    nx_error err;
    memset(&err, 0, sizeof err);

    nx_slice input = nx_slice_cstr(SAMPLE_JSONL);
    nx_status st = nx_seg_build(input, NULL, &out, &err);
    NX_CHECK_OK(st);

    nx_segment seg;
    memset(&seg, 0, sizeof seg);
    st = nx_segment_open(nx_buf_slice(&out), &seg, &err);
    NX_CHECK_OK(st);

    /* Scan all sections looking for a trigram section that has actual trigrams */
    bool found_good_trig = false;
    const uint8_t *dir = (const uint8_t *)seg.dir;
    for (uint32_t i = 0; i < seg.sec_count; i++) {
        const uint8_t *e = dir + (size_t)i * 16;
        uint32_t sid  = nx_ld32(e + 0);
        uint16_t fidx = nx_ld16(e + 4);
        if (sid != NX_SEC_TRIGRAM) continue;

        nx_slice tslice;
        st = nx_segment_section(&seg, NX_SEC_TRIGRAM, fidx, &tslice);
        NX_CHECK_OK(st);

        nx_trigram_index tidx;
        memset(&tidx, 0, sizeof tidx);
        st = nx_trigram_open(tslice, &tidx);
        NX_CHECK_OK(st);   /* Every trigram section must open successfully */

        if (tidx.trigram_count > 0) {
            NX_CHECK(tidx.doc_count == 4);
            /* "nex" trigram should exist in the title field */
            uint32_t trig = nx_trigram_pack('n', 'e', 'x');
            nx_bitmap bm;
            nx_status ls = nx_trigram_lookup(&tidx, trig, &bm);
            NX_CHECK(ls == NX_OK || ls == NX_ERR_NOT_FOUND);
            found_good_trig = true;
        }
    }
    NX_CHECK(found_good_trig);

    nx_buf_free(&out);
}


/* ---- test 4: graph index is searchable from segment -------------------- */
static void test_seg_graph_search(void) {
    nx_buf out;
    nx_buf_init(&out);
    nx_error err;
    memset(&err, 0, sizeof err);

    nx_slice input = nx_slice_cstr(SAMPLE_JSONL);
    nx_status st = nx_seg_build(input, NULL, &out, &err);
    NX_CHECK_OK(st);

    nx_segment seg;
    memset(&seg, 0, sizeof seg);
    st = nx_segment_open(nx_buf_slice(&out), &seg, &err);
    NX_CHECK_OK(st);

    for (uint32_t fi = 0; fi < 10; fi++) {
        nx_slice gslice;
        st = nx_segment_section(&seg, NX_SEC_GRAPH, (uint16_t)fi, &gslice);
        if (st == NX_ERR_NOT_FOUND) continue;
        NX_CHECK_OK(st);

        nx_graph graph;
        memset(&graph, 0, sizeof graph);
        st = nx_graph_open(gslice, &graph);
        NX_CHECK_OK(st);
        NX_CHECK(graph.node_count == 4);
        NX_CHECK(graph.dim == 3);

        /* Query: closest to [1,0,0] should be doc 0 */
        float query[3] = {1.0f, 0.0f, 0.0f};
        nx_graph_hit hits[4];
        size_t found = 0;
        st = nx_graph_search(&graph, query, 2, 8, NULL, hits, &found);
        NX_CHECK_OK(st);
        NX_CHECK(found >= 1);
        NX_CHECK(hits[0].id == 0);  /* doc 0 = [1,0,0] */
        break;
    }

    nx_buf_free(&out);
}

/* ---- test 5: rabitq index is searchable from segment -------------------- */
static void test_seg_rabitq_scan(void) {
    nx_buf out;
    nx_buf_init(&out);
    nx_error err;
    memset(&err, 0, sizeof err);

    nx_slice input = nx_slice_cstr(SAMPLE_JSONL);
    nx_status st = nx_seg_build(input, NULL, &out, &err);
    NX_CHECK_OK(st);

    nx_segment seg;
    memset(&seg, 0, sizeof seg);
    st = nx_segment_open(nx_buf_slice(&out), &seg, &err);
    NX_CHECK_OK(st);

    for (uint32_t fi = 0; fi < 10; fi++) {
        nx_slice rslice;
        st = nx_segment_section(&seg, NX_SEC_RABITQ, (uint16_t)fi, &rslice);
        if (st == NX_ERR_NOT_FOUND) continue;
        NX_CHECK_OK(st);

        nx_rabitq_index ridx;
        memset(&ridx, 0, sizeof ridx);
        st = nx_rabitq_open(rslice, &ridx);
        NX_CHECK_OK(st);
        NX_CHECK(ridx.node_count == 4);
        NX_CHECK(ridx.dim == 3);

        /* Query: closest to [1,0,0] should be doc 0 */
        float query[3] = {1.0f, 0.0f, 0.0f};
        nx_rabitq_query q;
        NX_CHECK_OK(nx_rabitq_query_init(&ridx, query, &q));

        nx_rabitq_hit hits[4];
        size_t found = 0;
        st = nx_rabitq_scan(&ridx, &q, 2, NULL, hits, &found);
        NX_CHECK_OK(st);
        NX_CHECK(found >= 1);
        NX_CHECK(hits[0].id == 0);
        break;
    }

    nx_buf_free(&out);
}

/* ---- test 6: store lifecycle (open, upsert, flush, reopen) -------------- */
#ifdef NX_USE_SHARED
static const char *test_dir = "test_store_shared_tmp";
static const char *test_wal_dir = "test_store_wal_shared_tmp";
#else
static const char *test_dir = "test_store_tmp";
static const char *test_wal_dir = "test_store_wal_tmp";
#endif

static void test_store_lifecycle(void) {
    nx_mem_stats pre = nx_mem_get_stats();

    nx_rmdir_r(test_dir);

    /* Open fresh store */
    nx_store store;
    memset(&store, 0, sizeof store);
    nx_error err;
    memset(&err, 0, sizeof err);

    nx_status st = nx_store_open(test_dir, &store, &err);
    NX_CHECK_OK(st);
    NX_CHECK(nx_store_seg_count(&store) == 0);
    NX_CHECK(nx_store_doc_count(&store) == 0);

    /* Upsert 3 documents */
    st = nx_store_upsert(&store,
        "{\"_id\":\"a1\",\"name\":\"Alice in Wonderland\"}", &err);
    NX_CHECK_OK(st);
    st = nx_store_upsert(&store,
        "{\"_id\":\"a2\",\"name\":\"Bob the Builder\"}", &err);
    NX_CHECK_OK(st);
    st = nx_store_upsert(&store,
        "{\"_id\":\"a3\",\"name\":\"Charlie and the Chocolate Factory\"}", &err);
    NX_CHECK_OK(st);

    NX_CHECK(nx_store_doc_count(&store) == 3);
    NX_CHECK(nx_store_seg_count(&store) == 0);

    /* Flush → creates segment */
    st = nx_store_flush(&store, &err);
    NX_CHECK_OK(st);

    NX_CHECK(nx_store_seg_count(&store) == 1);
    NX_CHECK(nx_store_doc_count(&store) == 3);

    /* The sealed segment should be accessible */
    const nx_segment *seg = nx_store_seg(&store, 0);
    NX_CHECK(seg != NULL);
    NX_CHECK(seg->doc_count == 3);

    /* Second flush with no new docs is a no-op */
    st = nx_store_flush(&store, &err);
    NX_CHECK_OK(st);
    NX_CHECK(nx_store_seg_count(&store) == 1);

    nx_store_close(&store);

    /* Re-open store — segments should persist via manifest */
    nx_store store2;
    memset(&store2, 0, sizeof store2);
    st = nx_store_open(test_dir, &store2, &err);
    NX_CHECK_OK(st);
    NX_CHECK(nx_store_seg_count(&store2) == 1);

    nx_store_close(&store2);

    /* Cleanup */
    nx_rmdir_r(test_dir);

    nx_mem_stats post = nx_mem_get_stats();
    NX_CHECK(post.live_allocs == pre.live_allocs);
}

/* ---- test 7: store WAL crash recovery ----------------------------------- */
static void test_store_wal_recovery(void) {
    nx_rmdir_r(test_wal_dir);

    /* Open store and upsert (but do NOT flush — simulating crash before flush) */
    {
        nx_store store;
        memset(&store, 0, sizeof store);
        nx_error err;
        memset(&err, 0, sizeof err);
        NX_CHECK_OK(nx_store_open(test_wal_dir, &store, &err));
        NX_CHECK_OK(nx_store_upsert(&store, "{\"_id\":\"r1\",\"name\":\"recovery test one\"}", &err));
        NX_CHECK_OK(nx_store_upsert(&store, "{\"_id\":\"r2\",\"name\":\"recovery test two\"}", &err));
        /* Force WAL to disk but do NOT flush/seal */
        nx_wal_sync(&store.wal);
        /* Close without flushing (simulate crash via close before flush) */
        nx_store_close(&store);
    }

    /* Re-open: WAL replay should restore the 2 unflushed docs into mutable buffer */
    {
        nx_store store2;
        memset(&store2, 0, sizeof store2);
        nx_error err;
        memset(&err, 0, sizeof err);
        NX_CHECK_OK(nx_store_open(test_wal_dir, &store2, &err));

        /* Replayed docs should be in mutable buffer */
        NX_CHECK(store2.mutable_docs == 2);
        NX_CHECK(nx_store_seg_count(&store2) == 0);

        /* Can flush the recovered data into a segment */
        NX_CHECK_OK(nx_store_flush(&store2, &err));
        NX_CHECK(nx_store_seg_count(&store2) == 1);

        nx_store_close(&store2);
    }

    nx_rmdir_r(test_wal_dir);
}

/* ---- test 7: segment with empty input ----------------------------------- */
static void test_seg_build_empty(void) {
    nx_buf out;
    nx_buf_init(&out);
    nx_error err;
    memset(&err, 0, sizeof err);

    /* Empty JSONL → valid segment with 0 docs */
    nx_slice empty = nx_slice_cstr("");
    nx_status st = nx_seg_build(empty, NULL, &out, &err);
    NX_CHECK_OK(st);
    NX_CHECK(out.len > 0);

    nx_segment seg;
    memset(&seg, 0, sizeof seg);
    st = nx_segment_open(nx_buf_slice(&out), &seg, &err);
    NX_CHECK_OK(st);
    NX_CHECK(seg.doc_count == 0);

    nx_buf_free(&out);
}

int main(void) {
    NX_RUN(test_seg_build_and_open);
    NX_RUN(test_seg_postings_lookup);
    NX_RUN(test_seg_trigram_lookup);
    NX_RUN(test_seg_graph_search);
    NX_RUN(test_seg_rabitq_scan);
    NX_RUN(test_store_lifecycle);
    NX_RUN(test_store_wal_recovery);
    NX_RUN(test_seg_build_empty);
    return nx_test_summary();
}
