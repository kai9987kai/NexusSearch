#include "index/nx_trigram.h"
#include "core/nx_mem.h"
#include "core/nx_crc32c.h"
#include <string.h>
#include <stdlib.h>

#define HEADER_SIZE 32u

typedef struct trigram_pair {
    uint32_t trigram;
    uint32_t doc_id;
} trigram_pair;

static int uint32_cmp(const void *a, const void *b) {
    uint32_t u1 = *(const uint32_t *)a, u2 = *(const uint32_t *)b;
    return (u1 > u2) - (u1 < u2);
}

static int pair_cmp(const void *a, const void *b) {
    const trigram_pair *p1 = (const trigram_pair *)a;
    const trigram_pair *p2 = (const trigram_pair *)b;
    if (p1->trigram != p2->trigram) {
        return (p1->trigram > p2->trigram) ? 1 : -1;
    }
    return (p1->doc_id > p2->doc_id) - (p1->doc_id < p2->doc_id);
}

nx_status nx_trigram_extract(nx_slice text, uint32_t *out_trigrams, size_t max_trigrams, size_t *out_count) {
    if (!out_count) return NX_ERR_INVALID;
    *out_count = 0;
    if (!text.p || text.n < 3) return NX_OK;
    if (!out_trigrams || max_trigrams == 0) return NX_ERR_INVALID;

    size_t raw_count = text.n - 2;
    if (raw_count > max_trigrams) raw_count = max_trigrams;

    for (size_t i = 0; i < raw_count; ++i) {
        out_trigrams[i] = nx_trigram_pack(text.p[i], text.p[i + 1], text.p[i + 2]);
    }

    qsort(out_trigrams, raw_count, sizeof(uint32_t), uint32_cmp);

    /* Deduplicate in-place */
    size_t unique = 0;
    for (size_t i = 0; i < raw_count; ++i) {
        if (i == 0 || out_trigrams[i] != out_trigrams[i - 1]) {
            out_trigrams[unique++] = out_trigrams[i];
        }
    }
    *out_count = unique;
    return NX_OK;
}

nx_status nx_trigram_build(const nx_slice *docs, size_t doc_count, nx_buf *out) {
    if (!out) return NX_ERR_INVALID;
    if (doc_count > NX_TRIGRAM_MAX_DOCS) return NX_ERR_LIMIT;
    if (doc_count == 0 || !docs) return NX_ERR_INVALID;

    size_t start_len = out->len;
    bool old_oom = out->oom;

    /* Count total trigram pairs across all documents */
    size_t total_estimated_pairs = 0;
    for (size_t i = 0; i < doc_count; ++i) {
        if (docs[i].p && docs[i].n >= 3) {
            total_estimated_pairs += (docs[i].n - 2);
        }
    }

    trigram_pair *pairs = NULL;
    if (total_estimated_pairs > 0) {
        pairs = (trigram_pair *)nx_malloc(total_estimated_pairs * sizeof(trigram_pair));
        if (!pairs) return NX_ERR_NOMEM;
    }

    size_t total_pairs = 0;
    for (size_t d = 0; d < doc_count; ++d) {
        if (!docs[d].p || docs[d].n < 3) continue;
        size_t n = docs[d].n - 2;
        uint32_t *doc_trigrams = (uint32_t *)nx_malloc(n * sizeof(uint32_t));
        if (!doc_trigrams) {
            nx_free(pairs);
            return NX_ERR_NOMEM;
        }
        size_t unique_in_doc = 0;
        (void)nx_trigram_extract(docs[d], doc_trigrams, n, &unique_in_doc);
        for (size_t i = 0; i < unique_in_doc; ++i) {
            if (total_pairs < total_estimated_pairs) {
                pairs[total_pairs].trigram = doc_trigrams[i];
                pairs[total_pairs].doc_id = (uint32_t)d;
                total_pairs++;
            }
        }
        nx_free(doc_trigrams);
    }

    if (total_pairs > 0) {
        qsort(pairs, total_pairs, sizeof(trigram_pair), pair_cmp);
    }

    /* Count unique trigrams */
    size_t unique_trigrams = 0;
    for (size_t i = 0; i < total_pairs; ++i) {
        if (i == 0 || pairs[i].trigram != pairs[i - 1].trigram) {
            unique_trigrams++;
        }
    }

    if (unique_trigrams > NX_TRIGRAM_MAX_COUNT) {
        nx_free(pairs);
        return NX_ERR_LIMIT;
    }

    uint32_t *trig_keys = NULL;
    uint32_t *offsets = NULL;
    if (unique_trigrams > 0) {
        trig_keys = (uint32_t *)nx_malloc(unique_trigrams * sizeof(uint32_t));
        offsets = (uint32_t *)nx_malloc((unique_trigrams + 1) * sizeof(uint32_t));
        if (!trig_keys || !offsets) {
            nx_free(pairs);
            nx_free(trig_keys);
            nx_free(offsets);
            return NX_ERR_NOMEM;
        }
    }

    nx_buf postings;
    nx_buf_init(&postings);

    size_t cur_trig_idx = 0;
    size_t pair_start = 0;
    nx_status st = NX_OK;

    while (pair_start < total_pairs) {
        size_t pair_end = pair_start;
        uint32_t current_trig = pairs[pair_start].trigram;
        while (pair_end < total_pairs && pairs[pair_end].trigram == current_trig) {
            pair_end++;
        }

        size_t num_docs_with_trig = pair_end - pair_start;
        uint32_t *doc_ids = (uint32_t *)nx_malloc(num_docs_with_trig * sizeof(uint32_t));
        if (!doc_ids) {
            st = NX_ERR_NOMEM;
            break;
        }
        for (size_t j = 0; j < num_docs_with_trig; ++j) {
            doc_ids[j] = pairs[pair_start + j].doc_id;
        }

        offsets[cur_trig_idx] = (uint32_t)postings.len;
        trig_keys[cur_trig_idx] = current_trig;
        cur_trig_idx++;

        st = nx_bitmap_build(doc_ids, num_docs_with_trig, &postings);
        nx_free(doc_ids);
        if (st != NX_OK) break;

        pair_start = pair_end;
    }

    if (st == NX_OK && unique_trigrams > 0) {
        offsets[unique_trigrams] = (uint32_t)postings.len;
    }

    nx_free(pairs);

    if (st != NX_OK || postings.oom) {
        nx_buf_free(&postings);
        nx_free(trig_keys);
        nx_free(offsets);
        out->len = start_len;
        out->oom = old_oom;
        return st != NX_OK ? st : NX_ERR_NOMEM;
    }

    /* Serialize to out buffer:
     * 1. 32-byte Header
     * 2. trig_keys (unique_trigrams * 4 bytes)
     * 3. offsets ((unique_trigrams + 1) * 4 bytes)
     * 4. postings bytes
     */
    size_t payload_size = unique_trigrams * sizeof(uint32_t) +
                          (unique_trigrams + 1) * sizeof(uint32_t) +
                          postings.len;

    uint8_t hdr[HEADER_SIZE];
    memset(hdr, 0, HEADER_SIZE);
    memcpy(hdr, NX_TRIGRAM_MAGIC, 8);
    hdr[8] = 1; hdr[9] = 0; hdr[10] = 0; hdr[11] = 0; /* version 1 */
    uint32_t ut32 = (uint32_t)unique_trigrams;
    uint32_t dc32 = (uint32_t)doc_count;
    memcpy(hdr + 12, &ut32, 4);
    memcpy(hdr + 16, &dc32, 4);
    uint32_t hs32 = HEADER_SIZE;
    memcpy(hdr + 20, &hs32, 4);
    uint32_t res0 = 0;
    memcpy(hdr + 24, &res0, 4);

    /* Compute CRC32C over header[0:28] and all payload sections */
    uint32_t crc = nx_crc32c(0, hdr, 28);
    if (unique_trigrams > 0) {
        crc = nx_crc32c(crc, trig_keys, unique_trigrams * sizeof(uint32_t));
        crc = nx_crc32c(crc, offsets, (unique_trigrams + 1) * sizeof(uint32_t));
        if (postings.len > 0) {
            crc = nx_crc32c(crc, postings.data, postings.len);
        }
    }
    memcpy(hdr + 28, &crc, 4);

    /* Write everything to out */
    nx_buf_put(out, hdr, HEADER_SIZE);
    if (unique_trigrams > 0) {
        nx_buf_put(out, trig_keys, unique_trigrams * sizeof(uint32_t));
        nx_buf_put(out, offsets, (unique_trigrams + 1) * sizeof(uint32_t));
        if (postings.len > 0) {
            nx_buf_put(out, postings.data, postings.len);
        }
    }

    nx_buf_free(&postings);
    nx_free(trig_keys);
    nx_free(offsets);

    if (out->oom) {
        out->len = start_len;
        out->oom = old_oom;
        return NX_ERR_NOMEM;
    }
    (void)payload_size;
    return NX_OK;
}

nx_status nx_trigram_open(nx_slice bytes, nx_trigram_index *out) {
    if (!out) return NX_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    if (!bytes.p || bytes.n < HEADER_SIZE) return NX_ERR_CORRUPT;

    if (memcmp(bytes.p, NX_TRIGRAM_MAGIC, 8) != 0) return NX_ERR_CORRUPT;
    uint32_t ver = (uint32_t)bytes.p[8] | ((uint32_t)bytes.p[9] << 8) |
                   ((uint32_t)bytes.p[10] << 16) | ((uint32_t)bytes.p[11] << 24);
    if (ver != 1) return NX_ERR_UNSUPPORTED;

    uint32_t trigram_count = 0, doc_count = 0, header_size = 0, stored_crc = 0;
    memcpy(&trigram_count, bytes.p + 12, 4);
    memcpy(&doc_count, bytes.p + 16, 4);
    memcpy(&header_size, bytes.p + 20, 4);
    memcpy(&stored_crc, bytes.p + 28, 4);

    if (header_size != HEADER_SIZE || trigram_count > NX_TRIGRAM_MAX_COUNT || doc_count > NX_TRIGRAM_MAX_DOCS) {
        return NX_ERR_CORRUPT;
    }

    size_t trig_bytes = (size_t)trigram_count * sizeof(uint32_t);
    /* When trigram_count==0, builder writes no offset sentinel, so off_bytes=0. */
    size_t off_bytes = trigram_count > 0u ? ((size_t)trigram_count + 1u) * sizeof(uint32_t) : 0u;
    if (bytes.n < HEADER_SIZE + trig_bytes + off_bytes) return NX_ERR_CORRUPT;

    /* Verify CRC32C */
    uint32_t crc = nx_crc32c(0, bytes.p, 28);
    crc = nx_crc32c(crc, bytes.p + HEADER_SIZE, bytes.n - HEADER_SIZE);
    if (crc != stored_crc) return NX_ERR_CORRUPT;

    const uint32_t *trigs = (const uint32_t *)(const void *)(bytes.p + HEADER_SIZE);
    const uint32_t *offs = (const uint32_t *)(const void *)(bytes.p + HEADER_SIZE + trig_bytes);
    size_t postings_base = HEADER_SIZE + trig_bytes + off_bytes;

    /* Verify trigram ordering is strictly sorted */
    for (size_t i = 1; i < trigram_count; ++i) {
        if (trigs[i] <= trigs[i - 1]) return NX_ERR_CORRUPT;
    }

    /* Verify offsets are non-decreasing and bounded */
    if (trigram_count > 0) {
        if (offs[0] != 0) return NX_ERR_CORRUPT;
        size_t total_postings = bytes.n - postings_base;
        if (offs[trigram_count] != total_postings) return NX_ERR_CORRUPT;
        for (size_t i = 0; i < trigram_count; ++i) {
            if (offs[i] > offs[i + 1]) return NX_ERR_CORRUPT;
        }
    }

    out->bytes = bytes;
    out->trigram_count = trigram_count;
    out->doc_count = doc_count;
    out->trigrams = trigs;
    out->offsets = offs;
    out->postings_base = postings_base;
    return NX_OK;
}

nx_status nx_trigram_lookup(const nx_trigram_index *index, uint32_t trigram, nx_bitmap *out_bitmap) {
    if (!index || !out_bitmap) return NX_ERR_INVALID;
    memset(out_bitmap, 0, sizeof(*out_bitmap));
    if (index->trigram_count == 0) return NX_ERR_NOT_FOUND;

    size_t left = 0, right = index->trigram_count;
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (index->trigrams[mid] == trigram) {
            uint32_t start_off = index->offsets[mid];
            uint32_t end_off = index->offsets[mid + 1];
            size_t len = (size_t)(end_off - start_off);
            nx_slice bmp_slice = nx_slice_make(index->bytes.p + index->postings_base + start_off, len);
            return nx_bitmap_open(bmp_slice, out_bitmap);
        }
        if (index->trigrams[mid] < trigram) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return NX_ERR_NOT_FOUND;
}

nx_status nx_trigram_query_substring(const nx_trigram_index *index, nx_slice needle,
                                     nx_buf *out_bitmap_buf, bool *has_candidates) {
    if (!index || !out_bitmap_buf || !has_candidates) return NX_ERR_INVALID;
    *has_candidates = false;

    if (!needle.p || needle.n < 3) {
        /* Substring too short to extract trigrams; caller must use scan fallback */
        return NX_OK;
    }

    size_t n_trigrams = needle.n - 2;
    uint32_t *needle_trigrams = (uint32_t *)nx_malloc(n_trigrams * sizeof(uint32_t));
    if (!needle_trigrams) return NX_ERR_NOMEM;

    size_t unique_count = 0;
    nx_status st = nx_trigram_extract(needle, needle_trigrams, n_trigrams, &unique_count);
    if (st != NX_OK) {
        nx_free(needle_trigrams);
        return st;
    }

    if (unique_count == 0) {
        nx_free(needle_trigrams);
        return NX_OK;
    }

    /* Look up the first trigram */
    nx_bitmap current_bm;
    st = nx_trigram_lookup(index, needle_trigrams[0], &current_bm);
    if (st == NX_ERR_NOT_FOUND) {
        /* Trigram is absent from the entire corpus => 0 documents can match */
        nx_free(needle_trigrams);
        *has_candidates = true;
        uint32_t empty_ids[1] = {0};
        return nx_bitmap_build(empty_ids, 0, out_bitmap_buf);
    }
    if (st != NX_OK) {
        nx_free(needle_trigrams);
        return st;
    }

    nx_buf cur_buf;
    nx_buf_init(&cur_buf);
    nx_buf_put(&cur_buf, current_bm.bytes.p, current_bm.bytes.n);

    /* Intersect with all remaining trigrams */
    for (size_t i = 1; i < unique_count; ++i) {
        nx_bitmap next_bm;
        st = nx_trigram_lookup(index, needle_trigrams[i], &next_bm);
        if (st == NX_ERR_NOT_FOUND) {
            /* Any absent trigram means 0 candidates */
            nx_free(needle_trigrams);
            nx_buf_free(&cur_buf);
            *has_candidates = true;
            uint32_t empty_ids[1] = {0};
            return nx_bitmap_build(empty_ids, 0, out_bitmap_buf);
        }
        if (st != NX_OK) {
            nx_free(needle_trigrams);
            nx_buf_free(&cur_buf);
            return st;
        }

        nx_bitmap cur_open;
        st = nx_bitmap_open(nx_buf_slice(&cur_buf), &cur_open);
        if (st != NX_OK) {
            nx_free(needle_trigrams);
            nx_buf_free(&cur_buf);
            return st;
        }

        nx_buf combined;
        nx_buf_init(&combined);
        st = nx_bitmap_combine(&cur_open, &next_bm, NX_BITMAP_AND, &combined);
        nx_buf_free(&cur_buf);
        if (st != NX_OK) {
            nx_free(needle_trigrams);
            nx_buf_free(&combined);
            return st;
        }
        cur_buf = combined;
    }

    nx_free(needle_trigrams);
    *has_candidates = true;
    nx_buf_put(out_bitmap_buf, cur_buf.data, cur_buf.len);
    nx_buf_free(&cur_buf);
    return NX_OK;
}
