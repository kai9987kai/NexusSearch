/* Immutable typed document table. All views/cells borrow validated bytes until
 * those bytes are freed/unmapped; opening allocates nothing. JSONL construction
 * appends transactionally to a caller-owned buffer. No mutable global state.
 * Empty/whitespace-only input produces a table with zero rows and fields.
 * Nonempty input requires unique, nonempty UTF-8 _id strings without NUL;
 * field zero is always _id. Names are nonempty UTF-8 without NUL. Null and
 * missing are equivalent. Integers outside int64 and mixed int/float columns
 * are rejected. Float/vector conversion uses a private C-runtime numeric locale
 * (allocated/freed by libc, the same exception as the query parser).
 * Defaults: input 64 MiB, 100k rows, 128 fields, 1 MiB per decoded field/name,
 * 4096 vector dimensions. Hard ceilings: input 256 MiB, 1m rows, 256 fields,
 * 16 MiB per field/name, 4096 dimensions, serialized table 1 GiB.
 * Format version 1 persists search-text profile 1 and little-endian cells.
 * Zero limit members mean zero allowed; NULL limits selects defaults. */
#ifndef NX_TABLE_H
#define NX_TABLE_H
#include "core/nx_buf.h"
#include "core/nx_status.h"
#include "index/nx_bsi.h"

typedef enum nx_field_type { NX_FIELD_NULL, NX_FIELD_INT, NX_FIELD_FLOAT,
    NX_FIELD_BOOL, NX_FIELD_TEXT, NX_FIELD_VECTOR } nx_field_type;
typedef struct nx_field { nx_slice name; nx_field_type type; uint32_t dims; } nx_field;
typedef struct nx_cell {
    bool present;
    nx_field_type type;
    union { int64_t integer; double real; bool boolean; nx_slice text;
        struct { nx_slice bytes; uint32_t dims; } vector; } as;
} nx_cell;
typedef struct nx_table { nx_slice bytes; uint32_t rows, fields; } nx_table;
typedef struct nx_table_limits {
    size_t max_input_bytes, max_rows, max_fields, max_field_bytes, max_vector_dims;
} nx_table_limits;
NX_API nx_table_limits nx_table_default_limits(void);
NX_API nx_status nx_table_build(nx_slice jsonl, const nx_table_limits *limits,
                               nx_buf *out, nx_error *error);
NX_API nx_status nx_table_open(nx_slice bytes, nx_table *out, nx_error *error);
NX_API nx_status nx_table_field(const nx_table *table, uint32_t field, nx_field *out);
NX_API nx_status nx_table_find(const nx_table *table, nx_slice name, uint32_t *out);
NX_API nx_status nx_table_get(const nx_table *table, uint32_t row, uint32_t field, nx_cell *out);
NX_API nx_slice nx_table_id(const nx_table *table, uint32_t row);
NX_API nx_slice nx_table_document(const nx_table *table, uint32_t row);
NX_API nx_status nx_table_int_index(const nx_table *table, uint32_t field, nx_bsi *out);
NX_API float nx_cell_vector_at(const nx_cell *cell, uint32_t dimension);
#endif
