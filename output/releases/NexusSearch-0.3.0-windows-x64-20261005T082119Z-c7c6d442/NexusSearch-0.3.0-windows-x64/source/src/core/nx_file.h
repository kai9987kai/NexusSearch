/* nx_file.h - bounded whole-file reads and immutable read-only mappings.
 * Paths are borrowed NUL-terminated UTF-8 (native byte paths on POSIX).
 * No process-global state. Separate objects are safe to use concurrently.
 * A mapped file must never be truncated/modified in place while mapped;
 * nx_file_write_atomic replaces its name and preserves existing mappings.
 */
#ifndef NX_FILE_H
#define NX_FILE_H
#include "core/nx_config.h"
#include "core/nx_status.h"

typedef struct nx_mmap {
    const uint8_t *data; /* borrowed from mapping; valid until nx_mmap_close */
    size_t size;
    void *file_handle;  /* private platform handles; do not edit */
    void *map_handle;
} nx_mmap;

/* out must not hold a live mapping; cleared on failure. Empty files succeed
 * with NULL data and size=0. max_bytes is an inclusive cap, not a hint. */
NX_API nx_status nx_mmap_open(const char *path, size_t max_bytes, nx_mmap *out);
/* Releases mapping ownership; NULL and repeated close are harmless. */
NX_API void nx_mmap_close(nx_mmap *map);
/* On success *data is owned (nx_free); no terminator is appended. Empty files
 * return NULL/0. Outputs are cleared on failure. May not alias each other. */
NX_API nx_status nx_file_read(const char *path, size_t max_bytes, uint8_t **data, size_t *size);
/* Atomically replaces path through an exclusive temporary in its directory.
 * data is borrowed, NULL only for size=0. Flushes content before rename and
 * on POSIX fsyncs the parent directory after rename. Windows uses flushed file
 * content and FileRenameInfoEx with POSIX replacement (Windows 10+); older
 * Windows/filesystems lacking this operation return IO. Power-loss metadata guarantees depend
 * on filesystem/OS. An IO error after rename can mean replacement occurred.
 * The containing directory must be trusted; this is not a sandbox boundary. */
NX_API nx_status nx_file_write_atomic(const char *path, const void *data, size_t size);
/* Removes path only (never recursive); missing path returns NOT_FOUND. */
NX_API nx_status nx_file_remove(const char *path);
#endif
