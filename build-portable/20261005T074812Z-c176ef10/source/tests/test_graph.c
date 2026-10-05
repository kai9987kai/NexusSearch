#include "core/nx_config.h"
#include "nx_test.h"
#include "index/nx_graph.h"
#include <string.h>

static void test_graph_build_and_search(void) {
    /* 5 vectors of dimension 3 */
    float vecs[] = {
        1.0f, 0.0f, 0.0f, /* node 0: X axis */
        0.0f, 1.0f, 0.0f, /* node 1: Y axis */
        0.0f, 0.0f, 1.0f, /* node 2: Z axis */
        0.9f, 0.1f, 0.0f, /* node 3: near X axis */
        0.1f, 0.9f, 0.0f  /* node 4: near Y axis */
    };

    nx_graph_config cfg = nx_graph_default_config();
    cfg.R = 4;
    cfg.ef_construction = 8;
    cfg.metric = NX_VEC_COSINE;

    nx_buf index_buf;
    nx_buf_init(&index_buf);
    NX_REQUIRE(nx_graph_build(vecs, 5, 3, &cfg, &index_buf) == NX_OK);
    NX_CHECK(index_buf.len > 48);

    nx_graph g;
    NX_REQUIRE(nx_graph_open(nx_buf_slice(&index_buf), &g) == NX_OK);
    NX_CHECK_EQ_U(g.node_count, 5);
    NX_CHECK_EQ_U(g.dim, 3);
    NX_CHECK_EQ_U(g.R, 4);

    /* 1. Unfiltered query: search for [1.0, 0.0, 0.0] */
    float q[] = {1.0f, 0.0f, 0.0f};
    nx_graph_hit hits[5];
    size_t hit_count = 0;
    NX_CHECK_OK(nx_graph_search(&g, q, 3, 8, NULL, hits, &hit_count));
    NX_CHECK_EQ_U(hit_count, 3);

    /* Top hit must be node 0 (exact match) */
    NX_CHECK_EQ_U(hits[0].id, 0);
    NX_CHECK(hits[0].score > 0.99);

    /* Second hit must be node 3 (near X axis) */
    NX_CHECK_EQ_U(hits[1].id, 3);
    NX_CHECK(hits[1].score > 0.9);

    /* 2. Filtered search: only allow nodes {1, 2, 4} (exclude 0 and 3) */
    uint32_t allowed_ids[] = {1, 2, 4};
    nx_buf bm_buf;
    nx_buf_init(&bm_buf);
    NX_REQUIRE(nx_bitmap_build(allowed_ids, 3, &bm_buf) == NX_OK);
    nx_bitmap allowed;
    NX_REQUIRE(nx_bitmap_open(nx_buf_slice(&bm_buf), &allowed) == NX_OK);

    memset(hits, 0, sizeof(hits));
    NX_CHECK_OK(nx_graph_search(&g, q, 3, 8, &allowed, hits, &hit_count));
    NX_CHECK_EQ_U(hit_count, 3);

    /* None of the hits can be node 0 or node 3 */
    for (size_t i = 0; i < hit_count; ++i) {
        NX_CHECK(hits[i].id == 1 || hits[i].id == 2 || hits[i].id == 4);
    }
    /* Among {1, 2, 4}, node 4 has highest X-component (0.1) */
    NX_CHECK_EQ_U(hits[0].id, 4);

    nx_buf_free(&bm_buf);
    nx_buf_free(&index_buf);
}

static void test_graph_corruptions(void) {
    float vecs[] = {1.0f, 0.0f, 0.0f, 1.0f};
    nx_buf index_buf;
    nx_buf_init(&index_buf);
    NX_REQUIRE(nx_graph_build(vecs, 2, 2, NULL, &index_buf) == NX_OK);

    /* 1. Truncated */
    nx_graph g;
    NX_CHECK_EQ_I(nx_graph_open(nx_slice_make(index_buf.data, 30), &g), NX_ERR_CORRUPT);

    /* 2. Corrupted CRC */
    uint8_t copy[1024];
    NX_REQUIRE(index_buf.len <= sizeof(copy));
    memcpy(copy, index_buf.data, index_buf.len);
    copy[index_buf.len - 1] ^= 0x33;
    NX_CHECK_EQ_I(nx_graph_open(nx_slice_make(copy, index_buf.len), &g), NX_ERR_CORRUPT);

    /* 3. Bad magic */
    memcpy(copy, index_buf.data, index_buf.len);
    copy[0] = 'X';
    NX_CHECK_EQ_I(nx_graph_open(nx_slice_make(copy, index_buf.len), &g), NX_ERR_CORRUPT);

    nx_buf_free(&index_buf);
}

int main(void) {
    NX_RUN(test_graph_build_and_search);
    NX_RUN(test_graph_corruptions);
    return nx_test_summary();
}
