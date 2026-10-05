/* nx_seg_builder.c - Sealed segment builder and reader.
 *
 * Builds a single self-describing segment file from JSONL, integrating:
 *   - nx_table  (column store with BSI)
 *   - nx_postings per text field (BM25 inverted index)
 *   - nx_trigram per text field  (substring candidate pruning)
 *   - nx_graph per vector field  (ANN proximity graph)
 *
 * Segment format v1 (little-endian):
 *   32B header | N×16B section directory | section data ... | 32B footer
 *
 * The footer CRC32C covers every byte from offset 0 to the footer start,
 * so the entire file is integrity-checked in one pass.
 */
#include "core/nx_config.h"
#include "core/nx_mem.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"
#include "core/nx_crc32c.h"
#include "seg/nx_seg_builder.h"
#include "seg/nx_table.h"
#include "index/nx_postings.h"
#include "index/nx_trigram.h"
#include "index/nx_graph.h"
#include "index/nx_rabitq.h"
#include "index/nx_bitmap.h"
#include <stdio.h>
#include <string.h>

/* ---- constants ---------------------------------------------------------- */
#define SEG_HEADER_SIZE  32u
#define SEC_ENTRY_SIZE   16u
#define SEG_FOOTER_SIZE  32u
#define MAX_SECTIONS     256u   /* practical maximum: 1 table + 3× fields */
#define MAX_FIELDS       128u

/* ---- internal helpers --------------------------------------------------- */

/* Write a 32-byte segment header into buf at current position. */
static void write_seg_header(nx_buf *buf, uint64_t seg_id,
                              uint32_t doc_count, uint32_t sec_count)
{
    uint8_t hdr[SEG_HEADER_SIZE];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, NX_SEG_MAGIC, 8);
    nx_st16(hdr + 8,  (uint16_t)NX_SEG_VERSION);
    nx_st16(hdr + 10, 0u);           /* flags */
    nx_st64(hdr + 12, seg_id);
    nx_st32(hdr + 20, doc_count);
    nx_st32(hdr + 24, sec_count);
    nx_st32(hdr + 28, 0u);           /* reserved */
    uint32_t crc = nx_crc32c(0, hdr, 28);
    nx_st32(hdr + 28, crc);
    nx_buf_put(buf, hdr, sizeof hdr);
}

/* Write one section directory entry. */
static void write_dir_entry(uint8_t *entry, uint32_t section_id,
                             uint16_t field_idx, uint32_t offset, uint32_t size)
{
    memset(entry, 0, SEC_ENTRY_SIZE);
    nx_st32(entry + 0,  section_id);
    nx_st16(entry + 4,  field_idx);
    nx_st16(entry + 6,  0u);   /* flags */
    nx_st32(entry + 8,  offset);
    nx_st32(entry + 12, size);
}

/* ---- tokeniser (simple whitespace/punct splitter) ----------------------- */
static bool is_tok_sep(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == ',' || c == '.' || c == ';' || c == ':' ||
           c == '!' || c == '?' || c == '(' || c == ')' ||
           c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '"' || c == '\'' || c == '/' || c == '\\';
}

/* Case-fold byte for token normalisation. */
static uint8_t casefold(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c;
}

/* Tokenise text into nx_slice array allocated from a flat byte pool.
 * All token bytes are appended to token_pool; slices point into it.
 * Returns number of tokens. max_tokens is a hard cap. */
static size_t tokenise(nx_slice text, nx_buf *token_pool,
                        nx_slice *out_tokens, size_t max_tokens)
{
    size_t ntok = 0;
    const uint8_t *p = text.p;
    const uint8_t *end = p + text.n;

    while (p < end && ntok < max_tokens) {
        /* skip separators */
        while (p < end && is_tok_sep(*p)) p++;
        if (p >= end) break;
        /* find end of token */
        const uint8_t *tok_start = p;
        while (p < end && !is_tok_sep(*p)) p++;
        size_t tok_len = (size_t)(p - tok_start);
        if (tok_len == 0) continue;

        /* case-fold into pool */
        size_t pool_off = token_pool->len;
        for (size_t i = 0; i < tok_len; i++) {
            uint8_t ch = casefold(tok_start[i]);
            nx_buf_put(token_pool, &ch, 1);
        }
        out_tokens[ntok].p = (const uint8_t *)token_pool->data + pool_off;
        out_tokens[ntok].n = tok_len;
        ntok++;
    }
    return ntok;
}

/* ---- default config ----------------------------------------------------- */
nx_seg_build_config nx_seg_build_default_config(void) {
    nx_seg_build_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.max_trigrams_per_doc = 65536;
    return cfg;
}

/* ---- nx_seg_build ------------------------------------------------------- */
nx_status nx_seg_build(nx_slice jsonl, const nx_seg_build_config *config_in,
                        nx_buf *out, nx_error *error)
{
    nx_seg_build_config cfg = nx_seg_build_default_config();
    if (config_in) cfg = *config_in;
    if (cfg.max_trigrams_per_doc == 0) cfg.max_trigrams_per_doc = 65536;

    nx_status st = NX_OK;
    const size_t out_start = out->len;

    /* ---- Step 1: build nx_table ---------------------------------------- */
    nx_table_limits tlim;
    if (cfg.table_limits) {
        tlim = *cfg.table_limits;
    } else {
        tlim = nx_table_default_limits();
    }

    nx_buf table_buf;
    nx_buf_init(&table_buf);
    st = nx_table_build(jsonl, &tlim, &table_buf, error);
    if (st != NX_OK) goto fail;

    /* Open the table to iterate fields and rows */
    nx_table table;
    memset(&table, 0, sizeof table);
    st = nx_table_open(nx_buf_slice(&table_buf), &table, error);
    if (st != NX_OK) { nx_buf_free(&table_buf); goto fail; }

    uint32_t doc_count = table.rows;
    uint32_t field_count = table.fields;

    /* ---- Step 2: identify text and vector fields ----------------------- */
    /* We'll build postings + trigram for each text field,
     * graph for each vector field. */

    /* Collect field types */
    nx_field *fields = NX_NEW_ARRAY(nx_field, field_count);
    if (!fields) { nx_buf_free(&table_buf); st = NX_ERR_NOMEM; goto fail; }
    for (uint32_t fi = 0; fi < field_count; fi++) {
        nx_table_field(&table, fi, &fields[fi]);
    }

    /* ---- Step 3: build postings + trigram per text field --------------- */
    /* We need arrays of docs per text field. */
    /* Use dynamic arrays of nx_buf per field. */

    /* Count text and vector fields */
    uint32_t n_text = 0, n_vec = 0;
    for (uint32_t fi = 0; fi < field_count; fi++) {
        if (fields[fi].type == NX_FIELD_TEXT) n_text++;
        if (fields[fi].type == NX_FIELD_VECTOR) n_vec++;
    }

    /* Postings / trigram buffers per text field (field_count slots, unused stay empty) */
    nx_buf *post_bufs = NX_NEW_ARRAY(nx_buf, field_count);
    nx_buf *trig_bufs = NX_NEW_ARRAY(nx_buf, field_count);
    /* Graph and RaBitQ buffers per vector field */
    nx_buf *graph_bufs = NX_NEW_ARRAY(nx_buf, field_count);
    nx_buf *rabitq_bufs = NX_NEW_ARRAY(nx_buf, field_count);
    if (!post_bufs || !trig_bufs || !graph_bufs || !rabitq_bufs) {
        nx_free(fields);
        nx_free(post_bufs); nx_free(trig_bufs); nx_free(graph_bufs); nx_free(rabitq_bufs);
        nx_buf_free(&table_buf);
        st = NX_ERR_NOMEM;
        goto fail;
    }
    for (uint32_t fi = 0; fi < field_count; fi++) {
        nx_buf_init(&post_bufs[fi]);
        nx_buf_init(&trig_bufs[fi]);
        nx_buf_init(&graph_bufs[fi]);
        nx_buf_init(&rabitq_bufs[fi]);
    }

    /* ---- Build postings + trigram for each text field ------------------- */
    for (uint32_t fi = 0; fi < field_count && st == NX_OK; fi++) {
        if (fields[fi].type != NX_FIELD_TEXT) continue;
        if (doc_count == 0) {
            /* Empty table — skip */
            continue;
        }

        /* Build doc arrays for postings */
        nx_postings_doc *pdocs = NX_NEW_ARRAY(nx_postings_doc, doc_count);
        nx_slice *tdocs = NX_NEW_ARRAY(nx_slice, doc_count);  /* for trigram */
        if (!pdocs || !tdocs) {
            nx_free(pdocs); nx_free(tdocs);
            st = NX_ERR_NOMEM; break;
        }

        /* Per-doc token storage: pool and per-doc token arrays */
        nx_buf token_pool;
        nx_buf_init(&token_pool);

        /* Max tokens across all docs in one field: generous cap */
        const size_t MAX_TOKS_PER_DOC = 65536;
        nx_slice *tok_scratch = NX_NEW_ARRAY(nx_slice, MAX_TOKS_PER_DOC);
        if (!tok_scratch) {
            nx_free(pdocs); nx_free(tdocs);
            nx_buf_free(&token_pool);
            st = NX_ERR_NOMEM; break;
        }

        /* We need to keep all tokens alive for postings_build.
         * Store per-doc token arrays in one big flat array. */
        /* First pass: count total tokens */
        size_t total_toks = 0;
        for (uint32_t di = 0; di < doc_count; di++) {
            nx_cell cell;
            if (nx_table_get(&table, di, fi, &cell) == NX_OK && cell.present) {
                total_toks += cell.as.text.n / 3 + 4; /* rough upper bound */
            }
        }
        if (total_toks < doc_count * 4) total_toks = doc_count * 4;
        if (total_toks > doc_count * MAX_TOKS_PER_DOC) total_toks = doc_count * MAX_TOKS_PER_DOC;

        nx_slice *all_tokens = NX_NEW_ARRAY(nx_slice, total_toks + 1);
        if (!all_tokens) {
            nx_free(pdocs); nx_free(tdocs); nx_free(tok_scratch);
            nx_buf_free(&token_pool);
            st = NX_ERR_NOMEM; break;
        }

        size_t tok_cursor = 0;
        bool overflow = false;

        for (uint32_t di = 0; di < doc_count && !overflow; di++) {
            nx_cell cell;
            nx_slice text = { (const uint8_t *)"", 0 };
            if (nx_table_get(&table, di, fi, &cell) == NX_OK && cell.present) {
                text = cell.as.text;
            }
            tdocs[di] = text;  /* raw text for trigram */

            size_t pool_before = token_pool.len;
            size_t ntok = tokenise(text, &token_pool, tok_scratch, MAX_TOKS_PER_DOC);

            if (tok_cursor + ntok > total_toks) {
                /* Realloc not practical here — truncate gracefully */
                ntok = total_toks - tok_cursor;
                overflow = true;
            }
            /* Copy slices to stable array (pool pointers are stable
             * only as long as token_pool doesn't grow — we computed
             * an upper bound so we need to fixup after any growth).
             * Simpler: copy slices as offsets from pool base, fixup later.
             * Use a trick: store offset in .p temporarily as (uint8_t*)offset. */
            for (size_t ti = 0; ti < ntok; ti++) {
                size_t off = (size_t)(tok_scratch[ti].p - (const uint8_t *)token_pool.data);
                /* Store offset; fixup .p after pool is finalised */
                all_tokens[tok_cursor + ti].p = (const uint8_t *)off;  /* offset, not pointer */
                all_tokens[tok_cursor + ti].n = tok_scratch[ti].n;
            }
            pdocs[di].tokens = all_tokens + tok_cursor;
            pdocs[di].token_count = ntok;
            tok_cursor += ntok;
            (void)pool_before;
        }

        /* Fixup all token pointers now that pool is stable */
        const uint8_t *pool_base = (const uint8_t *)token_pool.data;
        for (size_t ti = 0; ti < tok_cursor; ti++) {
            size_t off = (size_t)all_tokens[ti].p;
            all_tokens[ti].p = pool_base + off;
        }

        /* Build postings */
        st = nx_postings_build(pdocs, doc_count, &post_bufs[fi]);

        /* Build trigram (only if postings succeeded) */
        if (st == NX_OK) {
            st = nx_trigram_build(tdocs, doc_count, &trig_bufs[fi]);
        }

        nx_free(all_tokens);
        nx_free(tok_scratch);
        nx_buf_free(&token_pool);
        nx_free(pdocs);
        nx_free(tdocs);
    }

    /* ---- Build graph for each vector field ------------------------------ */
    for (uint32_t fi = 0; fi < field_count && st == NX_OK; fi++) {
        if (fields[fi].type != NX_FIELD_VECTOR) continue;
        if (doc_count == 0) continue;

        uint32_t dims = fields[fi].dims;
        if (dims == 0) continue;

        float *vectors = NX_NEW_ARRAY(float, (size_t)doc_count * dims);
        if (!vectors) { st = NX_ERR_NOMEM; break; }

        uint32_t vec_docs = 0;
        for (uint32_t di = 0; di < doc_count; di++) {
            nx_cell cell;
            memset(vectors + (size_t)di * dims, 0, (size_t)dims * sizeof(float));
            if (nx_table_get(&table, di, fi, &cell) == NX_OK && cell.present
                && cell.as.vector.dims == dims) {
                for (uint32_t d = 0; d < dims; d++) {
                    vectors[(size_t)di * dims + d] = nx_cell_vector_at(&cell, d);
                }
                vec_docs++;
            }
        }

        if (vec_docs >= 2) {
            /* Need at least 2 vectors to build a meaningful graph */
            nx_graph_config gcfg;
            if (cfg.graph_config) {
                gcfg = *(const nx_graph_config *)cfg.graph_config;
            } else {
                gcfg = nx_graph_default_config();
            }
            st = nx_graph_build(vectors, doc_count, dims, &gcfg, &graph_bufs[fi]);
            if (st == NX_OK) {
                nx_rabitq_config rcfg = nx_rabitq_default_config(dims, NX_VEC_L2SQ);
                st = nx_rabitq_build(vectors, doc_count, dims, &rcfg, &rabitq_bufs[fi]);
            }
        }
        nx_free(vectors);
    }

    if (st != NX_OK) goto cleanup_indices;

    /* ---- Step 4: count sections and write segment ----------------------- */
    /* Count sections: always 1 table; plus 1 postings + 1 trigram per text
     * field that has data; plus 1 graph and 1 rabitq per vector field that has data. */
    uint32_t sec_count = 1; /* table */
    for (uint32_t fi = 0; fi < field_count; fi++) {
        if (post_bufs[fi].len > 0)   sec_count++;
        if (trig_bufs[fi].len > 0)   sec_count++;
        if (graph_bufs[fi].len > 0)  sec_count++;
        if (rabitq_bufs[fi].len > 0) sec_count++;
    }

    /* Assemble section entries and data into a staging buffer */
    nx_buf dir_buf;   /* section directory */
    nx_buf data_buf;  /* section payloads */
    nx_buf_init(&dir_buf);
    nx_buf_init(&data_buf);

    /* Helper lambda-like macro for appending a section */
#define EMIT_SECTION(sec_id, fld_idx, section_buf)  do { \
    if ((section_buf).len > 0) { \
        uint8_t entry[SEC_ENTRY_SIZE]; \
        write_dir_entry(entry, (sec_id), (fld_idx), \
                        (uint32_t)data_buf.len, (uint32_t)(section_buf).len); \
        nx_buf_put(&dir_buf, entry, SEC_ENTRY_SIZE); \
        nx_buf_put(&data_buf, (section_buf).data, (section_buf).len); \
    } \
} while (0)

    /* Table section */
    {
        uint8_t entry[SEC_ENTRY_SIZE];
        write_dir_entry(entry, NX_SEC_TABLE, NX_SEC_NO_FIELD,
                        (uint32_t)data_buf.len, (uint32_t)table_buf.len);
        nx_buf_put(&dir_buf, entry, SEC_ENTRY_SIZE);
        nx_buf_put(&data_buf, table_buf.data, table_buf.len);
    }

    /* Per-field sections */
    for (uint32_t fi = 0; fi < field_count; fi++) {
        EMIT_SECTION(NX_SEC_POSTINGS, (uint16_t)fi, post_bufs[fi]);
        EMIT_SECTION(NX_SEC_TRIGRAM,  (uint16_t)fi, trig_bufs[fi]);
        EMIT_SECTION(NX_SEC_GRAPH,    (uint16_t)fi, graph_bufs[fi]);
        EMIT_SECTION(NX_SEC_RABITQ,   (uint16_t)fi, rabitq_bufs[fi]);
    }
#undef EMIT_SECTION

    /* ---- Step 5: write out the complete segment ------------------------- */
    /* Layout: [header][directory][section data][footer] */
    write_seg_header(out, cfg.seg_id, doc_count, sec_count);
    nx_buf_put(out, dir_buf.data, dir_buf.len);
    nx_buf_put(out, data_buf.data, data_buf.len);

    /* Footer: CRC of everything so far */
    size_t content_size = out->len - out_start;
    uint32_t file_crc = nx_crc32c(0,
        (const uint8_t *)out->data + out_start, content_size);

    uint8_t footer[SEG_FOOTER_SIZE];
    memset(footer, 0, sizeof footer);
    memcpy(footer, NX_SEGF_MAGIC, 8);
    nx_st64(footer + 8,  (uint64_t)(content_size + SEG_FOOTER_SIZE));
    nx_st64(footer + 16, 0u);  /* reserved */
    nx_st32(footer + 24, file_crc);
    nx_st32(footer + 28, 0u);  /* reserved */
    nx_buf_put(out, footer, sizeof footer);

    /* Check OOM accumulated during all nx_buf_put calls */
    if (out->data == NULL && out->len > 0) {
        st = NX_ERR_NOMEM;
    }

    nx_buf_free(&dir_buf);
    nx_buf_free(&data_buf);

cleanup_indices:
    for (uint32_t fi = 0; fi < field_count; fi++) {
        nx_buf_free(&post_bufs[fi]);
        nx_buf_free(&trig_bufs[fi]);
        nx_buf_free(&graph_bufs[fi]);
        nx_buf_free(&rabitq_bufs[fi]);
    }
    nx_free(post_bufs);
    nx_free(trig_bufs);
    nx_free(graph_bufs);
    nx_free(rabitq_bufs);
    nx_free(fields);
    nx_buf_free(&table_buf);

    if (st != NX_OK) {
        out->len = out_start;  /* transactional rollback */
        if (error && error->code == NX_OK) {
            nx_error_set(error, st, -1, 0, "seg_build failed: %s", nx_status_str(st));
        }
    }
    return st;

fail:
    out->len = out_start;
    return st;

    /* suppress unused-label warning if goto fail is reachable from cleanup */
    (void)n_text; (void)n_vec;
}

/* ---- nx_segment_open ---------------------------------------------------- */
nx_status nx_segment_open(nx_slice bytes, nx_segment *out, nx_error *error)
{
    memset(out, 0, sizeof *out);

    /* Minimum size check */
    if (bytes.n < SEG_HEADER_SIZE + SEG_FOOTER_SIZE) {
        if (error) nx_error_set(error, NX_ERR_CORRUPT, 0, 0, "segment too small");
        return NX_ERR_CORRUPT;
    }

    const uint8_t *p = bytes.p;

    /* Validate segment header magic */
    if (memcmp(p, NX_SEG_MAGIC, 8) != 0) {
        if (error) nx_error_set(error, NX_ERR_CORRUPT, 0, 0, "bad segment magic");
        return NX_ERR_CORRUPT;
    }
    uint16_t ver = nx_ld16(p + 8);
    if (ver != NX_SEG_VERSION) {
        if (error) nx_error_set(error, NX_ERR_VERSION, 0, 0,
                                "segment version %u unsupported", (unsigned)ver);
        return NX_ERR_VERSION;
    }

    /* Validate header CRC */
    uint32_t hdr_crc_stored = nx_ld32(p + 28);
    uint32_t hdr_crc_calc   = nx_crc32c(0, p, 28);
    if (hdr_crc_stored != hdr_crc_calc) {
        if (error) nx_error_set(error, NX_ERR_CORRUPT, 0, 0, "segment header CRC mismatch");
        return NX_ERR_CORRUPT;
    }

    uint64_t seg_id    = nx_ld64(p + 12);
    uint32_t doc_count = nx_ld32(p + 20);
    uint32_t sec_count = nx_ld32(p + 24);

    /* Validate footer (last SEG_FOOTER_SIZE bytes) */
    const uint8_t *footer = bytes.p + bytes.n - SEG_FOOTER_SIZE;
    if (memcmp(footer, NX_SEGF_MAGIC, 8) != 0) {
        if (error) nx_error_set(error, NX_ERR_CORRUPT, 0, 0, "bad segment footer magic");
        return NX_ERR_CORRUPT;
    }
    uint64_t stored_file_size = nx_ld64(footer + 8);
    if (stored_file_size != bytes.n) {
        if (error) nx_error_set(error, NX_ERR_CORRUPT, 0, 0,
                                "segment size mismatch: stored %llu actual %zu",
                                (unsigned long long)stored_file_size, bytes.n);
        return NX_ERR_CORRUPT;
    }
    uint32_t stored_crc = nx_ld32(footer + 24);
    size_t content_size = bytes.n - SEG_FOOTER_SIZE;
    uint32_t calc_crc   = nx_crc32c(0, bytes.p, content_size);
    if (stored_crc != calc_crc) {
        if (error) nx_error_set(error, NX_ERR_CORRUPT, 0, 0, "segment file CRC mismatch");
        return NX_ERR_CORRUPT;
    }

    /* Validate section directory fits */
    size_t dir_size = (size_t)sec_count * SEC_ENTRY_SIZE;
    size_t data_start = SEG_HEADER_SIZE + dir_size;
    if (data_start > content_size) {
        if (error) nx_error_set(error, NX_ERR_CORRUPT, 0, 0,
                                "section directory overflows segment");
        return NX_ERR_CORRUPT;
    }

    const uint8_t *dir_ptr  = bytes.p + SEG_HEADER_SIZE;
    const uint8_t *data_ptr = bytes.p + data_start;
    size_t data_size = content_size - data_start;

    /* Parse and bounds-check directory entries */
    for (uint32_t i = 0; i < sec_count; i++) {
        const uint8_t *e = dir_ptr + (size_t)i * SEC_ENTRY_SIZE;
        uint32_t off  = nx_ld32(e + 8);
        uint32_t size = nx_ld32(e + 12);
        size_t end_off;
        if (nx_add_overflow((size_t)off, (size_t)size, &end_off) || end_off > data_size) {
            if (error) nx_error_set(error, NX_ERR_CORRUPT, 0, 0,
                                    "section %u out of bounds", i);
            return NX_ERR_CORRUPT;
        }
    }

    /* Populate output struct */
    out->bytes     = bytes;
    out->seg_id    = seg_id;
    out->doc_count = doc_count;
    out->sec_count = sec_count;
    out->dir       = (const nx_sec_entry *)dir_ptr;
    out->sec_data  = data_ptr;

    /* Find table and tombstone sections for convenience */
    for (uint32_t i = 0; i < sec_count; i++) {
        const uint8_t *e  = dir_ptr + (size_t)i * SEC_ENTRY_SIZE;
        uint32_t sid  = nx_ld32(e + 0);
        uint32_t off  = nx_ld32(e + 8);
        uint32_t size = nx_ld32(e + 12);
        nx_slice s = nx_slice_make(data_ptr + off, size);
        if (sid == NX_SEC_TABLE)     out->table_bytes     = s;
        if (sid == NX_SEC_TOMBSTONE) out->tombstone_bytes = s;
    }

    return NX_OK;
}

/* ---- nx_segment_section ------------------------------------------------- */
nx_status nx_segment_section(const nx_segment *seg, uint32_t section_id,
                               uint16_t field_idx, nx_slice *out_slice)
{
    const uint8_t *dir = (const uint8_t *)seg->dir;
    for (uint32_t i = 0; i < seg->sec_count; i++) {
        const uint8_t *e  = dir + (size_t)i * SEC_ENTRY_SIZE;
        uint32_t sid  = nx_ld32(e + 0);
        uint16_t fidx = nx_ld16(e + 4);
        uint32_t off  = nx_ld32(e + 8);
        uint32_t sz   = nx_ld32(e + 12);
        if (sid == section_id &&
            (field_idx == NX_SEC_NO_FIELD || fidx == field_idx)) {
            *out_slice = nx_slice_make(seg->sec_data + off, sz);
            return NX_OK;
        }
    }
    return NX_ERR_NOT_FOUND;
}

/* ---- nx_segment_section_count ------------------------------------------ */
uint32_t nx_segment_section_count(const nx_segment *seg, uint32_t section_id)
{
    uint32_t count = 0;
    const uint8_t *dir = (const uint8_t *)seg->dir;
    for (uint32_t i = 0; i < seg->sec_count; i++) {
        const uint8_t *e = dir + (size_t)i * SEC_ENTRY_SIZE;
        if (nx_ld32(e + 0) == section_id) count++;
    }
    return count;
}
