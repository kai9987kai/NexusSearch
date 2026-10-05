/* Bounded copy-on-write transactions for an existing nx_table snapshot.
 * path is borrowed, NUL-terminated UTF-8; mutations/error are borrowed.
 * Each nonblank JSONL line is {"op":"upsert","document":{...}} or
 * {"op":"delete","id":"..."}. Unknown envelope members are rejected.
 * Every operation and the final inferred schema are validated before publish.
 * Last operation per decoded _id wins; deleting an absent ID is a no-op.
 * Existing IDs retain row order, new IDs follow first appearance order.
 * Empty input validates the existing snapshot without rewriting it.
 * Limits: 64 MiB mutation input and final JSONL, 100k operations/final rows;
 * other limits match nx_table defaults. Rebuild requires whole-corpus memory.
 * A nonblocking OS lock on path + ".lock" serializes cooperating writers;
 * contention returns BUSY. The sidecar persists, holds no PID/state, and MUST
 * NOT be removed/renamed while any updater might run. Lock ownership vanishes
 * on process exit. All writers must use this API and the same path spelling
 * (no aliases/hardlinks); external build/replace operations do not cooperate.
 * Existing readers retain their old immutable mapping; reopen for new data.
 * Publication uses nx_file_write_atomic. NOMEM/validation errors preserve the
 * previous file. IO during publication can mean the replacement occurred:
 * inspect/reopen before reporting failure; replaying the same batch is safe.
 * Requires a trusted local directory/filesystem with working locks/replace.
 * No multi-file transactions or universal power-loss durability guarantee.
 */
#ifndef NX_UPDATE_H
#define NX_UPDATE_H
#include "core/nx_config.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"
NX_API nx_status nx_update_snapshot(const char *path, nx_slice mutations_jsonl, nx_error *error);
#endif
