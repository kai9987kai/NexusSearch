/* Write-Ahead Log (WAL) for durable, crash-resilient mutation logging.
 *
 * Implements an append-only journal supporting transactional document UPSERTs
 * and DELETEs. Every frame is protected by a 32-bit CRC32C checksum.
 * During recovery replay, torn writes and partial frames at EOF are safely detected
 * and ignored, guaranteeing atomicity and consistency across sudden crashes.
 *
 * Binary Layout:
 * - 32-byte WAL Header: magic "NXWAL1\0\0", version=1, flags, seq, CRC32C.
 * - Sequence of Frames:
 *   - 16-byte Frame Header:
 *     magic 0x57414C46 ("WALF"), type (1=UPSERT, 2=DELETE, 3=CHECKPOINT),
 *     flags (1B), reserved (2B), payload_len (4B), CRC32C (4B).
 *   - Payload: payload_len bytes (doc JSON or _id string).
 */
#ifndef NX_WAL_H
#define NX_WAL_H

#include "core/nx_config.h"
#include "core/nx_status.h"
#include "core/nx_buf.h"
#include <stdio.h>

#define NX_WAL_MAGIC "NXWAL1\0\0"
#define NX_WAL_FRAME_MAGIC (0x57414C46u) /* "WALF" */

typedef enum nx_wal_op {
    NX_WAL_OP_UPSERT = 1,
    NX_WAL_OP_DELETE = 2,
    NX_WAL_OP_CHECKPOINT = 3
} nx_wal_op;

typedef struct nx_wal {
    char path[1024];
    FILE *fp;
    uint64_t seq;
    size_t file_size;
} nx_wal;

typedef nx_status (*nx_wal_replay_fn)(nx_wal_op op, nx_slice payload, void *user_data);

/* Open or create a WAL file at the given path.
 * If file does not exist, writes WAL header and initializes.
 * If file exists, validates header and positions at EOF for appending. */
NX_API nx_status nx_wal_open(const char *path, nx_wal *out);

/* Closes the WAL file handle. Harmless if called on closed/NULL. */
NX_API void nx_wal_close(nx_wal *wal);

/* Append an UPSERT mutation (full document JSONL string).
 * Writes frame header, payload, and CRC32C. */
NX_API nx_status nx_wal_append_upsert(nx_wal *wal, nx_slice doc_json);

/* Append a DELETE mutation (_id string). */
NX_API nx_status nx_wal_append_delete(nx_wal *wal, nx_slice id);

/* Append a CHECKPOINT marker with sequence number. */
NX_API nx_status nx_wal_append_checkpoint(nx_wal *wal, uint64_t seq);

/* Force flush and fsync buffered WAL frames to physical storage. */
NX_API nx_status nx_wal_sync(nx_wal *wal);

/* Replay all valid frames from the beginning of the WAL.
 * Calls fn for each valid frame until EOF.
 * Automatically stops cleanly on partial / torn frame at EOF. */
NX_API nx_status nx_wal_replay(const char *path, nx_wal_replay_fn fn, void *user_data, size_t *out_records);

/* Truncate / reset the WAL file to empty state after a segment flush. */
NX_API nx_status nx_wal_truncate(nx_wal *wal);

#endif
