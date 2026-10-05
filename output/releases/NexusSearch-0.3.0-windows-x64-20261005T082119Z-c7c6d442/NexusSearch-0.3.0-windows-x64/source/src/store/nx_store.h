/* nx_store.h - Multi-Segment Persistent Store with WAL and Manifest.
 *
 * The store is the top-level persistence layer for a NexusSearch index directory.
 * It manages:
 *
 *   - A manifest file tracking all live segment files (atomic, generation-numbered)
 *   - A Write-Ahead Log (nx_wal) for in-progress mutations (durable before ack)
 *   - A mutable document accumulation buffer that becomes a new segment on flush
 *   - Atomic segment publication: write seg → fsync → write manifest → rename
 *
 * Directory layout:
 *   <dir>/manifest-<gen>.json  — live manifest (highest valid gen wins)
 *   <dir>/seg-<id>.nxs         — sealed immutable segment files
 *   <dir>/wal.log              — active Write-Ahead Log
 *   <dir>/LOCK                 — exclusive lock file (advisory)
 *
 * Manifest JSON format:
 *   {
 *     "gen": <uint64>,
 *     "segments": [{"id": <uint64>, "file": "<name>", "docs": <uint32>}, ...],
 *     "next_seg_id": <uint64>
 *   }
 *
 * Crash recovery protocol:
 *   1. Find highest valid manifest-<gen>.json.
 *   2. Open all segments listed in manifest.
 *   3. Replay wal.log from the beginning, applying upserts / deletes.
 *   4. The replayed WAL mutations form the in-memory mutable buffer.
 *
 * Thread safety: NOT thread-safe. External synchronisation required for
 * concurrent access. (Engine layer adds its write_mu above this.)
 */
#ifndef NX_STORE_H
#define NX_STORE_H

#include "core/nx_config.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"
#include "store/nx_wal.h"
#include "seg/nx_seg_builder.h"

#define NX_STORE_MAX_SEGMENTS 256u
#define NX_STORE_MAX_PATH     1024u

/* ---- live segment slot -------------------------------------------------- */
typedef struct nx_store_seg_entry {
    uint64_t    seg_id;
    char        file[NX_STORE_MAX_PATH];  /* absolute or relative path */
    uint32_t    doc_count;
    nx_segment  seg;          /* opened segment reader */
    bool        open;         /* true if seg is live */
} nx_store_seg_entry;

/* ---- store handle ------------------------------------------------------- */
typedef struct nx_store {
    char     dir[NX_STORE_MAX_PATH];   /* store directory path */
    uint64_t manifest_gen;             /* current manifest generation */
    uint64_t next_seg_id;              /* monotonically increasing */

    /* Live sealed segments */
    nx_store_seg_entry segs[NX_STORE_MAX_SEGMENTS];
    uint32_t           seg_count;

    /* Mutable accumulation buffer (JSONL; lines accumulated since last flush) */
    nx_buf   mutable_buf;   /* accumulates doc JSON lines */
    uint32_t mutable_docs;  /* number of documents in mutable_buf */

    /* Active WAL */
    nx_wal   wal;
    bool     wal_open;

    /* Memory-mapped segment bytes per slot (owned; freed on close) */
    uint8_t *seg_mem[NX_STORE_MAX_SEGMENTS];  /* heap-allocated segment bytes */
    size_t   seg_mem_size[NX_STORE_MAX_SEGMENTS];
} nx_store;

/* ---- API ---------------------------------------------------------------- */

/* Open (or create) a store at the given directory path.
 * Creates the directory if it does not exist.
 * On success, *out is ready to use. Performs crash recovery (WAL replay). */
NX_API nx_status nx_store_open(const char *dir, nx_store *out, nx_error *error);

/* Close the store, flushing the WAL. Does NOT automatically flush the
 * mutable buffer to a sealed segment. */
NX_API void nx_store_close(nx_store *store);

/* Append one document (JSON object as a NUL-terminated string) to the
 * mutable buffer and journal it to the WAL. */
NX_API nx_status nx_store_upsert(nx_store *store, const char *doc_json, nx_error *error);

/* Mark a document deleted by _id. The deletion is WAL-journaled. */
NX_API nx_status nx_store_delete(nx_store *store, const char *id, nx_error *error);

/* Flush: seal the mutable buffer into a new segment file, checkpoint the
 * WAL, and atomically update the manifest.
 * After flush, the mutable buffer is empty and the WAL is truncated.
 * No-op if mutable_docs == 0. */
NX_API nx_status nx_store_flush(nx_store *store, nx_error *error);

/* Return the total number of documents across all sealed segments plus the
 * mutable buffer (approximate; does not account for deletes/overwrites). */
NX_API uint64_t nx_store_doc_count(const nx_store *store);

/* Return the number of sealed segments. */
NX_API uint32_t nx_store_seg_count(const nx_store *store);

/* Access a sealed segment by slot index [0, seg_count). */
NX_API const nx_segment *nx_store_seg(const nx_store *store, uint32_t slot);

#endif
