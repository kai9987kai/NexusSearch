/* test_rabitq.c - Unit tests for RaBitQ 1-bit vector quantization */
#include "nx_test.h"
#include "index/nx_rabitq.h"
#include "index/nx_vec.h"
#include "core/nx_mem.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* Simple pseudo-random float generator */
static float rand_f32(uint32_t *seed) {
    *seed = (*seed * 1664525u + 1013904223u);
    return (float)(*seed & 0xFFFF) / 65536.0f;
}

/* ---- test 1: build, open, and CRC validation --------------------------- */
static void test_rabitq_build_open(void) {
    nx_mem_stats pre = nx_mem_get_stats();

    const size_t N = 64;
    const size_t D = 32;
    float *vectors = NX_NEW_ARRAY(float, N * D);
    NX_REQUIRE(vectors != NULL);

    uint32_t seed = 12345;
    for (size_t i = 0; i < N * D; i++) {
        vectors[i] = rand_f32(&seed);
    }

    nx_rabitq_config cfg = nx_rabitq_default_config((uint32_t)D, NX_VEC_L2SQ);
    cfg.seed = 42;
    cfg.eps0 = 1.9f;

    nx_buf buf;
    nx_buf_init(&buf);

    nx_status st = nx_rabitq_build(vectors, N, D, &cfg, &buf);
    NX_CHECK_OK(st);
    NX_CHECK(buf.len > 0);

    /* Open the index */
    nx_rabitq_index idx;
    st = nx_rabitq_open(nx_buf_slice(&buf), &idx);
    NX_CHECK_OK(st);
    NX_CHECK(idx.node_count == (uint32_t)N);
    NX_CHECK(idx.dim == (uint32_t)D);
    NX_CHECK(idx.padded_dim >= (uint32_t)D);
    NX_CHECK(idx.padded_dim % 32 == 0);
    NX_CHECK(idx.metric == NX_VEC_L2SQ);

    /* Check corrupt CRC */
    uint8_t *corrupt_data = (uint8_t *)nx_malloc(buf.len);
    NX_REQUIRE(corrupt_data != NULL);
    memcpy(corrupt_data, buf.data, buf.len);
    corrupt_data[buf.len - 1] ^= 0xFF;  /* flip bit */

    nx_rabitq_index corrupt_idx;
    st = nx_rabitq_open(nx_slice_make(corrupt_data, buf.len), &corrupt_idx);
    NX_CHECK(st == NX_ERR_CORRUPT);

    nx_free(corrupt_data);
    nx_buf_free(&buf);
    nx_free(vectors);

    nx_mem_stats post = nx_mem_get_stats();
    NX_CHECK(post.live_allocs == pre.live_allocs);
}

/* ---- test 2: distance estimation accuracy & lower bound ---------------- */
static void test_rabitq_accuracy_and_bound(void) {
    const size_t N = 100;
    const size_t D = 64;
    float *vectors = NX_NEW_ARRAY(float, N * D);
    NX_REQUIRE(vectors != NULL);

    uint32_t seed = 999;
    for (size_t i = 0; i < N * D; i++) {
        vectors[i] = rand_f32(&seed);
    }

    nx_rabitq_config cfg = nx_rabitq_default_config((uint32_t)D, NX_VEC_L2SQ);
    nx_buf buf;
    nx_buf_init(&buf);

    NX_CHECK_OK(nx_rabitq_build(vectors, N, D, &cfg, &buf));

    nx_rabitq_index idx;
    NX_CHECK_OK(nx_rabitq_open(nx_buf_slice(&buf), &idx));

    /* Query with first document */
    float query[64];
    memcpy(query, vectors, D * sizeof(float));

    nx_rabitq_query q;
    NX_CHECK_OK(nx_rabitq_query_init(&idx, query, &q));

    /* Distance to self should be very small */
    double dist_self = 0.0, lb_self = 0.0;
    NX_CHECK_OK(nx_rabitq_estimate(&idx, &q, 0, &dist_self, &lb_self));
    NX_CHECK(dist_self < 2.0);  /* quantized approximation */
    NX_CHECK(lb_self <= dist_self + 1e-6); /* lower bound condition */

    /* Verify lower bound property across all documents: lb <= dist */
    size_t lb_valid_count = 0;
    for (uint32_t i = 0; i < N; i++) {
        double d = 0.0, lb = 0.0;
        NX_CHECK_OK(nx_rabitq_estimate(&idx, &q, i, &d, &lb));
        if (lb <= d + 1e-6) {
            lb_valid_count++;
        }
    }
    NX_CHECK(lb_valid_count == N);

    nx_buf_free(&buf);
    nx_free(vectors);
}

/* ---- test 3: top-k scanning and ranking --------------------------------- */
static void test_rabitq_scan(void) {
    const size_t N = 50;
    const size_t D = 32;
    float *vectors = NX_NEW_ARRAY(float, N * D);
    NX_REQUIRE(vectors != NULL);

    uint32_t seed = 42;
    for (size_t i = 0; i < N * D; i++) {
        vectors[i] = rand_f32(&seed);
    }

    nx_rabitq_config cfg = nx_rabitq_default_config((uint32_t)D, NX_VEC_L2SQ);
    nx_buf buf;
    nx_buf_init(&buf);
    NX_CHECK_OK(nx_rabitq_build(vectors, N, D, &cfg, &buf));

    nx_rabitq_index idx;
    NX_CHECK_OK(nx_rabitq_open(nx_buf_slice(&buf), &idx));

    /* Query matching doc 10 */
    float query[32];
    memcpy(query, vectors + 10 * D, D * sizeof(float));

    nx_rabitq_query q;
    NX_CHECK_OK(nx_rabitq_query_init(&idx, query, &q));

    nx_rabitq_hit hits[5];
    size_t count = 0;
    NX_CHECK_OK(nx_rabitq_scan(&idx, &q, 5, NULL, hits, &count));
    NX_CHECK(count == 5);
    /* Closest hit should be doc 10 */
    NX_CHECK(hits[0].id == 10);

    /* Hits must be in ascending order of estimated distance */
    for (size_t i = 1; i < count; i++) {
        NX_CHECK(hits[i].dist >= hits[i - 1].dist);
    }

    nx_buf_free(&buf);
    nx_free(vectors);
}

/* ---- test 4: cosine metric mode ---------------------------------------- */
static void test_rabitq_cosine(void) {
    const size_t N = 30;
    const size_t D = 32;
    float *vectors = NX_NEW_ARRAY(float, N * D);
    NX_REQUIRE(vectors != NULL);

    uint32_t seed = 777;
    for (size_t i = 0; i < N * D; i++) {
        vectors[i] = rand_f32(&seed);
    }

    nx_rabitq_config cfg = nx_rabitq_default_config((uint32_t)D, NX_VEC_COSINE);
    nx_buf buf;
    nx_buf_init(&buf);
    NX_CHECK_OK(nx_rabitq_build(vectors, N, D, &cfg, &buf));

    nx_rabitq_index idx;
    NX_CHECK_OK(nx_rabitq_open(nx_buf_slice(&buf), &idx));
    NX_CHECK(idx.metric == NX_VEC_COSINE);

    /* Query doc 5 */
    float query[32];
    memcpy(query, vectors + 5 * D, D * sizeof(float));

    nx_rabitq_query q;
    NX_CHECK_OK(nx_rabitq_query_init(&idx, query, &q));

    nx_rabitq_hit hits[3];
    size_t count = 0;
    NX_CHECK_OK(nx_rabitq_scan(&idx, &q, 3, NULL, hits, &count));
    NX_CHECK(count == 3);
    /* For cosine, higher similarity is better */
    NX_CHECK(hits[0].id == 5);
    NX_CHECK(hits[0].dist > 0.8);
    for (size_t i = 1; i < count; i++) {
        NX_CHECK(hits[i].dist <= hits[i - 1].dist);
    }

    nx_buf_free(&buf);
    nx_free(vectors);
}

int main(void) {
    NX_RUN(test_rabitq_build_open);
    NX_RUN(test_rabitq_accuracy_and_bound);
    NX_RUN(test_rabitq_scan);
    NX_RUN(test_rabitq_cosine);
    return nx_test_summary();
}
