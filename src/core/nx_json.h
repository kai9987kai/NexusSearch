/* nx_json.h - bounded RFC 8259 syntax parser and compact serializer.
 * Trees and every string/raw number are arena-owned; input is borrowed only
 * during parse. Objects retain source order and reject duplicate decoded keys.
 * Strings/keys are length-aware UTF-8 and can contain escaped U+0000.
 * All numbers retain their original decimal lexeme (no floating-point loss).
 * exact_i64 is true only for integer lexemes representable as int64_t.
 * Trees are immutable after parse, freely shareable between readers. No global
 * mutable state. Bounds include cumulative decoded strings and work units.
 */
#ifndef NX_JSON_H
#define NX_JSON_H
#include "core/nx_config.h"
#include "core/nx_arena.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"

typedef enum nx_json_kind {
    NX_JSON_NULL, NX_JSON_BOOL, NX_JSON_NUMBER, NX_JSON_STRING,
    NX_JSON_ARRAY, NX_JSON_OBJECT
} nx_json_kind;
typedef struct nx_json_number {
    nx_slice raw;
    bool exact_i64;
    int64_t i64;
} nx_json_number;
typedef struct nx_json {
    nx_json_kind kind;
    size_t offset, length;      /* value source span, including quotes/brackets */
    nx_slice key;              /* object members only; decoded binary key */
    size_t key_offset, key_length;
    struct nx_json *child, *next; /* arena-owned linked children, source order */
    size_t count;
    union { bool boolean; nx_json_number number; nx_slice string; } as;
} nx_json;
typedef struct nx_json_limits {
    size_t max_input_bytes;     /* also bounds bytes appended by serializer */
    size_t max_nodes;
    size_t max_depth;           /* root depth 1, hard ceiling 256 */
    size_t max_string_bytes;    /* cumulative decoded strings and keys */
    size_t max_work;            /* byte scans, nodes, duplicate comparisons */
} nx_json_limits;

NX_API nx_json_limits nx_json_default_limits(void);
/* arena/input/limits/out/error are borrowed (limits/error may be NULL).
 * *out is arena-owned on success, NULL on error. Failed parsing restores the
 * arena mark. Malformed syntax returns NX_ERR_PARSE, budgets NX_ERR_LIMIT.
 * Zero fields mean zero allowed; a NULL limits pointer selects defaults. */
NX_API nx_status nx_json_parse(nx_arena *arena, nx_slice input, const nx_json_limits *limits,
                               nx_json **out, nx_error *error);
/* Borrowed tree/key; return a borrowed child or NULL for missing/wrong kind.
 * Binary keys are compared byte-for-byte without normalization. */
NX_API const nx_json *nx_json_get(const nx_json *object, nx_slice key);
NX_API const nx_json *nx_json_get_cstr(const nx_json *object, const char *key);
NX_API const nx_json *nx_json_at(const nx_json *array, size_t index);
/* Borrowed value/out; returns NX_ERR_TYPE if not an exact integer lexeme. */
NX_API nx_status nx_json_get_i64(const nx_json *value, int64_t *out);
/* Borrowed immutable parser-produced tree/limits/output/error. Appends compact
 * JSON preserving number lexemes and object order. Output remains caller-owned
 * (nx_buf_free); its original length is restored on failure. The output must
 * not alias tree storage. Serializer is bounded even for cycles in bad trees. */
NX_API nx_status nx_json_serialize(const nx_json *value, const nx_json_limits *limits,
                                   nx_buf *output, nx_error *error);
#endif
