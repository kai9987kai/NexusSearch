/* nx_mem.h - allocator wrappers with leak accounting, OOM injection and an
 * optional guarded debug mode.
 *
 * This toolchain has no ASan/UBSan/TSan, so memory safety is verified with:
 *   - ALWAYS-ON counters (nx_mem_get_stats): tests assert live_allocs == 0.
 *   - OOM injection (nx_mem_fail_after): tests iterate n = 0,1,2,... and assert
 *     every failure path returns NX_ERR_NOMEM without leaking or crashing.
 *   - NX_MEM_DEBUG (default ON in Debug/test builds): every block carries a
 *     header + tail canary, freed memory is poisoned, a registry of live blocks
 *     lets nx_mem_check_all() detect overruns and nx_mem_dump_leaks() report
 *     leak sites (file:line).
 *
 * All blocks (aligned or not) are released with nx_free().
 */
#ifndef NX_MEM_H
#define NX_MEM_H

#include "nx_config.h"

typedef struct nx_mem_stats {
    uint64_t live_allocs;
    uint64_t live_bytes;
    uint64_t total_allocs;   /* number of successful allocations since start  */
    uint64_t peak_bytes;
} nx_mem_stats;

NX_API void *nx_malloc_at(size_t n, const char *file, int line);
NX_API void *nx_calloc_at(size_t count, size_t size, const char *file, int line);
NX_API void *nx_realloc_at(void *p, size_t n, const char *file, int line);
NX_API void *nx_aligned_alloc_at(size_t align, size_t n, const char *file, int line);
NX_API void  nx_free(void *p);

#define nx_malloc(n)               nx_malloc_at((n), __FILE__, __LINE__)
#define nx_calloc(c, s)            nx_calloc_at((c), (s), __FILE__, __LINE__)
#define nx_realloc(p, n)           nx_realloc_at((p), (n), __FILE__, __LINE__)
#define nx_aligned_alloc(al, n)    nx_aligned_alloc_at((al), (n), __FILE__, __LINE__)

NX_API char *nx_strdup(const char *s);
NX_API char *nx_strndup(const char *s, size_t n);

/* Allocate n elements of type T (checked multiplication, NULL on overflow/OOM). */
#define NX_NEW_ARRAY(T, n)  ((T *)nx_calloc((n), sizeof(T)))
#define NX_NEW(T)           NX_NEW_ARRAY(T, 1)

/* ---- accounting / testing hooks ---------------------------------------- */
NX_API nx_mem_stats nx_mem_get_stats(void);
/* Fail the (n+1)-th allocation from now (n==0: the very next one). The failure
 * is one-shot; pass -1 to disable. Used to exercise every NOMEM path. */
NX_API void     nx_mem_fail_after(int64_t n);
NX_API uint64_t nx_mem_alloc_counter(void);      /* monotonically increasing  */
/* Debug mode only (no-ops returning 0 otherwise): */
NX_API size_t   nx_mem_check_all(void);          /* returns #corrupt blocks   */
NX_API size_t   nx_mem_dump_leaks(void *file /* FILE* or NULL for stderr */);

/* ---- fatal errors / assertions ----------------------------------------- */
NX_API NX_NORETURN void nx_panic(const char *fmt, ...) NX_PRINTF(1, 2);

#ifndef NDEBUG
#  define NX_ASSERT(c) ((c) ? (void)0 : nx_panic("assertion failed: %s (%s:%d)", #c, __FILE__, __LINE__))
#else
#  define NX_ASSERT(c) ((void)0)
#endif
/* Invariants that must hold even in release builds (cheap checks on data that
 * could be corrupt): returns NX_ERR_CORRUPT from the enclosing function. */
#define NX_UNREACHABLE() nx_panic("unreachable code reached (%s:%d)", __FILE__, __LINE__)

#endif /* NX_MEM_H */
