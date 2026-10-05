#include "index/nx_graph.h"
#include "core/nx_mem.h"
#include "core/nx_crc32c.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define HEADER_SIZE 48u

typedef struct cand_node {
    uint32_t id;
    double dist;
    bool expanded;
} cand_node;

static inline double compute_distance(const float *a, const float *b, size_t dim, nx_vec_metric metric) {
    double score = 0;
    (void)nx_vec_score(a, b, dim, metric, &score);
    if (metric == NX_VEC_COSINE) {
        /* Cosine similarity: score in [-1, 1], distance in [0, 2] */
        return 1.0 - score;
    } else if (metric == NX_VEC_L2SQ) {
        /* L2 squared distance: score in [0, inf) */
        return score;
    } else {
        /* Dot product: higher is closer -> negative score is distance */
        return -score;
    }
}

static inline double distance_to_score(double dist, nx_vec_metric metric) {
    if (metric == NX_VEC_COSINE) {
        return 1.0 - dist;
    } else if (metric == NX_VEC_L2SQ) {
        return 1.0 / (1.0 + sqrt(fmax(0.0, dist)));
    } else {
        return -dist;
    }
}

static int cand_dist_asc(const void *a, const void *b) {
    double da = ((const cand_node *)a)->dist;
    double db = ((const cand_node *)b)->dist;
    return (da > db) - (da < db);
}

/* Prune candidate list using the RobustPrune algorithm (alpha diversity heuristic) */
static void robust_prune(uint32_t node_id, const float *vectors, size_t dim,
                         nx_vec_metric metric, cand_node *cands, size_t n_cands,
                         float alpha, uint32_t R, uint32_t *out_neighbors, uint32_t *out_count) {
    *out_count = 0;
    if (n_cands == 0) return;

    /* Sort candidates by distance to node_id ascending */
    qsort(cands, n_cands, sizeof(cand_node), cand_dist_asc);

    bool *pruned = (bool *)nx_calloc(n_cands, sizeof(bool));
    if (!pruned) return;

    double alpha2 = (double)(alpha * alpha);

    for (size_t i = 0; i < n_cands && *out_count < R; ++i) {
        if (pruned[i] || cands[i].id == node_id) continue;

        uint32_t p_star = cands[i].id;
        out_neighbors[(*out_count)++] = p_star;
        const float *p_star_vec = vectors + (size_t)p_star * dim;

        /* Suppress candidates obscured by p_star under alpha diversity */
        for (size_t j = i + 1; j < n_cands; ++j) {
            if (pruned[j]) continue;
            const float *cand_vec = vectors + (size_t)cands[j].id * dim;
            double dist_pstar_cand = compute_distance(p_star_vec, cand_vec, dim, metric);
            double dist_p_cand = cands[j].dist;

            if (alpha2 * dist_pstar_cand <= dist_p_cand) {
                pruned[j] = true;
            }
        }
    }

    nx_free(pruned);
}

nx_status nx_graph_build(const float *vectors, size_t node_count, size_t dim,
                         const nx_graph_config *config, nx_buf *out) {
    if (!out) return NX_ERR_INVALID;
    if (node_count > NX_GRAPH_MAX_NODES || dim > NX_GRAPH_MAX_DIM || dim == 0) return NX_ERR_LIMIT;
    if (node_count == 0 || !vectors) return NX_ERR_INVALID;

    nx_graph_config cfg = config ? *config : nx_graph_default_config();
    if (cfg.R == 0 || cfg.R > 128 || cfg.alpha < 1.0f) return NX_ERR_INVALID;
    if (cfg.ef_construction < cfg.R) cfg.ef_construction = cfg.R;

    size_t start_len = out->len;
    bool old_oom = out->oom;

    uint32_t R = cfg.R;
    uint32_t stride = R + 1; /* [count][neighbor_0]...[neighbor_R-1] */
    size_t edge_cells = node_count * stride;

    uint32_t *edge_table = (uint32_t *)nx_calloc(edge_cells, sizeof(uint32_t));
    if (!edge_table) return NX_ERR_NOMEM;

    /* Initial ring connectivity: ensures graph is fully connected before greedy search */
    for (size_t i = 0; i < node_count; ++i) {
        uint32_t *row = edge_table + i * stride;
        if (node_count > 1) {
            row[0] = 1; /* count = 1 */
            row[1] = (uint32_t)((i + 1) % node_count);
        } else {
            row[0] = 0;
        }
    }

    uint32_t entry_node = 0;

    /* Build graph iteratively via beam search & RobustPrune */
    size_t max_pool = cfg.ef_construction + R + 1;
    cand_node *pool = (cand_node *)nx_malloc(max_pool * sizeof(cand_node));
    uint8_t *visited = (uint8_t *)nx_malloc((node_count + 7) / 8);

    if (!pool || !visited) {
        nx_free(edge_table);
        nx_free(pool);
        nx_free(visited);
        return NX_ERR_NOMEM;
    }

    uint32_t *temp_nbrs = (uint32_t *)nx_malloc(R * sizeof(uint32_t));
    if (!temp_nbrs) {
        nx_free(edge_table);
        nx_free(pool);
        nx_free(visited);
        return NX_ERR_NOMEM;
    }

    for (size_t i = 0; i < node_count; ++i) {
        const float *q = vectors + i * dim;
        memset(visited, 0, (node_count + 7) / 8);

        size_t n_pool = 0;
        /* Seed with entry_node */
        visited[entry_node / 8] |= (uint8_t)(1u << (entry_node % 8));
        pool[n_pool].id = entry_node;
        pool[n_pool].dist = compute_distance(q, vectors + (size_t)entry_node * dim, dim, cfg.metric);
        pool[n_pool].expanded = false;
        n_pool++;

        /* Greedy beam search */
        while (1) {
            /* Find nearest unexpanded candidate */
            size_t best_idx = SIZE_MAX;
            double best_dist = 1e30;
            for (size_t p = 0; p < n_pool; ++p) {
                if (!pool[p].expanded && pool[p].dist < best_dist) {
                    best_dist = pool[p].dist;
                    best_idx = p;
                }
            }
            if (best_idx == SIZE_MAX) break;

            pool[best_idx].expanded = true;
            uint32_t curr_id = pool[best_idx].id;
            const uint32_t *nbr_row = edge_table + (size_t)curr_id * stride;
            uint32_t n_nbrs = nbr_row[0];

            for (uint32_t n = 0; n < n_nbrs; ++n) {
                uint32_t nbr_id = nbr_row[1 + n];
                if (nbr_id >= node_count) continue;
                if ((visited[nbr_id / 8] & (1u << (nbr_id % 8))) == 0) {
                    visited[nbr_id / 8] |= (uint8_t)(1u << (nbr_id % 8));
                    if (n_pool < max_pool) {
                        pool[n_pool].id = nbr_id;
                        pool[n_pool].dist = compute_distance(q, vectors + (size_t)nbr_id * dim, dim, cfg.metric);
                        pool[n_pool].expanded = false;
                        n_pool++;
                    }
                }
            }
        }

        /* Prune to R neighbors */
        uint32_t pruned_count = 0;
        robust_prune((uint32_t)i, vectors, dim, cfg.metric, pool, n_pool, cfg.alpha, R, temp_nbrs, &pruned_count);

        uint32_t *my_row = edge_table + i * stride;
        my_row[0] = pruned_count;
        for (uint32_t k = 0; k < pruned_count; ++k) {
            my_row[1 + k] = temp_nbrs[k];
        }

        /* Add bidirectional back-edges */
        for (uint32_t k = 0; k < pruned_count; ++k) {
            uint32_t nbr_id = temp_nbrs[k];
            uint32_t *nbr_row = edge_table + (size_t)nbr_id * stride;
            uint32_t cur_cnt = nbr_row[0];
            bool exists = false;
            for (uint32_t e = 0; e < cur_cnt; ++e) {
                if (nbr_row[1 + e] == (uint32_t)i) { exists = true; break; }
            }
            if (!exists) {
                if (cur_cnt < R) {
                    nbr_row[1 + cur_cnt] = (uint32_t)i;
                    nbr_row[0]++;
                }
            }
        }
    }

    nx_free(pool);
    nx_free(visited);
    nx_free(temp_nbrs);

    /* Serialize to output buffer:
     * 1. 48-byte Header
     * 2. edge_table (node_count * (R + 1) * 4 bytes)
     * 3. vectors (node_count * dim * 4 bytes)
     */
    size_t edge_bytes = edge_cells * sizeof(uint32_t);
    size_t vec_bytes = node_count * dim * sizeof(float);

    uint8_t hdr[HEADER_SIZE];
    memset(hdr, 0, HEADER_SIZE);
    memcpy(hdr, NX_GRAPH_MAGIC, 8);
    uint32_t ver = 1;
    memcpy(hdr + 8, &ver, 4);
    uint32_t nc32 = (uint32_t)node_count;
    memcpy(hdr + 12, &nc32, 4);
    uint32_t dim32 = (uint32_t)dim;
    memcpy(hdr + 16, &dim32, 4);
    memcpy(hdr + 20, &R, 4);
    uint32_t met32 = (uint32_t)cfg.metric;
    memcpy(hdr + 24, &met32, 4);
    memcpy(hdr + 28, &entry_node, 4);

    uint32_t crc = nx_crc32c(0, hdr, 44);
    if (edge_bytes > 0) crc = nx_crc32c(crc, edge_table, edge_bytes);
    if (vec_bytes > 0) crc = nx_crc32c(crc, vectors, vec_bytes);
    memcpy(hdr + 44, &crc, 4);

    nx_buf_put(out, hdr, HEADER_SIZE);
    if (edge_bytes > 0) nx_buf_put(out, edge_table, edge_bytes);
    if (vec_bytes > 0) nx_buf_put(out, vectors, vec_bytes);

    nx_free(edge_table);

    if (out->oom) {
        out->len = start_len;
        out->oom = old_oom;
        return NX_ERR_NOMEM;
    }
    return NX_OK;
}

nx_status nx_graph_open(nx_slice bytes, nx_graph *out) {
    if (!out) return NX_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    if (!bytes.p || bytes.n < HEADER_SIZE) return NX_ERR_CORRUPT;

    if (memcmp(bytes.p, NX_GRAPH_MAGIC, 8) != 0) return NX_ERR_CORRUPT;
    uint32_t ver = 0;
    memcpy(&ver, bytes.p + 8, 4);
    if (ver != 1) return NX_ERR_UNSUPPORTED;

    uint32_t node_count = 0, dim = 0, R = 0, metric = 0, entry_node = 0, stored_crc = 0;
    memcpy(&node_count, bytes.p + 12, 4);
    memcpy(&dim, bytes.p + 16, 4);
    memcpy(&R, bytes.p + 20, 4);
    memcpy(&metric, bytes.p + 24, 4);
    memcpy(&entry_node, bytes.p + 28, 4);
    memcpy(&stored_crc, bytes.p + 44, 4);

    if (node_count > NX_GRAPH_MAX_NODES || dim > NX_GRAPH_MAX_DIM || R == 0 || R > 128) {
        return NX_ERR_CORRUPT;
    }

    size_t edge_bytes = (size_t)node_count * ((size_t)R + 1) * sizeof(uint32_t);
    size_t vec_bytes = (size_t)node_count * (size_t)dim * sizeof(float);
    if (bytes.n < HEADER_SIZE + edge_bytes + vec_bytes) return NX_ERR_CORRUPT;

    /* Verify CRC32C */
    uint32_t crc = nx_crc32c(0, bytes.p, 44);
    crc = nx_crc32c(crc, bytes.p + HEADER_SIZE, bytes.n - HEADER_SIZE);
    if (crc != stored_crc) return NX_ERR_CORRUPT;

    out->bytes = bytes;
    out->node_count = node_count;
    out->dim = dim;
    out->R = R;
    out->metric = (nx_vec_metric)metric;
    out->entry_node = entry_node;
    out->edges = (const uint32_t *)(const void *)(bytes.p + HEADER_SIZE);
    out->vectors = (const float *)(const void *)(bytes.p + HEADER_SIZE + edge_bytes);
    return NX_OK;
}

nx_status nx_graph_search(const nx_graph *graph, const float *query, size_t k, size_t ef,
                          const nx_bitmap *allowed, nx_graph_hit *out_hits, size_t *out_count) {
    if (!graph || !query || !out_hits || !out_count) return NX_ERR_INVALID;
    *out_count = 0;
    if (graph->node_count == 0 || k == 0) return NX_OK;

    if (ef < k) ef = k;
    size_t max_pool = ef + graph->R + 1;

    cand_node *pool = (cand_node *)nx_malloc(max_pool * sizeof(cand_node));
    uint8_t *visited = (uint8_t *)nx_malloc((graph->node_count + 7) / 8);
    if (!pool || !visited) {
        nx_free(pool);
        nx_free(visited);
        return NX_ERR_NOMEM;
    }
    memset(visited, 0, (graph->node_count + 7) / 8);

    size_t n_pool = 0;
    uint32_t start_node = graph->entry_node;
    visited[start_node / 8] |= (uint8_t)(1u << (start_node % 8));

    pool[n_pool].id = start_node;
    pool[n_pool].dist = compute_distance(query, graph->vectors + (size_t)start_node * graph->dim, graph->dim, graph->metric);
    pool[n_pool].expanded = false;
    n_pool++;

    uint32_t stride = graph->R + 1;

    /* Beam search */
    while (1) {
        size_t best_idx = SIZE_MAX;
        double best_dist = 1e30;
        for (size_t p = 0; p < n_pool; ++p) {
            if (!pool[p].expanded && pool[p].dist < best_dist) {
                best_dist = pool[p].dist;
                best_idx = p;
            }
        }
        if (best_idx == SIZE_MAX) break;

        pool[best_idx].expanded = true;
        uint32_t curr_id = pool[best_idx].id;
        const uint32_t *nbr_row = graph->edges + (size_t)curr_id * stride;
        uint32_t n_nbrs = nbr_row[0];

        for (uint32_t n = 0; n < n_nbrs; ++n) {
            uint32_t nbr_id = nbr_row[1 + n];
            if (nbr_id >= graph->node_count) continue;
            if ((visited[nbr_id / 8] & (1u << (nbr_id % 8))) == 0) {
                visited[nbr_id / 8] |= (uint8_t)(1u << (nbr_id % 8));
                if (n_pool < max_pool) {
                    pool[n_pool].id = nbr_id;
                    pool[n_pool].dist = compute_distance(query, graph->vectors + (size_t)nbr_id * graph->dim, graph->dim, graph->metric);
                    pool[n_pool].expanded = false;
                    n_pool++;
                }
            }
        }
    }

    /* Sort pool by distance ascending */
    qsort(pool, n_pool, sizeof(cand_node), cand_dist_asc);

    /* Collect top-k results that pass the allowed filter */
    size_t hits = 0;
    for (size_t i = 0; i < n_pool && hits < k; ++i) {
        uint32_t node_id = pool[i].id;
        if (!allowed || nx_bitmap_contains(allowed, node_id)) {
            out_hits[hits].id = node_id;
            out_hits[hits].score = distance_to_score(pool[i].dist, graph->metric);
            hits++;
        }
    }

    *out_count = hits;
    nx_free(pool);
    nx_free(visited);
    return NX_OK;
}
