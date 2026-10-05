/* RaBitQ: 1-Bit Randomized Vector Quantization for High-Throughput Approximate
 * Nearest Neighbor Search with Provable Theoretical Error Bounds.
 *
 * Based on Gao & Long (SIGMOD 2024, arXiv:2405.12497) and Section 03 research.
 *
 * Characteristics:
 * - 1-bit per dimension (~28x compression vs f32 raw vectors).
 * - Data-oblivious: no codebook or k-means training; uses centroid mean-centering
 *   and a deterministic orthogonal projection matrix P.
 * - Unbiased inner product estimator with provable Chebyshev-type error bounds:
 *     P(|ip_hat - <o, q>| > eps0 * err_o / sqrt(D' - 1)) <= 2 exp(-c0 * eps0^2)
 * - 4-bit query quantization with FastScan 4-bit LUT (both portable scalar and
 *   AVX2 bit-parallel table lookups).
 * - Exact zero-allocation reader view (nx_rabitq_open) with CRC32C integrity.
 *
 * Binary Layout:
 * - 48-byte LE header: magic "NXRBT1\0", version=1, dim, padded_dim, node_count,
 *   metric (0=L2SQ, 1=COSINE, 2=DOT), eps0 (float), seed (uint64), CRC32C.
 * - Centroid: float[dim]
 * - Orthogonal Matrix P: float[padded_dim * padded_dim] (column-major)
 * - Doc Metadata: nx_rabitq_doc[node_count]
 * - Quantized 1-bit Codes: uint8_t[node_count * (padded_dim / 8)]
 */
#ifndef NX_RABITQ_H
#define NX_RABITQ_H

#include "core/nx_config.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"
#include "index/nx_vec.h"
#include "index/nx_bitmap.h"

#define NX_RABITQ_MAGIC "NXRBT1\0"
#define NX_RABITQ_MAX_DIM 4096u
#define NX_RABITQ_DEFAULT_EPS0 1.9f

typedef struct nx_rabitq_config {
    uint32_t dim;            /* original vector dimensionality */
    uint64_t seed;           /* seed for orthogonal projection P (0 = default 42) */
    float eps0;              /* lower bound tightness knob (default 1.9) */
    nx_vec_metric metric;    /* NX_VEC_L2SQ or NX_VEC_COSINE */
} nx_rabitq_config;

static inline nx_rabitq_config nx_rabitq_default_config(uint32_t dim, nx_vec_metric metric) {
    nx_rabitq_config cfg;
    cfg.dim = dim;
    cfg.seed = 42;
    cfg.eps0 = NX_RABITQ_DEFAULT_EPS0;
    cfg.metric = metric;
    return cfg;
}

/* Per-document quantization metadata for error bounds and score reconstruction */
typedef struct nx_rabitq_doc {
    float no;                /* |o_r - c| (norm of centered document vector) */
    float inv_oo;            /* 1.0 / oo (bias correction factor) */
    float err_o;             /* sqrt(max(0, 1 - oo^2)) / oo (error bound coefficient) */
    uint16_t pc;             /* popcount of 1-bit codes x_b */
    uint16_t reserved;       /* padding */
    float dot_c;             /* <o_r, c> (for cosine / inner-product reconstruction) */
    float orig_norm_sq;      /* |o_r|^2 */
} nx_rabitq_doc;

/* Read-only zero-allocation index view */
typedef struct nx_rabitq_index {
    nx_slice bytes;
    uint32_t node_count;
    uint32_t dim;
    uint32_t padded_dim;     /* D' rounded up to next multiple of 32 */
    uint32_t code_bytes;     /* padded_dim / 8 */
    nx_vec_metric metric;
    float eps0;
    uint64_t seed;
    float c_norm_sq;         /* |c|^2 */
    const float *centroid;   /* points into bytes, dim floats */
    const float *proj_matrix;/* points into bytes, padded_dim * padded_dim floats */
    const nx_rabitq_doc *docs;/* points into bytes, node_count entries */
    const uint8_t *codes;    /* points into bytes, node_count * code_bytes */
} nx_rabitq_index;

/* Precomputed query state for high-throughput scanning */
typedef struct nx_rabitq_query {
    float nq;                /* |q_r - c| */
    float dot_qc;            /* <q_r, c> */
    float q_norm_sq;         /* |q_r|^2 */
    float K1, K2, K3;        /* query scaling factors */
    uint32_t padded_dim;
    uint32_t code_bytes;
    /* 4-bit FastScan Look-Up Table:
     * (padded_dim / 4) groups of 16 uint8 values */
    uint8_t lut[NX_RABITQ_MAX_DIM * 4];
} nx_rabitq_query;

typedef struct nx_rabitq_hit {
    uint32_t id;
    double dist;             /* estimated distance (or similarity) */
    double lower_bound;      /* theoretical lower bound */
} nx_rabitq_hit;

/* Build an immutable RaBitQ index from contiguous float32 vectors.
 * vectors: node_count * dim floats.
 * Appends serialized index to out buffer transactionally. */
NX_API nx_status nx_rabitq_build(const float *vectors, size_t node_count, size_t dim,
                                 const nx_rabitq_config *config, nx_buf *out);

/* Open an immutable RaBitQ index from serialized bytes. Zero allocation. */
NX_API nx_status nx_rabitq_open(nx_slice bytes, nx_rabitq_index *out);

/* Prepare a query: projects query vector, applies 4-bit quantization, and
 * precomputes the FastScan LUT. Scratch buffer not required. */
NX_API nx_status nx_rabitq_query_init(const nx_rabitq_index *idx, const float *query,
                                      nx_rabitq_query *out_q);

/* Estimate distance / similarity for a single candidate doc_id.
 * If out_lb is non-NULL, stores the theoretical lower bound. */
NX_API nx_status nx_rabitq_estimate(const nx_rabitq_index *idx, const nx_rabitq_query *q,
                                    uint32_t doc_id, double *out_dist, double *out_lb);

/* High-throughput FastScan across candidates:
 * Returns the top-k nearest neighbors (sorted by estimated distance/similarity).
 * If allowed is non-NULL, only documents present in allowed are considered. */
NX_API nx_status nx_rabitq_scan(const nx_rabitq_index *idx, const nx_rabitq_query *q,
                                size_t k, const nx_bitmap *allowed,
                                nx_rabitq_hit *out_hits, size_t *out_count);

#endif
