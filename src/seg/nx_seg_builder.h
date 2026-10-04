/* Sealed Segment Builder — produces a single self-describing segment file that
 * combines every index structure built over a set of JSONL documents:
 *
 *   - Column store   (nx_table)        — typed cell access, BSI for numerics
 *   - Inverted index (nx_postings)     — BM25 full-text per text field
 *   - Trigram index  (nx_trigram)      — substring/regex candidate pruning per text field
 *   - Proximity graph (nx_graph)       — ANN vector search per vector field
 *
 * Segment file format v1 (all values little-endian):
 *
 *   [32B segment header]
 *   [section directory: N × 16B entries]
 *   [section data: packed section blobs]
 *   [32B footer with CRC32C of entire file except footer CRC field]
 *
 * Segment header (32 bytes):
 *   magic[8]   "NXSEG1\0"
 *   version    uint16_t = 1
 *   flags      uint16_t = 0
 *   seg_id     uint64_t (assigned by store; 0 during build)
 *   doc_count  uint32_t
 *   sec_count  uint32_t
 *   reserved   uint32_t = 0
 *   header_crc uint32_t (CRC32C of bytes [0:28])
 *
 * Section directory entry (16 bytes each):
 *   section_id uint32_t   (NX_SEC_* constant below)
 *   field_idx  uint16_t   (0xFFFF = not field-specific)
 *   flags      uint16_t   = 0
 *   offset     uint32_t   (from start of section data region)
 *   size       uint32_t
 *
 * Section IDs:
 *   NX_SEC_TABLE    = 1   — nx_table blob (column store + BSI + doc store)
 *   NX_SEC_POSTINGS = 2   — nx_postings blob for field_idx
 *   NX_SEC_TRIGRAM  = 3   — nx_trigram blob for field_idx
 *   NX_SEC_GRAPH    = 4   — nx_graph blob for field_idx
 *   NX_SEC_TOMBSTONE= 5   — nx_bitmap of deleted doc ordinals (copy-on-write)
 *
 * Footer (32 bytes):
 *   magic[8]   "NXSEGF\0"
 *   file_size  uint64_t
 *   reserved   uint64_t = 0
 *   file_crc   uint32_t (CRC32C of all bytes from offset 0 to footer offset)
 *   reserved2  uint32_t = 0
 */
#ifndef NX_SEG_BUILDER_H
#define NX_SEG_BUILDER_H

#include "core/nx_config.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"
#include "seg/nx_table.h"

/* ---- section IDs -------------------------------------------------------- */
#define NX_SEC_TABLE     (1u)
#define NX_SEC_POSTINGS  (2u)
#define NX_SEC_TRIGRAM   (3u)
#define NX_SEC_GRAPH     (4u)
#define NX_SEC_TOMBSTONE (5u)
#define NX_SEC_RABITQ    (6u)

#define NX_SEG_MAGIC   "NXSEG1\0"
#define NX_SEGF_MAGIC  "NXSEGF\0"
#define NX_SEG_VERSION (1u)
#define NX_SEC_NO_FIELD (0xFFFFu)

/* ---- section directory entry (reader-side view) ------------------------- */
typedef struct nx_sec_entry {
    uint32_t section_id;
    uint16_t field_idx;
    uint16_t flags;
    uint32_t offset;   /* relative to start of section data region */
    uint32_t size;
} nx_sec_entry;

/* ---- immutable segment reader ------------------------------------------- */
typedef struct nx_segment {
    nx_slice  bytes;          /* borrowed from mmap or heap; valid until close */
    uint64_t  seg_id;
    uint32_t  doc_count;
    uint32_t  sec_count;
    /* Convenience direct pointers (NULL if section absent) */
    nx_slice  table_bytes;    /* NX_SEC_TABLE section */
    nx_slice  tombstone_bytes;/* NX_SEC_TOMBSTONE section (may be absent) */
    /* Full section directory for per-field index lookup */
    const nx_sec_entry *dir;  /* sec_count entries, points into bytes */
    const uint8_t *sec_data;  /* start of section data region */
} nx_segment;

/* ---- build config ------------------------------------------------------- */
typedef struct nx_seg_build_config {
    /* Table size limits (NULL = defaults) */
    const nx_table_limits *table_limits;
    /* Graph config per vector field; NULL = default nx_graph_config */
    /* (Single config applied to all vector fields in this build) */
    const void *graph_config; /* typed as nx_graph_config* to avoid header cycle */
    /* Maximum trigrams per document (default: 65536) */
    size_t max_trigrams_per_doc;
    /* Assigned segment ID (0 = unassigned; store fills this in) */
    uint64_t seg_id;
} nx_seg_build_config;

NX_API nx_seg_build_config nx_seg_build_default_config(void);

/* Build a sealed segment from JSONL input.
 *
 * jsonl:   newline-delimited JSON objects, one per document.
 *          Each must have a unique "_id" field.
 * config:  build parameters (NULL = defaults).
 * out:     caller-owned nx_buf; receives serialized segment bytes.
 * error:   receives human-readable error on failure (may be NULL).
 *
 * On success, out contains a complete, CRC-validated segment blob that can
 * be passed to nx_segment_open() or written atomically to disk.
 * On failure, out is left at its original length (transactional). */
NX_API nx_status nx_seg_build(nx_slice jsonl, const nx_seg_build_config *config,
                               nx_buf *out, nx_error *error);

/* Open an immutable segment from serialized bytes (zero allocation).
 * Validates segment header CRC, footer CRC, and section directory bounds.
 * out->bytes is borrowed from the caller; caller must keep bytes alive. */
NX_API nx_status nx_segment_open(nx_slice bytes, nx_segment *out, nx_error *error);

/* Lookup a section by ID and optional field index (NX_SEC_NO_FIELD = any).
 * Returns NX_OK and fills *out_slice on success; NX_ERR_NOT_FOUND if absent. */
NX_API nx_status nx_segment_section(const nx_segment *seg, uint32_t section_id,
                                     uint16_t field_idx, nx_slice *out_slice);

/* Return number of sections with the given ID (e.g. one POSTINGS per field). */
NX_API uint32_t nx_segment_section_count(const nx_segment *seg, uint32_t section_id);

#endif
