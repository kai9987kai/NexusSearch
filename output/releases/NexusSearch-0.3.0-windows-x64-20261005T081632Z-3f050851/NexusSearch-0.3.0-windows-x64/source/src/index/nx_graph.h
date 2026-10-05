/* Flat Navigable Proximity Graph for Approximate Nearest Neighbor (ANN) Search.
 * Based on FlatNav / Hub Highway Hypothesis (ICML 2025) and Vamana / RobustPrune
 * (NeurIPS 2019, Section 02 research).
 *
 * Implements high-throughput graph-based vector search:
 * - Cache-aligned flat adjacency lists with bounded degree R
 * - RobustPrune (alpha >= 1.0) for long-range navigation and high recall
 * - Filtered ANN: Navigates graph while constraining results to an optional
 *   Roaring bitmap (nx_bitmap) of allowed candidates
 * - Zero-allocation immutable reader (nx_graph_open) with CRC32C validation.
 */
#ifndef NX_GRAPH_H
#define NX_GRAPH_H

#include "core/nx_config.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"
#include "index/nx_vec.h"
#include "index/nx_bitmap.h"

#define NX_GRAPH_MAGIC "NXGRP1\0"
#define NX_GRAPH_MAX_NODES (1000000u)
#define NX_GRAPH_MAX_DIM   (4096u)
#define NX_GRAPH_DEFAULT_R (32u)
#define NX_GRAPH_DEFAULT_EF_CONSTRUCT (64u)
#define NX_GRAPH_DEFAULT_ALPHA (1.2f)

typedef struct nx_graph_config {
    uint32_t R;               /* Maximum out-degree per node (default: 32) */
    uint32_t ef_construction; /* Search beam width during build (default: 64) */
    float alpha;              /* RobustPrune diversity parameter (default: 1.2) */
    nx_vec_metric metric;     /* Distance metric (default: NX_VEC_COSINE) */
} nx_graph_config;

static inline nx_graph_config nx_graph_default_config(void) {
    nx_graph_config cfg = {
        NX_GRAPH_DEFAULT_R,
        NX_GRAPH_DEFAULT_EF_CONSTRUCT,
        NX_GRAPH_DEFAULT_ALPHA,
        NX_VEC_COSINE
    };
    return cfg;
}

typedef struct nx_graph {
    nx_slice bytes;
    uint32_t node_count;
    uint32_t dim;
    uint32_t R;
    nx_vec_metric metric;
    uint32_t entry_node;
    const uint32_t *edges;    /* node_count * (R + 1) uint32s: [count, id0, id1, ...] */
    const float *vectors;     /* node_count * dim floats */
} nx_graph;

typedef struct nx_graph_hit {
    uint32_t id;
    double score;             /* similarity score (higher = closer) */
} nx_graph_hit;

/* Build an immutable flat proximity graph from a contiguous array of float vectors.
 * vectors: node_count * dim float array.
 * Appends serialized graph to out buffer transactionally. */
NX_API nx_status nx_graph_build(const float *vectors, size_t node_count, size_t dim,
                                const nx_graph_config *config, nx_buf *out);

/* Open an immutable graph from serialized bytes.
 * Validates CRC32C, magic, version, node count, and edge bounds. Zero allocation. */
NX_API nx_status nx_graph_open(nx_slice bytes, nx_graph *out);

/* Approximate Nearest Neighbor search on the graph:
 * query: float vector of size graph->dim
 * k: maximum number of nearest neighbors to return
 * ef: beam exploration width (must be >= k, typically 32-128)
 * allowed: optional Roaring bitmap of allowed doc IDs (NULL = all nodes allowed)
 * out_hits: caller-allocated buffer with capacity >= k
 * out_count: receives number of hits found
 */
NX_API nx_status nx_graph_search(const nx_graph *graph, const float *query, size_t k, size_t ef,
                                 const nx_bitmap *allowed, nx_graph_hit *out_hits, size_t *out_count);

#endif
