/* nx_rabitq.c - RaBitQ 1-Bit Vector Quantization Implementation.
 *
 * Implements Gao & Long (SIGMOD 2024) 1-bit randomized vector quantization
 * with 4-bit query quantization, FastScan LUT, and provable error bound estimation.
 */
#include "index/nx_rabitq.h"
#include "core/nx_mem.h"
#include "core/nx_crc32c.h"
#include "core/nx_simd.h"
#include <math.h>
#include <string.h>
#include <float.h>

#define HEADER_SIZE 48u
#define M_PI_F 3.14159265358979323846f

/* SplitMix64 PRNG for deterministic, reproducible pseudo-random numbers */
static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* Generate standard normal random variable using Box-Muller transform */
static float rand_normal(uint64_t *state) {
    /* 53-bit resolution uniform random double in (0, 1) */
    double u1 = ((double)(splitmix64(state) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    double u2 = ((double)(splitmix64(state) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    double r = sqrt(-2.0 * log(u1));
    double theta = 2.0 * (double)M_PI_F * u2;
    return (float)(r * cos(theta));
}

/* Generate a deterministic D' x D' orthogonal matrix via modified Gram-Schmidt */
static nx_status generate_orthogonal_matrix(uint32_t dim, uint64_t seed, float *P) {
    uint64_t rng = seed ? seed : 42ULL;
    size_t d = (size_t)dim;

    /* Fill P with i.i.d standard normals */
    for (size_t i = 0; i < d * d; i++) {
        P[i] = rand_normal(&rng);
    }

    /* Modified Gram-Schmidt column by column */
    for (size_t j = 0; j < d; j++) {
        float *v_j = P + j * d;
        for (size_t i = 0; i < j; i++) {
            const float *v_i = P + i * d;
            double dot = 0.0;
            for (size_t k = 0; k < d; k++) {
                dot += (double)v_i[k] * (double)v_j[k];
            }
            float fdot = (float)dot;
            for (size_t k = 0; k < d; k++) {
                v_j[k] -= fdot * v_i[k];
            }
        }
        double norm_sq = 0.0;
        for (size_t k = 0; k < d; k++) {
            norm_sq += (double)v_j[k] * (double)v_j[k];
        }
        if (norm_sq < 1e-12) {
            /* Degenerate column, refill with standard normals and retry */
            for (size_t k = 0; k < d; k++) v_j[k] = rand_normal(&rng);
            j--;
            continue;
        }
        float inv_norm = (float)(1.0 / sqrt(norm_sq));
        for (size_t k = 0; k < d; k++) {
            v_j[k] *= inv_norm;
        }
    }
    return NX_OK;
}

/* Round dim up to next multiple of 32 */
static inline uint32_t pad_dim32(uint32_t dim) {
    return (dim + 31u) & ~31u;
}

/* Count set bits in byte */
static inline uint32_t popcnt8(uint8_t x) {
    x = (uint8_t)(x - ((x >> 1) & 0x55));
    x = (uint8_t)((x & 0x33) + ((x >> 2) & 0x33));
    return (uint32_t)((x + (x >> 4)) & 0x0F);
}

nx_status nx_rabitq_build(const float *vectors, size_t node_count, size_t dim,
                          const nx_rabitq_config *config, nx_buf *out) {
    if (!vectors || node_count == 0 || dim == 0 || dim > NX_RABITQ_MAX_DIM || !out) {
        return NX_ERR_INVALID;
    }

    nx_rabitq_config cfg = config ? *config : nx_rabitq_default_config((uint32_t)dim, NX_VEC_L2SQ);
    if (cfg.seed == 0) cfg.seed = 42;
    if (cfg.eps0 <= 0.0f) cfg.eps0 = NX_RABITQ_DEFAULT_EPS0;

    uint32_t padded_dim = pad_dim32((uint32_t)dim);
    uint32_t code_bytes = padded_dim / 8u;
    size_t start_len = out->len;

    /* 1. Compute dataset centroid */
    float *centroid = NX_NEW_ARRAY(float, dim);
    if (!centroid) return NX_ERR_NOMEM;
    memset(centroid, 0, dim * sizeof(float));

    for (size_t i = 0; i < node_count; i++) {
        const float *v = vectors + i * dim;
        for (size_t d = 0; d < dim; d++) {
            centroid[d] += v[d];
        }
    }
    float inv_n = 1.0f / (float)node_count;
    for (size_t d = 0; d < dim; d++) {
        centroid[d] *= inv_n;
    }

    /* Centroid norm squared */
    double c_norm_sq = 0.0;
    for (size_t d = 0; d < dim; d++) {
        c_norm_sq += (double)centroid[d] * (double)centroid[d];
    }

    /* 2. Generate D' x D' orthogonal matrix P */
    float *proj_matrix = NX_NEW_ARRAY(float, (size_t)padded_dim * padded_dim);
    if (!proj_matrix) {
        nx_free(centroid);
        return NX_ERR_NOMEM;
    }
    generate_orthogonal_matrix(padded_dim, cfg.seed, proj_matrix);

    /* 3. Quantize each vector */
    nx_rabitq_doc *docs = NX_NEW_ARRAY(nx_rabitq_doc, node_count);
    uint8_t *codes = NX_NEW_ARRAY(uint8_t, (size_t)node_count * code_bytes);
    float *centered = NX_NEW_ARRAY(float, padded_dim);
    float *rotated = NX_NEW_ARRAY(float, padded_dim);

    if (!docs || !codes || !centered || !rotated) {
        nx_free(centroid);
        nx_free(proj_matrix);
        nx_free(docs);
        nx_free(codes);
        nx_free(centered);
        nx_free(rotated);
        return NX_ERR_NOMEM;
    }

    float sqrt_d = sqrtf((float)padded_dim);

    for (size_t i = 0; i < node_count; i++) {
        const float *v = vectors + i * dim;
        nx_rabitq_doc *doc = &docs[i];
        memset(doc, 0, sizeof(*doc));

        /* Original norm and dot with centroid */
        double orig_norm_sq = 0.0;
        double dot_c = 0.0;
        for (size_t d = 0; d < dim; d++) {
            orig_norm_sq += (double)v[d] * (double)v[d];
            dot_c += (double)v[d] * (double)centroid[d];
        }
        doc->orig_norm_sq = (float)orig_norm_sq;
        doc->dot_c = (float)dot_c;

        /* Centered vector r = v - c */
        double diff_norm_sq = 0.0;
        for (size_t d = 0; d < dim; d++) {
            float diff = v[d] - centroid[d];
            centered[d] = diff;
            diff_norm_sq += (double)diff * (double)diff;
        }
        /* Zero pad remaining dimensions */
        for (size_t d = dim; d < padded_dim; d++) {
            centered[d] = 0.0f;
        }

        float no = (float)sqrt(diff_norm_sq);
        doc->no = no;

        if (no < 1e-12f) {
            /* Zero residual */
            doc->inv_oo = 1.0f;
            doc->err_o = 0.0f;
            doc->pc = 0;
            memset(codes + i * code_bytes, 0, code_bytes);
            continue;
        }

        /* Normalize r */
        float inv_no = 1.0f / no;
        for (size_t d = 0; d < padded_dim; d++) {
            centered[d] *= inv_no;
        }

        /* Rotate: o' = P^T * centered */
        for (size_t j = 0; j < padded_dim; j++) {
            const float *col_j = proj_matrix + j * padded_dim;
            double sum = 0.0;
            for (size_t k = 0; k < padded_dim; k++) {
                sum += (double)col_j[k] * (double)centered[k];
            }
            rotated[j] = (float)sum;
        }

        /* 1-bit quantization: sign bits */
        uint8_t *doc_code = codes + i * code_bytes;
        memset(doc_code, 0, code_bytes);
        double abs_sum = 0.0;
        uint32_t pc = 0;

        for (size_t d = 0; d < padded_dim; d++) {
            float val = rotated[d];
            abs_sum += (double)fabsf(val);
            if (val > 0.0f) {
                doc_code[d / 8u] |= (uint8_t)(1u << (d % 8u));
                pc++;
            }
        }
        doc->pc = (uint16_t)pc;

        /* Normalization factor oo = (sum |o'[d]|) / sqrt(D') */
        float oo = (float)(abs_sum / (double)sqrt_d);
        if (oo < 1e-6f) oo = 1e-6f;
        doc->inv_oo = 1.0f / oo;

        float var = 1.0f - oo * oo;
        doc->err_o = var > 0.0f ? sqrtf(var) / oo : 0.0f;
    }

    nx_free(centered);
    nx_free(rotated);

    /* 4. Serialize into output buffer */
    uint8_t hdr[HEADER_SIZE];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, NX_RABITQ_MAGIC, 8);
    nx_st16(hdr + 8,  1u);                    /* version = 1 */
    nx_st16(hdr + 10, (uint16_t)cfg.metric);
    nx_st32(hdr + 12, (uint32_t)dim);
    nx_st32(hdr + 16, padded_dim);
    nx_st32(hdr + 20, (uint32_t)node_count);
    nx_stf32(hdr + 24, cfg.eps0);
    nx_stf32(hdr + 28, (float)c_norm_sq);
    nx_st64(hdr + 32, cfg.seed);
    nx_st32(hdr + 40, 0u);                    /* reserved */

    /* Compute CRC32C over header (without CRC field) and all payload data */
    uint32_t crc = nx_crc32c(0, hdr, 44);
    crc = nx_crc32c(crc, centroid, dim * sizeof(float));
    crc = nx_crc32c(crc, proj_matrix, (size_t)padded_dim * padded_dim * sizeof(float));
    crc = nx_crc32c(crc, docs, node_count * sizeof(nx_rabitq_doc));
    crc = nx_crc32c(crc, codes, (size_t)node_count * code_bytes);
    nx_st32(hdr + 44, crc);

    /* Append to buffer */
    nx_buf_put(out, hdr, sizeof(hdr));
    nx_buf_put(out, centroid, dim * sizeof(float));
    nx_buf_put(out, proj_matrix, (size_t)padded_dim * padded_dim * sizeof(float));
    nx_buf_put(out, docs, node_count * sizeof(nx_rabitq_doc));
    nx_buf_put(out, codes, (size_t)node_count * code_bytes);

    nx_free(centroid);
    nx_free(proj_matrix);
    nx_free(docs);
    nx_free(codes);

    if (out->oom) {
        out->len = start_len;
        return NX_ERR_NOMEM;
    }
    return NX_OK;
}

nx_status nx_rabitq_open(nx_slice bytes, nx_rabitq_index *out) {
    if (!out) return NX_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    if (!bytes.p || bytes.n < HEADER_SIZE) return NX_ERR_CORRUPT;

    const uint8_t *p = bytes.p;
    if (memcmp(p, NX_RABITQ_MAGIC, 8) != 0) return NX_ERR_CORRUPT;

    uint16_t ver = nx_ld16(p + 8);
    if (ver != 1u) return NX_ERR_VERSION;

    nx_vec_metric metric = (nx_vec_metric)nx_ld16(p + 10);
    uint32_t dim = nx_ld32(p + 12);
    uint32_t padded_dim = nx_ld32(p + 16);
    uint32_t node_count = nx_ld32(p + 20);
    float eps0 = nx_ldf32(p + 24);
    float c_norm_sq = nx_ldf32(p + 28);
    uint64_t seed = nx_ld64(p + 32);
    uint32_t stored_crc = nx_ld32(p + 44);

    if (dim == 0 || dim > NX_RABITQ_MAX_DIM || padded_dim < dim || padded_dim % 32 != 0) {
        return NX_ERR_CORRUPT;
    }

    uint32_t code_bytes = padded_dim / 8u;
    size_t centroid_sz = (size_t)dim * sizeof(float);
    size_t proj_sz = (size_t)padded_dim * padded_dim * sizeof(float);
    size_t docs_sz = (size_t)node_count * sizeof(nx_rabitq_doc);
    size_t codes_sz = (size_t)node_count * code_bytes;

    size_t expected_total = HEADER_SIZE + centroid_sz + proj_sz + docs_sz + codes_sz;
    if (bytes.n != expected_total) return NX_ERR_CORRUPT;

    /* Verify CRC */
    uint32_t crc = nx_crc32c(0, p, 44);
    crc = nx_crc32c(crc, p + HEADER_SIZE, bytes.n - HEADER_SIZE);
    if (crc != stored_crc) return NX_ERR_CORRUPT;

    /* Populate zero-allocation views */
    out->bytes = bytes;
    out->node_count = node_count;
    out->dim = dim;
    out->padded_dim = padded_dim;
    out->code_bytes = code_bytes;
    out->metric = metric;
    out->eps0 = eps0;
    out->c_norm_sq = c_norm_sq;
    out->seed = seed;

    size_t off = HEADER_SIZE;
    out->centroid = (const float *)(p + off);
    off += centroid_sz;
    out->proj_matrix = (const float *)(p + off);
    off += proj_sz;
    out->docs = (const nx_rabitq_doc *)(p + off);
    off += docs_sz;
    out->codes = p + off;

    return NX_OK;
}

nx_status nx_rabitq_query_init(const nx_rabitq_index *idx, const float *query,
                               nx_rabitq_query *out_q) {
    if (!idx || !query || !out_q) return NX_ERR_INVALID;
    memset(out_q, 0, sizeof(*out_q));

    uint32_t dim = idx->dim;
    uint32_t padded_dim = idx->padded_dim;
    out_q->padded_dim = padded_dim;
    out_q->code_bytes = idx->code_bytes;

    /* Original query norm and dot with centroid */
    double q_norm_sq = 0.0;
    double dot_qc = 0.0;
    for (size_t d = 0; d < dim; d++) {
        q_norm_sq += (double)query[d] * (double)query[d];
        dot_qc += (double)query[d] * (double)idx->centroid[d];
    }
    out_q->q_norm_sq = (float)q_norm_sq;
    out_q->dot_qc = (float)dot_qc;

    /* r_q = query - centroid */
    float r_q[NX_RABITQ_MAX_DIM];
    double diff_norm_sq = 0.0;
    for (size_t d = 0; d < dim; d++) {
        float diff = query[d] - idx->centroid[d];
        r_q[d] = diff;
        diff_norm_sq += (double)diff * (double)diff;
    }
    for (size_t d = dim; d < padded_dim; d++) {
        r_q[d] = 0.0f;
    }

    float nq = (float)sqrt(diff_norm_sq);
    out_q->nq = nq;

    if (nq < 1e-12f) {
        /* Centroid query, no rotation needed */
        out_q->K1 = 0.0f;
        out_q->K2 = 0.0f;
        out_q->K3 = 0.0f;
        return NX_OK;
    }

    float inv_nq = 1.0f / nq;
    for (size_t d = 0; d < padded_dim; d++) {
        r_q[d] *= inv_nq;
    }

    /* Rotate: q' = P^T * r_q */
    float q_prime[NX_RABITQ_MAX_DIM];
    float vl = FLT_MAX;
    float vr = -FLT_MAX;

    for (size_t j = 0; j < padded_dim; j++) {
        const float *col_j = idx->proj_matrix + j * padded_dim;
        double sum = 0.0;
        for (size_t k = 0; k < padded_dim; k++) {
            sum += (double)col_j[k] * (double)r_q[k];
        }
        float val = (float)sum;
        q_prime[j] = val;
        if (val < vl) vl = val;
        if (val > vr) vr = val;
    }

    float delta = (vr - vl) / 15.0f;
    if (delta < 1e-8f) delta = 1e-8f;
    float inv_delta = 1.0f / delta;

    /* 4-bit uniform quantization: qu[i] in 0..15 */
    uint8_t qu[NX_RABITQ_MAX_DIM];
    uint32_t sum_qu = 0;
    for (size_t d = 0; d < padded_dim; d++) {
        float q_norm = (q_prime[d] - vl) * inv_delta;
        int code = (int)floorf(q_norm + 0.5f);
        if (code < 0) code = 0;
        if (code > 15) code = 15;
        qu[d] = (uint8_t)code;
        sum_qu += (uint32_t)code;
    }

    float sqrt_d = sqrtf((float)padded_dim);
    out_q->K1 = 2.0f * delta / sqrt_d;
    out_q->K2 = 2.0f * vl / sqrt_d;
    out_q->K3 = (delta / sqrt_d) * (float)sum_qu + sqrt_d * vl;

    /* Build 4-bit FastScan Look-Up Table:
     * For each 4-bit group j = 0 .. (padded_dim/4 - 1):
     * lut[j][m] = sum_{t=0}^3 bit_t(m) * qu[4*j + t] */
    size_t num_groups = padded_dim / 4u;
    for (size_t j = 0; j < num_groups; j++) {
        uint8_t *group_lut = out_q->lut + j * 16u;
        uint8_t q0 = qu[4u * j + 0u];
        uint8_t q1 = qu[4u * j + 1u];
        uint8_t q2 = qu[4u * j + 2u];
        uint8_t q3 = qu[4u * j + 3u];

        for (uint32_t m = 0; m < 16u; m++) {
            uint32_t sum = 0;
            if (m & 1u) sum += q0;
            if (m & 2u) sum += q1;
            if (m & 4u) sum += q2;
            if (m & 8u) sum += q3;
            group_lut[m] = (uint8_t)sum;
        }
    }

    return NX_OK;
}

/* FastScan dot-product kernel between 1-bit document code and 4-bit query LUT */
static inline uint32_t fastscan_accumulate(const uint8_t *code, const uint8_t *lut, size_t code_bytes) {
    uint32_t S = 0;
    for (size_t b = 0; b < code_bytes; b++) {
        uint8_t byte = code[b];
        uint8_t m0 = byte & 0x0Fu;
        uint8_t m1 = (uint8_t)(byte >> 4u);
        const uint8_t *lut0 = lut + (2u * b + 0u) * 16u;
        const uint8_t *lut1 = lut + (2u * b + 1u) * 16u;
        S += (uint32_t)lut0[m0] + (uint32_t)lut1[m1];
    }
    return S;
}

nx_status nx_rabitq_estimate(const nx_rabitq_index *idx, const nx_rabitq_query *q,
                             uint32_t doc_id, double *out_dist, double *out_lb) {
    if (!idx || !q || doc_id >= idx->node_count || !out_dist) {
        return NX_ERR_INVALID;
    }

    const nx_rabitq_doc *doc = &idx->docs[doc_id];
    const uint8_t *code = idx->codes + (size_t)doc_id * idx->code_bytes;

    /* S = sum_{i=0}^{D'-1} x_b[i] * qu[i] via FastScan */
    uint32_t S = fastscan_accumulate(code, q->lut, idx->code_bytes);

    /* ip_hat = (K1 * S + K2 * pc - K3) * inv_oo */
    float ip_hat = (q->K1 * (float)S + q->K2 * (float)doc->pc - q->K3) * doc->inv_oo;

    if (idx->metric == NX_VEC_L2SQ) {
        double d_hat = (double)doc->no * (double)doc->no +
                       (double)q->nq * (double)q->nq -
                       2.0 * (double)doc->no * (double)q->nq * (double)ip_hat;
        if (d_hat < 0.0) d_hat = 0.0;
        *out_dist = d_hat;

        if (out_lb) {
            double bound_margin = 2.0 * (double)doc->no * (double)q->nq *
                                  (double)idx->eps0 * (double)doc->err_o /
                                  sqrt((double)(idx->padded_dim - 1u));
            double lb = d_hat - bound_margin;
            *out_lb = lb > 0.0 ? lb : 0.0;
        }
    } else {
        /* Cosine / Dot product */
        double dot_hat = (double)doc->no * (double)q->nq * (double)ip_hat +
                         (double)doc->dot_c + (double)q->dot_qc - (double)idx->c_norm_sq;
        if (idx->metric == NX_VEC_COSINE) {
            double denom = sqrt((double)doc->orig_norm_sq * (double)q->q_norm_sq);
            double sim = denom > 1e-12 ? dot_hat / denom : 0.0;
            if (sim > 1.0) sim = 1.0;
            if (sim < -1.0) sim = -1.0;
            *out_dist = sim;
            if (out_lb) {
                double bound_margin = 2.0 * (double)doc->no * (double)q->nq *
                                      (double)idx->eps0 * (double)doc->err_o /
                                      sqrt((double)(idx->padded_dim - 1u));
                double lb = denom > 1e-12 ? (dot_hat - bound_margin) / denom : -1.0;
                *out_lb = lb < -1.0 ? -1.0 : lb;
            }
        } else {
            /* DOT */
            *out_dist = dot_hat;
            if (out_lb) {
                double bound_margin = 2.0 * (double)doc->no * (double)q->nq *
                                      (double)idx->eps0 * (double)doc->err_o /
                                      sqrt((double)(idx->padded_dim - 1u));
                *out_lb = dot_hat - bound_margin;
            }
        }
    }

    return NX_OK;
}

/* Helper insertion sort for top-k buffer */
static void insert_hit(nx_rabitq_hit *hits, size_t *count, size_t k,
                       nx_rabitq_hit item, bool higher_is_better) {
    size_t n = *count;
    if (n < k) {
        /* Not full yet: find insertion spot */
        size_t pos = n;
        while (pos > 0) {
            bool swap = higher_is_better ? (item.dist > hits[pos - 1].dist)
                                         : (item.dist < hits[pos - 1].dist);
            if (swap) {
                hits[pos] = hits[pos - 1];
                pos--;
            } else {
                break;
            }
        }
        hits[pos] = item;
        (*count)++;
    } else {
        /* Full: check if better than worst */
        bool better = higher_is_better ? (item.dist > hits[k - 1].dist)
                                       : (item.dist < hits[k - 1].dist);
        if (!better) return;
        size_t pos = k - 1;
        while (pos > 0) {
            bool swap = higher_is_better ? (item.dist > hits[pos - 1].dist)
                                         : (item.dist < hits[pos - 1].dist);
            if (swap) {
                hits[pos] = hits[pos - 1];
                pos--;
            } else {
                break;
            }
        }
        hits[pos] = item;
    }
}

nx_status nx_rabitq_scan(const nx_rabitq_index *idx, const nx_rabitq_query *q,
                         size_t k, const nx_bitmap *allowed,
                         nx_rabitq_hit *out_hits, size_t *out_count) {
    if (!idx || !q || k == 0 || !out_hits || !out_count) return NX_ERR_INVALID;
    *out_count = 0;

    bool higher_is_better = (idx->metric == NX_VEC_COSINE || idx->metric == NX_VEC_DOT);

    for (uint32_t i = 0; i < idx->node_count; i++) {
        if (allowed && !nx_bitmap_contains(allowed, i)) continue;

        double dist = 0.0, lb = 0.0;
        nx_status st = nx_rabitq_estimate(idx, q, i, &dist, &lb);
        if (st != NX_OK) continue;

        nx_rabitq_hit hit;
        hit.id = i;
        hit.dist = dist;
        hit.lower_bound = lb;

        insert_hit(out_hits, out_count, k, hit, higher_is_better);
    }

    return NX_OK;
}
