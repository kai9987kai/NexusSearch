#include "index/nx_postings.h"
#include "core/nx_mem.h"
#include "core/nx_crc32c.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define HEADER_SIZE 40u

nx_status nx_postings_build(const nx_postings_doc *docs, size_t doc_count, nx_buf *out) {
    if (!out) return NX_ERR_INVALID;
    if (doc_count > NX_POSTINGS_MAX_DOCS) return NX_ERR_LIMIT;
    if (doc_count == 0 || !docs) return NX_ERR_INVALID;

    size_t start_len = out->len;
    bool old_oom = out->oom;

    /* 1. Calculate document lengths and field statistics */
    uint32_t *doc_lengths = (uint32_t *)nx_calloc(doc_count, sizeof(uint32_t));
    if (!doc_lengths) return NX_ERR_NOMEM;

    uint64_t total_doc_len = 0;
    uint32_t field_doc_count = 0;
    size_t total_token_refs = 0;

    for (size_t d = 0; d < doc_count; ++d) {
        doc_lengths[d] = (uint32_t)docs[d].token_count;
        total_doc_len += docs[d].token_count;
        if (docs[d].token_count > 0) {
            field_doc_count++;
            total_token_refs += docs[d].token_count;
        }
    }

    float avgdl = field_doc_count > 0 ? (float)((double)total_doc_len / (double)field_doc_count) : 0.0f;

    /* 2. Collect all token slices to build the dictionary */
    nx_slice *all_tokens = NULL;
    if (total_token_refs > 0) {
        all_tokens = (nx_slice *)nx_malloc(total_token_refs * sizeof(nx_slice));
        if (!all_tokens) {
            nx_free(doc_lengths);
            return NX_ERR_NOMEM;
        }
    }

    size_t tok_idx = 0;
    for (size_t d = 0; d < doc_count; ++d) {
        for (size_t t = 0; t < docs[d].token_count; ++t) {
            all_tokens[tok_idx++] = docs[d].tokens[t];
        }
    }

    nx_buf dict_buf;
    nx_buf_init(&dict_buf);
    nx_status st = nx_dict_build(all_tokens, total_token_refs, &dict_buf);
    nx_free(all_tokens);
    if (st != NX_OK) {
        nx_free(doc_lengths);
        nx_buf_free(&dict_buf);
        return st;
    }

    nx_dict dict;
    st = nx_dict_open(nx_buf_slice(&dict_buf), &dict);
    if (st != NX_OK) {
        nx_free(doc_lengths);
        nx_buf_free(&dict_buf);
        return st;
    }

    uint32_t term_count = dict.count;
    if (term_count > NX_POSTINGS_MAX_TERMS) {
        nx_free(doc_lengths);
        nx_buf_free(&dict_buf);
        return NX_ERR_LIMIT;
    }

    uint32_t *term_dfs = NULL;
    uint32_t *offsets = NULL;
    if (term_count > 0) {
        term_dfs = (uint32_t *)nx_calloc(term_count, sizeof(uint32_t));
        offsets = (uint32_t *)nx_calloc(term_count + 1, sizeof(uint32_t));
        if (!term_dfs || !offsets) {
            nx_free(doc_lengths);
            nx_free(term_dfs);
            nx_free(offsets);
            nx_buf_free(&dict_buf);
            return NX_ERR_NOMEM;
        }
    }

    /* 3. Build postings lists for each term */
    nx_buf postings_buf;
    nx_buf_init(&postings_buf);

    for (uint32_t term_id = 0; term_id < term_count; ++term_id) {
        nx_slice term_slice;
        (void)nx_dict_term(&dict, term_id, &term_slice);

        /* Find all documents containing this term and compute tf */
        uint32_t *matching_docs = (uint32_t *)nx_malloc(doc_count * sizeof(uint32_t));
        uint16_t *matching_tfs = (uint16_t *)nx_malloc(doc_count * sizeof(uint16_t));
        if (!matching_docs || !matching_tfs) {
            nx_free(matching_docs);
            nx_free(matching_tfs);
            st = NX_ERR_NOMEM;
            break;
        }

        uint32_t df = 0;
        for (size_t d = 0; d < doc_count; ++d) {
            uint16_t tf = 0;
            for (size_t t = 0; t < docs[d].token_count; ++t) {
                if (nx_slice_eq(docs[d].tokens[t], term_slice)) {
                    if (tf < UINT16_MAX) tf++;
                }
            }
            if (tf > 0) {
                matching_docs[df] = (uint32_t)d;
                matching_tfs[df] = tf;
                df++;
            }
        }

        term_dfs[term_id] = df;
        offsets[term_id] = (uint32_t)postings_buf.len;

        /* Write Roaring bitmap of doc IDs */
        nx_buf bm_buf;
        nx_buf_init(&bm_buf);
        st = nx_bitmap_build(matching_docs, df, &bm_buf);
        if (st == NX_OK) {
            /* Prefix with 4-byte bitmap length so reader can separate bitmap from tf array */
            uint32_t bm_len = (uint32_t)bm_buf.len;
            nx_buf_put(&postings_buf, &bm_len, 4);
            nx_buf_put(&postings_buf, bm_buf.data, bm_buf.len);
            /* Append tf array (df * 2 bytes) */
            if (df > 0) {
                nx_buf_put(&postings_buf, matching_tfs, df * sizeof(uint16_t));
            }
        }
        nx_buf_free(&bm_buf);
        nx_free(matching_docs);
        nx_free(matching_tfs);

        if (st != NX_OK) break;
    }

    if (st == NX_OK && term_count > 0) {
        offsets[term_count] = (uint32_t)postings_buf.len;
    }

    if (st != NX_OK || postings_buf.oom) {
        nx_free(doc_lengths);
        nx_free(term_dfs);
        nx_free(offsets);
        nx_buf_free(&dict_buf);
        nx_buf_free(&postings_buf);
        out->len = start_len;
        out->oom = old_oom;
        return st != NX_OK ? st : NX_ERR_NOMEM;
    }

    /* 4. Serialize to output buffer:
     * Header (40 bytes):
     *  0:8   magic "NXPOST1\0"
     *  8:12  version = 1
     *  12:16 term_count
     *  16:20 doc_count
     *  20:24 field_doc_count
     *  24:32 total_doc_len (uint64)
     *  32:36 avgdl (float32)
     *  36:40 dict_len (uint32)
     * Followed by CRC at 40..44? Let's use 48-byte header:
     *  40:44 reserved = 0
     *  44:48 CRC32C covering [0:44] and all subsequent payload!
     */
    uint8_t hdr[48];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, NX_POSTINGS_MAGIC, 8);
    uint32_t ver = 1;
    memcpy(hdr + 8, &ver, 4);
    memcpy(hdr + 12, &term_count, 4);
    uint32_t dc32 = (uint32_t)doc_count;
    memcpy(hdr + 16, &dc32, 4);
    memcpy(hdr + 20, &field_doc_count, 4);
    memcpy(hdr + 24, &total_doc_len, 8);
    memcpy(hdr + 32, &avgdl, 4);
    uint32_t dict_len = (uint32_t)dict_buf.len;
    memcpy(hdr + 36, &dict_len, 4);

    uint32_t crc = nx_crc32c(0, hdr, 44);
    if (dict_buf.len > 0) crc = nx_crc32c(crc, dict_buf.data, dict_buf.len);
    if (doc_count > 0) crc = nx_crc32c(crc, doc_lengths, doc_count * sizeof(uint32_t));
    if (term_count > 0) {
        crc = nx_crc32c(crc, term_dfs, term_count * sizeof(uint32_t));
        crc = nx_crc32c(crc, offsets, (term_count + 1) * sizeof(uint32_t));
        if (postings_buf.len > 0) {
            crc = nx_crc32c(crc, postings_buf.data, postings_buf.len);
        }
    }
    memcpy(hdr + 44, &crc, 4);

    /* Write everything to out */
    nx_buf_put(out, hdr, sizeof(hdr));
    if (dict_buf.len > 0) nx_buf_put(out, dict_buf.data, dict_buf.len);
    if (doc_count > 0) nx_buf_put(out, doc_lengths, doc_count * sizeof(uint32_t));
    if (term_count > 0) {
        nx_buf_put(out, term_dfs, term_count * sizeof(uint32_t));
        nx_buf_put(out, offsets, (term_count + 1) * sizeof(uint32_t));
        if (postings_buf.len > 0) {
            nx_buf_put(out, postings_buf.data, postings_buf.len);
        }
    }

    nx_free(doc_lengths);
    nx_free(term_dfs);
    nx_free(offsets);
    nx_buf_free(&dict_buf);
    nx_buf_free(&postings_buf);

    if (out->oom) {
        out->len = start_len;
        out->oom = old_oom;
        return NX_ERR_NOMEM;
    }
    return NX_OK;
}

nx_status nx_postings_open(nx_slice bytes, nx_postings_index *out) {
    if (!out) return NX_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    if (!bytes.p || bytes.n < 48) return NX_ERR_CORRUPT;

    if (memcmp(bytes.p, NX_POSTINGS_MAGIC, 8) != 0) return NX_ERR_CORRUPT;
    uint32_t ver = 0;
    memcpy(&ver, bytes.p + 8, 4);
    if (ver != 1) return NX_ERR_UNSUPPORTED;

    uint32_t term_count = 0, doc_count = 0, field_doc_count = 0, dict_len = 0, stored_crc = 0;
    uint64_t total_doc_len = 0;
    float avgdl = 0.0f;

    memcpy(&term_count, bytes.p + 12, 4);
    memcpy(&doc_count, bytes.p + 16, 4);
    memcpy(&field_doc_count, bytes.p + 20, 4);
    memcpy(&total_doc_len, bytes.p + 24, 8);
    memcpy(&avgdl, bytes.p + 32, 4);
    memcpy(&dict_len, bytes.p + 36, 4);
    memcpy(&stored_crc, bytes.p + 44, 4);

    if (term_count > NX_POSTINGS_MAX_TERMS || doc_count > NX_POSTINGS_MAX_DOCS) {
        return NX_ERR_CORRUPT;
    }

    size_t doc_len_bytes = (size_t)doc_count * sizeof(uint32_t);
    size_t term_dfs_bytes = (size_t)term_count * sizeof(uint32_t);
    size_t offsets_bytes = ((size_t)term_count + 1) * sizeof(uint32_t);

    size_t min_size = 48 + dict_len + doc_len_bytes + term_dfs_bytes + offsets_bytes;
    if (bytes.n < min_size) return NX_ERR_CORRUPT;

    /* Verify CRC32C */
    uint32_t crc = nx_crc32c(0, bytes.p, 44);
    crc = nx_crc32c(crc, bytes.p + 48, bytes.n - 48);
    if (crc != stored_crc) return NX_ERR_CORRUPT;

    /* Open embedded dictionary */
    nx_slice dict_slice = nx_slice_make(bytes.p + 48, dict_len);
    nx_status st = nx_dict_open(dict_slice, &out->dict);
    if (st != NX_OK) return st;

    const uint8_t *cur = bytes.p + 48 + dict_len;
    out->doc_lengths = (const uint32_t *)(const void *)cur; cur += doc_len_bytes;
    out->term_dfs = (const uint32_t *)(const void *)cur; cur += term_dfs_bytes;
    out->offsets = (const uint32_t *)(const void *)cur; cur += offsets_bytes;
    out->postings_base = (size_t)(cur - bytes.p);

    if (term_count > 0) {
        if (out->offsets[0] != 0) return NX_ERR_CORRUPT;
        size_t total_postings = bytes.n - out->postings_base;
        if (out->offsets[term_count] != total_postings) return NX_ERR_CORRUPT;
        for (size_t i = 0; i < term_count; ++i) {
            if (out->offsets[i] > out->offsets[i + 1]) return NX_ERR_CORRUPT;
        }
    }

    out->bytes = bytes;
    out->term_count = term_count;
    out->doc_count = doc_count;
    out->field_doc_count = field_doc_count;
    out->total_doc_len = total_doc_len;
    out->avgdl = avgdl;
    return NX_OK;
}

nx_status nx_postings_lookup(const nx_postings_index *index, nx_slice term,
                             uint32_t *out_df, nx_bitmap *out_doc_bitmap) {
    if (!index || !out_df || !out_doc_bitmap) return NX_ERR_INVALID;
    memset(out_doc_bitmap, 0, sizeof(*out_doc_bitmap));
    *out_df = 0;

    uint32_t ord = UINT32_MAX;
    nx_status st = nx_dict_lookup(&index->dict, term, &ord);
    if (st != NX_OK || ord >= index->term_count) return NX_ERR_NOT_FOUND;

    *out_df = index->term_dfs[ord];

    uint32_t start_off = index->offsets[ord];
    uint32_t end_off = index->offsets[ord + 1];
    if (end_off <= start_off + 4) return NX_ERR_NOT_FOUND;

    const uint8_t *post_ptr = index->bytes.p + index->postings_base + start_off;
    uint32_t bm_len = 0;
    memcpy(&bm_len, post_ptr, 4);
    if (start_off + 4 + bm_len > end_off) return NX_ERR_CORRUPT;

    nx_slice bm_slice = nx_slice_make(post_ptr + 4, bm_len);
    return nx_bitmap_open(bm_slice, out_doc_bitmap);
}

double nx_postings_bm25_score(const nx_postings_index *index, nx_slice term,
                              uint32_t doc_id, double k1, double b) {
    if (!index || doc_id >= index->doc_count || k1 < 0.0 || b < 0.0 || b > 1.0) return 0.0;

    uint32_t ord = UINT32_MAX;
    nx_status st = nx_dict_lookup(&index->dict, term, &ord);
    if (st != NX_OK || ord >= index->term_count) return 0.0;

    uint32_t start_off = index->offsets[ord];
    uint32_t end_off = index->offsets[ord + 1];
    if (end_off <= start_off + 4) return 0.0;

    const uint8_t *post_ptr = index->bytes.p + index->postings_base + start_off;
    uint32_t bm_len = 0;
    memcpy(&bm_len, post_ptr, 4);
    if (start_off + 4 + bm_len > end_off) return 0.0;

    nx_bitmap bm;
    st = nx_bitmap_open(nx_slice_make(post_ptr + 4, bm_len), &bm);
    if (st != NX_OK || !nx_bitmap_contains(&bm, doc_id)) return 0.0;

    /* Determine the rank of doc_id in this term's posting list to get tf */
    uint32_t df = index->term_dfs[ord];
    const uint16_t *tfs = (const uint16_t *)(const void *)(post_ptr + 4 + bm_len);
    if (start_off + 4 + bm_len + (size_t)df * sizeof(uint16_t) > end_off) return 0.0;

    nx_bitmap_iter it;
    nx_bitmap_iter_init(&bm, &it);
    uint32_t cur_id = 0;
    uint32_t rank = 0;
    uint16_t tf = 1;
    while (nx_bitmap_next(&it, &cur_id)) {
        if (cur_id == doc_id) {
            tf = (rank < df) ? tfs[rank] : 1;
            break;
        }
        rank++;
    }

    double N = (double)index->field_doc_count;
    double df_d = (double)df;
    double dl = (double)index->doc_lengths[doc_id];
    double avgdl = index->avgdl > 0.0f ? (double)index->avgdl : 1.0;

    /* Lucene 10.3.1 BM25 formula:
     * idf = ln(1 + (N - df + 0.5) / (df + 0.5))
     * score = idf * tf / (tf + k1 * (1 - b + b * dl / avgdl))
     */
    double idf = log(1.0 + (N - df_d + 0.5) / (df_d + 0.5));
    double denom = (double)tf + k1 * (1.0 - b + b * (dl / avgdl));
    if (denom <= 0.0) return 0.0;
    return idf * ((double)tf / denom);
}
