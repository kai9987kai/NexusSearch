/* nx_arena.h - bump allocator for short-lived, same-lifetime objects (query
 * ASTs, plans, parse trees, per-query scratch). Never frees individual blocks.
 * Not thread-safe: one arena per thread / per query. */
#ifndef NX_ARENA_H
#define NX_ARENA_H

#include "nx_config.h"

typedef struct nx_arena_chunk nx_arena_chunk;

typedef struct nx_arena {
    nx_arena_chunk *head;       /* most-recent chunk first */
    size_t chunk_size;          /* default payload size of a new chunk */
    size_t total;               /* total bytes handed out (excl. padding) */
    bool oom;                   /* sticky: set if any allocation failed */
} nx_arena;

typedef struct nx_arena_mark {
    nx_arena_chunk *chunk;
    nx_arena_chunk *next;   /* oversize chunks inserted after the mark must also be released */
    size_t used;
    size_t total;
} nx_arena_mark;

/* chunk_size == 0 selects 32 KiB. */
NX_API void  nx_arena_init(nx_arena *a, size_t chunk_size);
NX_API void  nx_arena_free(nx_arena *a);               /* releases all chunks */
NX_API void  nx_arena_reset(nx_arena *a);              /* keeps the first chunk, frees the rest */
NX_API void *nx_arena_alloc(nx_arena *a, size_t n, size_t align);   /* NULL on OOM (also sets a->oom) */
NX_API void *nx_arena_zalloc(nx_arena *a, size_t n, size_t align);
NX_API char *nx_arena_strndup(nx_arena *a, const char *s, size_t n);
NX_API char *nx_arena_strdup(nx_arena *a, const char *s);
NX_API nx_arena_mark nx_arena_save(const nx_arena *a);
/* m must be a live mark from this arena; restoring invalidates newer marks/pointers.
 * Frees all newer chunks, including oversize allocations. Does not clear sticky oom. */
NX_API void  nx_arena_restore(nx_arena *a, nx_arena_mark m);

#define NX_ARENA_NEW(a, T)        ((T *)nx_arena_zalloc((a), sizeof(T), _Alignof(T)))
#define NX_ARENA_NEW_ARRAY(a, T, n) ((T *)nx_arena_zalloc((a), sizeof(T) * (size_t)(n), _Alignof(T)))

#endif /* NX_ARENA_H */
