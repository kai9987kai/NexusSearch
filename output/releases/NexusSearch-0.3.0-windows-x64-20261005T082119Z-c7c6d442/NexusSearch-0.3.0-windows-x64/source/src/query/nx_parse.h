/* nx_parse.h - bounded, schema-independent NexusQL parser.
 * Immutable results and all copied strings belong to the borrowed caller arena.
 * Parser state is local; distinct arenas may be used concurrently. Limits count
 * input bytes, recursive grammar nesting, final clauses, each list, decoded string
 * bytes, and total arena bytes requested during one call (including scratch).
 */
#ifndef NX_PARSE_H
#define NX_PARSE_H
#include "core/nx_config.h"
#include "core/nx_status.h"
#include "query/nx_ast.h"

typedef struct nx_parse_limits {
    size_t max_query_bytes;
    uint32_t max_depth;
    uint32_t max_clauses;
    uint32_t max_list_items;
    uint32_t max_string_bytes;
    size_t max_arena_bytes;
} nx_parse_limits;

/* Returns defaults: 64 KiB input, depth 64, 4096 clauses/list items,
 * 16 KiB strings and 16 MiB arena requests. */
NX_API nx_parse_limits nx_parse_limits_default(void);

/* text/limits/error are borrowed; limits/error may be NULL. text may be NULL
 * only when len is zero. On success *out is arena-owned, valid until arena
 * reset/free. On error *out is NULL and the arena mark is restored; its OOM
 * flag remains sticky. Invalid configuration returns NX_ERR_INVALID; syntax
 * errors NX_ERR_PARSE; exceeded limits NX_ERR_LIMIT; failed allocation NOMEM.
 * Query bytes may contain non-ASCII bytes, but never NUL. Four-digit positive
 * years are DATETIME scalars; numeric-only contexts (LIMIT/vector/boost) remain
 * NUMBER. A future binder must use the retained raw lexeme for numeric fields.
 */
NX_API nx_status nx_query_parse(nx_arena *arena, const char *text, size_t len,
                               const nx_parse_limits *limits, nx_stmt **out, nx_error *error);
#endif
