#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include "seg/nx_table.h"
#include "core/nx_crc32c.h"
#include "core/nx_json.h"
#include "core/nx_mem.h"
#include "core/nx_utf8.h"
#include <float.h>
#include <locale.h>

#ifdef NX_WINDOWS
typedef _locale_t table_locale;
#else
typedef locale_t table_locale;
#endif

enum { TABLE_HEADER = 128, FIELD_BYTES = 80, TABLE_VERSION = 1, TEXT_PROFILE = 1,
       HARD_ROWS = 1000000, HARD_FIELDS = 256, HARD_DIMS = 4096,
       HARD_INPUT = 256 * 1024 * 1024, HARD_FIELD = 16 * 1024 * 1024,
       HARD_TABLE = 1024 * 1024 * 1024 };
static const uint8_t table_magic[8] = {'N','X','T','A','B','L','E','1'};

typedef struct field_record {
    uint64_t name_off, name_len;
    uint32_t type, dims;
    uint64_t col_off, col_len, var_off, var_len, bsi_off, bsi_len, reserved;
} field_record;
typedef struct build_field { nx_slice name; nx_field_type type; uint32_t dims;
    nx_buf col, variable, index; } build_field;
typedef struct id_row { nx_slice id; uint32_t row; } id_row;
typedef struct builder {
    nx_table_limits limits;
    nx_arena schema, parse;
    build_field *fields;
    uint32_t rows, count;
    size_t storage;
    nx_buf docs;             /* source offset/length pairs */
    table_locale locale;
    nx_error *error;
} builder;

nx_table_limits nx_table_default_limits(void) {
    nx_table_limits limits = {64u * 1024u * 1024u, 100000, 128, 1024u * 1024u, 4096};
    return limits;
}

static nx_status failure(nx_error *error, nx_status status, const char *message) {
    nx_error_set(error, status, -1, 0, "%s", message);
    return status;
}
static bool blank(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool valid_name(nx_slice name) {
    return name.n != 0 && name.p != NULL && memchr(name.p, 0, name.n) == NULL
        && nx_utf8_validate(name, NULL) == NX_OK;
}
static int slice_order(nx_slice a, nx_slice b) {
    size_t n = NX_MIN(a.n, b.n);
    int cmp = n ? memcmp(a.p, b.p, n) : 0;
    return cmp ? cmp : (a.n > b.n) - (a.n < b.n);
}
static int id_order(const void *a, const void *b) {
    return slice_order(((const id_row *)a)->id, ((const id_row *)b)->id);
}
static size_t cell_width(uint32_t type, uint32_t dims) {
    switch (type) {
    case NX_FIELD_NULL: return 0;
    case NX_FIELD_BOOL: return 1;
    case NX_FIELD_INT: case NX_FIELD_FLOAT: return 8;
    case NX_FIELD_TEXT: return 16;
    case NX_FIELD_VECTOR: return (size_t)dims * 4;
    default: return SIZE_MAX;
    }
}
static nx_status budget(builder *b, size_t extra) {
    if (extra > HARD_TABLE || b->storage > HARD_TABLE - extra)
        return failure(b->error, NX_ERR_LIMIT, "table exceeds the 1 GiB format limit");
    b->storage += extra;
    return NX_OK;
}
static nx_status as_double(builder *b, const nx_json *value, double *out) {
    nx_slice raw = value->as.number.raw;
    if (!b->locale) {
#ifdef NX_WINDOWS
        b->locale = _create_locale(LC_NUMERIC, "C");
#else
        b->locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
#endif
        if (!b->locale) return NX_ERR_NOMEM;
    }
    /* Parser-owned raw number storage is not a public NUL-termination promise. */
    char *terminated = nx_arena_alloc(&b->parse, raw.n + 1, 1);
    if (!terminated) return NX_ERR_NOMEM;
    memcpy(terminated, raw.p, raw.n); terminated[raw.n] = 0;
    char *end = NULL;
#ifdef NX_WINDOWS
    double number = _strtod_l(terminated, &end, b->locale);
#else
    double number = strtod_l(terminated, &end, b->locale);
#endif
    if (end != terminated + raw.n || !(number >= -DBL_MAX && number <= DBL_MAX))
        return failure(b->error, NX_ERR_TYPE, "numeric field must be finite float64");
    *out = number;
    return NX_OK;
}
static nx_status classify(builder *b, const nx_json *value, nx_field_type *type, uint32_t *dims) {
    *dims = 0;
    switch (value->kind) {
    case NX_JSON_NULL: *type = NX_FIELD_NULL; return NX_OK;
    case NX_JSON_BOOL: *type = NX_FIELD_BOOL; return NX_OK;
    case NX_JSON_STRING:
        if (value->as.string.n > b->limits.max_field_bytes) return NX_ERR_LIMIT;
        *type = NX_FIELD_TEXT; return NX_OK;
    case NX_JSON_NUMBER:
        if (value->as.number.exact_i64) { *type = NX_FIELD_INT; return NX_OK; }
        if (!memchr(value->as.number.raw.p, '.', value->as.number.raw.n)
            && !memchr(value->as.number.raw.p, 'e', value->as.number.raw.n)
            && !memchr(value->as.number.raw.p, 'E', value->as.number.raw.n))
            return failure(b->error, NX_ERR_TYPE, "integer field is outside the exact int64 range");
        *type = NX_FIELD_FLOAT;
        { double ignored; return as_double(b, value, &ignored); }
    case NX_JSON_ARRAY:
        if (!value->count) return failure(b->error, NX_ERR_TYPE, "vectors must have at least one dimension");
        if (value->count > b->limits.max_vector_dims) return NX_ERR_LIMIT;
        for (const nx_json *item = value->child; item; item = item->next) {
            double number;
            if (item->kind != NX_JSON_NUMBER) return NX_ERR_TYPE;
            nx_status status = as_double(b, item, &number);
            if (status != NX_OK) return status;
            if (!(number >= -(double)FLT_MAX && number <= (double)FLT_MAX))
                return failure(b->error, NX_ERR_TYPE, "vector entries must be finite float32");
        }
        *type = NX_FIELD_VECTOR; *dims = (uint32_t)value->count; return NX_OK;
    default: return failure(b->error, NX_ERR_UNSUPPORTED, "nested objects are not supported table fields");
    }
}
static nx_status add_schema(builder *b, const nx_json *value) {
    if (!valid_name(value->key)) return failure(b->error, NX_ERR_INVALID, "field names must be nonempty UTF-8 without NUL");
    if (value->key.n > b->limits.max_field_bytes) return NX_ERR_LIMIT;
    uint32_t found = b->count;
    for (uint32_t i = 0; i < b->count; i++) {
        if (nx_slice_eq(b->fields[i].name, value->key)) { found = i; break; }
    }
    nx_field_type type; uint32_t dims;
    nx_status status = classify(b, value, &type, &dims);
    if (status != NX_OK) return status;
    if (found == b->count) {
        if (b->count == b->limits.max_fields) return NX_ERR_LIMIT;
        uint8_t *name = nx_arena_alloc(&b->schema, value->key.n, 1);
        if (!name) return NX_ERR_NOMEM;
        memcpy(name, value->key.p, value->key.n);
        b->fields[found].name = nx_slice_make(name, value->key.n);
        b->fields[found].type = type; b->fields[found].dims = dims; b->count++;
    } else if (type != NX_FIELD_NULL) {
        if (b->fields[found].type == NX_FIELD_NULL) {
            b->fields[found].type = type; b->fields[found].dims = dims;
        } else if (b->fields[found].type != type || b->fields[found].dims != dims) {
            return failure(b->error, NX_ERR_TYPE, "field types or vector dimensions differ between documents");
        }
    }
    return NX_OK;
}
static nx_status parse_document(builder *b, nx_slice input, nx_json **out) {
    nx_arena_reset(&b->parse);
    nx_json_limits limits = nx_json_default_limits();
    limits.max_input_bytes = b->limits.max_input_bytes;
    limits.max_string_bytes = b->limits.max_input_bytes;
    limits.max_nodes = 1 + b->limits.max_fields * (b->limits.max_vector_dims + 1);
    limits.max_depth = 3;
    limits.max_work = (input.n + 1) * (b->limits.max_fields + 8);
    nx_status status = nx_json_parse(&b->parse, input, &limits, out, b->error);
    if (status != NX_OK) return status;
    if ((*out)->kind != NX_JSON_OBJECT) return failure(b->error, NX_ERR_TYPE, "each JSONL row must be an object");
    return NX_OK;
}
static nx_status infer_schema(builder *b, nx_slice input) {
    size_t begin = 0;
    while (begin < input.n) {
        size_t end = begin;
        while (end < input.n && input.p[end] != '\n') end++;
        size_t next = end < input.n ? end + 1 : end;
        while (begin < end && blank(input.p[begin])) begin++;
        while (end > begin && blank(input.p[end - 1])) end--;
        if (begin != end) {
            if (b->rows == b->limits.max_rows) return NX_ERR_LIMIT;
            nx_json *object = NULL;
            nx_status status = parse_document(b, nx_slice_sub(input, begin, end - begin), &object);
            if (status != NX_OK) return status;
            const nx_json *id = nx_json_get_cstr(object, "_id");
            if (!id || id->kind != NX_JSON_STRING || !valid_name(id->as.string))
                return failure(b->error, NX_ERR_INVALID, "each document requires a nonempty UTF-8 _id without NUL");
            status = add_schema(b, id);
            if (status != NX_OK) return status;
            for (const nx_json *value = object->child; value; value = value->next) {
                if (value == id) continue;
                status = add_schema(b, value);
                if (status != NX_OK) return status;
            }
            nx_buf_put_u64(&b->docs, begin); nx_buf_put_u64(&b->docs, end - begin);
            if (b->docs.oom) return NX_ERR_NOMEM;
            b->rows++;
        }
        begin = next;
    }
    return NX_OK;
}
static nx_slice input_doc(const builder *b, nx_slice input, uint32_t row) {
    nx_cursor cursor = nx_cursor_make(nx_buf_slice(&b->docs));
    (void)nx_rd_seek(&cursor, (size_t)row * 16);
    size_t off = (size_t)nx_rd_u64(&cursor), len = (size_t)nx_rd_u64(&cursor);
    return nx_slice_sub(input, off, len);
}
static nx_status build_columns(builder *b, nx_slice input) {
    for (uint32_t i = 0; i < b->count; i++) {
        build_field *field = &b->fields[i];
        size_t bytes;
        if (nx_mul_overflow((size_t)b->rows, cell_width(field->type, field->dims) + 1, &bytes)) return NX_ERR_LIMIT;
        nx_status status = budget(b, bytes + field->name.n);
        if (status != NX_OK) return status;
        nx_buf_put_zeros(&field->col, bytes);
        if (field->col.oom) return NX_ERR_NOMEM;
    }
    for (uint32_t row = 0; row < b->rows; row++) {
        nx_json *object = NULL;
        nx_status status = parse_document(b, input_doc(b, input, row), &object);
        if (status != NX_OK) return status;
        for (const nx_json *value = object->child; value; value = value->next) {
            if (value->kind == NX_JSON_NULL) continue;
            uint32_t f = 0;
            while (f < b->count && !nx_slice_eq(b->fields[f].name, value->key)) f++;
            if (f == b->count) return NX_ERR_INTERNAL;
            build_field *field = &b->fields[f];
            field->col.data[row] = 1;
            uint8_t *cell = field->col.data + b->rows + (size_t)row * cell_width(field->type, field->dims);
            switch (field->type) {
            case NX_FIELD_INT: {
                uint64_t bits; memcpy(&bits, &value->as.number.i64, 8); nx_st64(cell, bits); break;
            }
            case NX_FIELD_FLOAT: {
                double number; status = as_double(b, value, &number);
                if (status != NX_OK) return status;
                nx_stf64(cell, number); break;
            }
            case NX_FIELD_BOOL: cell[0] = value->as.boolean ? 1 : 0; break;
            case NX_FIELD_TEXT:
                status = budget(b, value->as.string.n);
                if (status != NX_OK) return status;
                nx_st64(cell, field->variable.len); nx_st64(cell + 8, value->as.string.n);
                nx_buf_put(&field->variable, value->as.string.p, value->as.string.n);
                if (field->variable.oom) return NX_ERR_NOMEM;
                break;
            case NX_FIELD_VECTOR: {
                uint32_t dimension = 0;
                for (const nx_json *item = value->child; item; item = item->next) {
                    double number; status = as_double(b, item, &number);
                    if (status != NX_OK) return status;
                    nx_stf32(cell + (size_t)dimension * 4, (float)number); dimension++;
                }
                break;
            }
            default: return NX_ERR_INTERNAL;
            }
        }
    }
    for (uint32_t f = 0; f < b->count; f++) {
        build_field *field = &b->fields[f];
        if (field->type != NX_FIELD_INT) continue;
        int64_t *values = NX_NEW_ARRAY(int64_t, b->rows);
        if (!values) return NX_ERR_NOMEM;
        memcpy(values, field->col.data + b->rows, (size_t)b->rows * 8);
        nx_status status = nx_bsi_build(values, field->col.data, b->rows, &field->index);
        nx_free(values);
        if (status != NX_OK) return status;
        status = budget(b, field->index.len);
        if (status != NX_OK) return status;
    }
    return NX_OK;
}

static field_record read_record(nx_slice bytes, uint32_t field) {
    nx_cursor c = nx_cursor_make(bytes);
    (void)nx_rd_seek(&c, TABLE_HEADER + (size_t)field * FIELD_BYTES);
    field_record r;
    r.name_off = nx_rd_u64(&c); r.name_len = nx_rd_u64(&c);
    r.type = nx_rd_u32(&c); r.dims = nx_rd_u32(&c);
    r.col_off = nx_rd_u64(&c); r.col_len = nx_rd_u64(&c);
    r.var_off = nx_rd_u64(&c); r.var_len = nx_rd_u64(&c);
    r.bsi_off = nx_rd_u64(&c); r.bsi_len = nx_rd_u64(&c); r.reserved = nx_rd_u64(&c);
    return r;
}
static void write_record(nx_buf *buffer, uint32_t field, field_record r) {
    size_t off = TABLE_HEADER + (size_t)field * FIELD_BYTES;
    nx_buf_patch_u64(buffer, off, r.name_off); nx_buf_patch_u64(buffer, off + 8, r.name_len);
    nx_buf_patch_u32(buffer, off + 16, r.type); nx_buf_patch_u32(buffer, off + 20, r.dims);
    nx_buf_patch_u64(buffer, off + 24, r.col_off); nx_buf_patch_u64(buffer, off + 32, r.col_len);
    nx_buf_patch_u64(buffer, off + 40, r.var_off); nx_buf_patch_u64(buffer, off + 48, r.var_len);
    nx_buf_patch_u64(buffer, off + 56, r.bsi_off); nx_buf_patch_u64(buffer, off + 64, r.bsi_len);
}
static nx_slice field_text(const build_field *field, uint32_t rows, uint32_t row) {
    nx_cursor c = nx_cursor_make(nx_buf_slice(&field->col));
    (void)nx_rd_seek(&c, rows + (size_t)row * 16);
    size_t off = (size_t)nx_rd_u64(&c), len = (size_t)nx_rd_u64(&c);
    return nx_slice_sub(nx_buf_slice(&field->variable), off, len);
}
static nx_status serialize_table(builder *b, nx_slice input, nx_buf *output) {
    size_t docs_off = TABLE_HEADER + (size_t)b->count * FIELD_BYTES;
    size_t ids_off = docs_off + (size_t)b->rows * 16;
    size_t data_off = ids_off + (size_t)b->rows * 4;
    nx_status status = budget(b, data_off);
    if (status != NX_OK) return status;
    nx_buf_put_zeros(output, data_off);
    if (output->oom) return NX_ERR_NOMEM;
    id_row *ids = b->rows ? NX_NEW_ARRAY(id_row, b->rows) : NULL;
    if (b->rows && !ids) return NX_ERR_NOMEM;
    for (uint32_t row = 0; row < b->rows; row++) {
        ids[row].id = field_text(&b->fields[0], b->rows, row); ids[row].row = row;
    }
    if (b->rows > 1) qsort(ids, b->rows, sizeof *ids, id_order);
    for (uint32_t i = 0; i < b->rows; i++) {
        if (i && nx_slice_eq(ids[i - 1].id, ids[i].id)) {
            nx_free(ids); return failure(b->error, NX_ERR_EXISTS, "document _id values must be unique");
        }
        nx_buf_patch_u32(output, ids_off + (size_t)i * 4, ids[i].row);
    }
    nx_free(ids);
    for (uint32_t row = 0; row < b->rows; row++) {
        nx_slice document = input_doc(b, input, row);
        status = budget(b, document.n);
        if (status != NX_OK) return status;
        nx_buf_patch_u64(output, docs_off + (size_t)row * 16, output->len);
        nx_buf_patch_u64(output, docs_off + (size_t)row * 16 + 8, document.n);
        nx_buf_put(output, document.p, document.n);
    }
    for (uint32_t f = 0; f < b->count; f++) {
        build_field *field = &b->fields[f]; field_record r = {0};
        r.name_off = output->len; r.name_len = field->name.n;
        nx_buf_put(output, field->name.p, field->name.n);
        r.type = (uint32_t)field->type; r.dims = field->dims;
        r.col_off = output->len; r.col_len = field->col.len;
        nx_buf_put(output, field->col.data, field->col.len);
        r.var_off = output->len; r.var_len = field->variable.len;
        nx_buf_put(output, field->variable.data, field->variable.len);
        r.bsi_off = output->len; r.bsi_len = field->index.len;
        nx_buf_put(output, field->index.data, field->index.len);
        write_record(output, f, r);
    }
    if (output->oom) return NX_ERR_NOMEM;
    memcpy(output->data, table_magic, sizeof table_magic);
    nx_buf_patch_u32(output, 8, TABLE_VERSION); nx_buf_patch_u32(output, 12, TEXT_PROFILE);
    nx_buf_patch_u32(output, 16, b->rows); nx_buf_patch_u32(output, 20, b->count);
    nx_buf_patch_u64(output, 32, TABLE_HEADER);
    nx_buf_patch_u64(output, 40, (size_t)b->count * FIELD_BYTES);
    nx_buf_patch_u64(output, 48, docs_off); nx_buf_patch_u64(output, 56, (size_t)b->rows * 16);
    nx_buf_patch_u64(output, 64, ids_off); nx_buf_patch_u64(output, 72, (size_t)b->rows * 4);
    nx_buf_patch_u64(output, 80, data_off); nx_buf_patch_u64(output, 88, output->len - data_off);
    nx_buf_patch_u64(output, 96, output->len);
    nx_buf_patch_u32(output, 108, nx_crc32c(0, output->data + TABLE_HEADER, data_off - TABLE_HEADER));
    nx_buf_patch_u32(output, 112, nx_crc32c(0, output->data + data_off, output->len - data_off));
    nx_buf_patch_u32(output, 104, nx_crc32c(0, output->data, TABLE_HEADER));
    return NX_OK;
}

nx_status nx_table_build(nx_slice jsonl, const nx_table_limits *limits, nx_buf *out, nx_error *error) {
    nx_error_clear(error);
    if (!out || (!jsonl.p && jsonl.n)) return failure(error, NX_ERR_INVALID, "invalid table build arguments");
    if (out->oom) return failure(error, NX_ERR_NOMEM, "destination buffer is out of memory");
    builder b; memset(&b, 0, sizeof b);
    b.limits = limits ? *limits : nx_table_default_limits(); b.error = error;
    if (b.limits.max_input_bytes > HARD_INPUT || b.limits.max_rows > HARD_ROWS
        || b.limits.max_fields > HARD_FIELDS || b.limits.max_field_bytes > HARD_FIELD
        || b.limits.max_vector_dims > HARD_DIMS || jsonl.n > b.limits.max_input_bytes)
        return failure(error, NX_ERR_LIMIT, "table input or configured limits exceed hard ceilings");
    nx_arena_init(&b.schema, 4096); nx_arena_init(&b.parse, 4096); nx_buf_init(&b.docs);
    nx_buf encoded; nx_buf_init(&encoded);
    b.fields = b.limits.max_fields ? NX_NEW_ARRAY(build_field, b.limits.max_fields) : NULL;
    nx_status status = b.limits.max_fields && !b.fields ? NX_ERR_NOMEM : NX_OK;
    if (status == NX_OK) status = infer_schema(&b, jsonl);
    if (status == NX_OK) status = build_columns(&b, jsonl);
    if (status == NX_OK) status = serialize_table(&b, jsonl, &encoded);
    if (status == NX_OK) {
        nx_table view;
        status = nx_table_open(nx_buf_slice(&encoded), &view, error);
    }
    if (status == NX_OK) {
        size_t previous = out->len;
        nx_buf_put(out, encoded.data, encoded.len);
        if (out->oom) { out->len = previous; status = NX_ERR_NOMEM; }
    }
    for (uint32_t i = 0; i < b.count; i++) {
        nx_buf_free(&b.fields[i].col); nx_buf_free(&b.fields[i].variable); nx_buf_free(&b.fields[i].index);
    }
#ifdef NX_WINDOWS
    if (b.locale) _free_locale(b.locale);
#else
    if (b.locale) freelocale(b.locale);
#endif
    nx_free(b.fields); nx_buf_free(&b.docs); nx_buf_free(&encoded);
    nx_arena_free(&b.parse); nx_arena_free(&b.schema);
    if (status != NX_OK && (!error || error->code == NX_OK))
        (void)failure(error, status, status == NX_ERR_NOMEM ? "table allocation failed" : "table input exceeds limits or has incompatible types");
    return status;
}

static bool consume_section(nx_slice bytes, size_t *next, uint64_t off, uint64_t len) {
    if (off != *next || off > bytes.n || len > bytes.n - (size_t)off) return false;
    *next += (size_t)len;
    return true;
}
static bool zero_bytes(nx_slice bytes) {
    for (size_t i = 0; i < bytes.n; i++) if (bytes.p[i]) return false;
    return true;
}

nx_status nx_table_field(const nx_table *table, uint32_t field, nx_field *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!table || !out || !table->bytes.p || field >= table->fields) return NX_ERR_INVALID;
    field_record r = read_record(table->bytes, field);
    out->name = nx_slice_sub(table->bytes, (size_t)r.name_off, (size_t)r.name_len);
    out->type = (nx_field_type)r.type; out->dims = r.dims;
    return NX_OK;
}
nx_status nx_table_find(const nx_table *table, nx_slice name, uint32_t *out) {
    if (out) *out = UINT32_MAX;
    if (!table || !out || (!name.p && name.n)) return NX_ERR_INVALID;
    for (uint32_t f = 0; f < table->fields; f++) {
        nx_field field;
        nx_status status = nx_table_field(table, f, &field);
        if (status != NX_OK) return status;
        if (nx_slice_eq(field.name, name)) { *out = f; return NX_OK; }
    }
    return NX_ERR_NOT_FOUND;
}
nx_status nx_table_get(const nx_table *table, uint32_t row, uint32_t field, nx_cell *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!table || !out || !table->bytes.p || row >= table->rows || field >= table->fields) return NX_ERR_INVALID;
    field_record r = read_record(table->bytes, field);
    nx_cursor c = nx_cursor_make(nx_slice_sub(table->bytes, (size_t)r.col_off, (size_t)r.col_len));
    (void)nx_rd_seek(&c, row); out->present = nx_rd_u8(&c) == 1; out->type = (nx_field_type)r.type;
    if (!out->present) return NX_OK;
    (void)nx_rd_seek(&c, table->rows + (size_t)row * cell_width(r.type, r.dims));
    switch (r.type) {
    case NX_FIELD_INT: { uint64_t bits = nx_rd_u64(&c); memcpy(&out->as.integer, &bits, 8); break; }
    case NX_FIELD_FLOAT: out->as.real = nx_rd_f64(&c); break;
    case NX_FIELD_BOOL: out->as.boolean = nx_rd_u8(&c) != 0; break;
    case NX_FIELD_TEXT: {
        size_t off = (size_t)nx_rd_u64(&c), len = (size_t)nx_rd_u64(&c);
        out->as.text = nx_slice_sub(nx_slice_sub(table->bytes, (size_t)r.var_off, (size_t)r.var_len), off, len);
        break;
    }
    case NX_FIELD_VECTOR:
        out->as.vector.bytes = nx_rd_bytes(&c, (size_t)r.dims * 4); out->as.vector.dims = r.dims; break;
    default: return NX_ERR_CORRUPT;
    }
    return c.err ? NX_ERR_CORRUPT : NX_OK;
}
nx_slice nx_table_id(const nx_table *table, uint32_t row) {
    nx_cell cell;
    if (nx_table_get(table, row, 0, &cell) != NX_OK || !cell.present || cell.type != NX_FIELD_TEXT)
        return nx_slice_make(NULL, 0);
    return cell.as.text;
}
nx_slice nx_table_document(const nx_table *table, uint32_t row) {
    if (!table || !table->bytes.p || row >= table->rows) return nx_slice_make(NULL, 0);
    nx_cursor c = nx_cursor_make(table->bytes);
    (void)nx_rd_seek(&c, TABLE_HEADER + (size_t)table->fields * FIELD_BYTES + (size_t)row * 16);
    size_t off = (size_t)nx_rd_u64(&c), len = (size_t)nx_rd_u64(&c);
    return nx_slice_sub(table->bytes, off, len);
}
nx_status nx_table_int_index(const nx_table *table, uint32_t field, nx_bsi *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!table || !out || !table->bytes.p || field >= table->fields) return NX_ERR_INVALID;
    field_record r = read_record(table->bytes, field);
    if (r.type != NX_FIELD_INT) return NX_ERR_TYPE;
    return nx_bsi_open(nx_slice_sub(table->bytes, (size_t)r.bsi_off, (size_t)r.bsi_len), out);
}
float nx_cell_vector_at(const nx_cell *cell, uint32_t dimension) {
    if (!cell || !cell->present || cell->type != NX_FIELD_VECTOR || dimension >= cell->as.vector.dims) return 0;
    nx_cursor c = nx_cursor_make(cell->as.vector.bytes);
    (void)nx_rd_seek(&c, (size_t)dimension * 4);
    return nx_rd_f32(&c);
}

static nx_status validate_column(const nx_table *table, field_record r) {
    nx_slice column = nx_slice_sub(table->bytes, (size_t)r.col_off, (size_t)r.col_len);
    nx_slice variable = nx_slice_sub(table->bytes, (size_t)r.var_off, (size_t)r.var_len);
    size_t width = cell_width(r.type, r.dims), expected;
    if (nx_mul_overflow(table->rows, width + 1, &expected) || r.col_len != expected) return NX_ERR_CORRUPT;
    nx_bsi index = {0};
    if (r.type == NX_FIELD_INT) {
        nx_status status = nx_bsi_open(nx_slice_sub(table->bytes, (size_t)r.bsi_off, (size_t)r.bsi_len), &index);
        if (status != NX_OK || index.rows != table->rows) return NX_ERR_CORRUPT;
    } else if (r.bsi_len) return NX_ERR_CORRUPT;
    if (r.type != NX_FIELD_TEXT && r.var_len) return NX_ERR_CORRUPT;
    size_t text_next = 0;
    for (uint32_t row = 0; row < table->rows; row++) {
        nx_cursor flags = nx_cursor_make(column); (void)nx_rd_seek(&flags, row);
        uint8_t present = nx_rd_u8(&flags);
        if (present > 1 || (r.type == NX_FIELD_NULL && present)) return NX_ERR_CORRUPT;
        nx_slice bytes = nx_slice_sub(column, table->rows + (size_t)row * width, width);
        nx_cursor c = nx_cursor_make(bytes);
        if (!present && !zero_bytes(bytes)) return NX_ERR_CORRUPT;
        switch (r.type) {
        case NX_FIELD_FLOAT: {
            double value = nx_rd_f64(&c);
            if (!(value >= -DBL_MAX && value <= DBL_MAX)) return NX_ERR_CORRUPT;
            break;
        }
        case NX_FIELD_BOOL: if (nx_rd_u8(&c) > 1) return NX_ERR_CORRUPT; break;
        case NX_FIELD_TEXT: {
            uint64_t off = nx_rd_u64(&c), len = nx_rd_u64(&c);
            if (len > HARD_FIELD || off > variable.n || len > variable.n - (size_t)off) return NX_ERR_CORRUPT;
            if (present) {
                if (off != text_next) return NX_ERR_CORRUPT;
                nx_slice text = nx_slice_sub(variable, (size_t)off, (size_t)len);
                if (nx_utf8_validate(text, NULL) != NX_OK) return NX_ERR_CORRUPT;
                text_next += (size_t)len;
            }
            break;
        }
        case NX_FIELD_VECTOR:
            for (uint32_t d = 0; d < r.dims; d++) {
                float value = nx_rd_f32(&c);
                if (!(value >= -FLT_MAX && value <= FLT_MAX)) return NX_ERR_CORRUPT;
            }
            break;
        case NX_FIELD_INT: {
            uint64_t bits = nx_rd_u64(&c); int64_t value; memcpy(&value, &bits, 8);
            int64_t indexed; bool exists;
            if (nx_bsi_get(&index, row, &indexed, &exists) != NX_OK
                || exists != (present != 0) || (exists && indexed != value)) return NX_ERR_CORRUPT;
            break;
        }
        default: break;
        }
        if (c.err) return NX_ERR_CORRUPT;
    }
    return text_next == variable.n ? NX_OK : NX_ERR_CORRUPT;
}

static nx_status document_matches_cell(builder *parser, const nx_json *value, const nx_cell *cell) {
    if (value->kind == NX_JSON_NULL) return cell->present ? NX_ERR_CORRUPT : NX_OK;
    nx_field_type type; uint32_t dims;
    nx_status status = classify(parser, value, &type, &dims);
    if (status != NX_OK) return status;
    if (!cell->present || cell->type != type) return NX_ERR_CORRUPT;
    switch (type) {
    case NX_FIELD_INT: return cell->as.integer == value->as.number.i64 ? NX_OK : NX_ERR_CORRUPT;
    case NX_FIELD_FLOAT: {
        double number; status = as_double(parser, value, &number);
        if (status != NX_OK) return status;
        return memcmp(&number, &cell->as.real, sizeof number) == 0 ? NX_OK : NX_ERR_CORRUPT;
    }
    case NX_FIELD_BOOL: return cell->as.boolean == value->as.boolean ? NX_OK : NX_ERR_CORRUPT;
    case NX_FIELD_TEXT: return nx_slice_eq(cell->as.text, value->as.string) ? NX_OK : NX_ERR_CORRUPT;
    case NX_FIELD_VECTOR: {
        if (dims != cell->as.vector.dims) return NX_ERR_CORRUPT;
        uint32_t dimension = 0;
        for (const nx_json *item = value->child; item; item = item->next) {
            double number; status = as_double(parser, item, &number);
            if (status != NX_OK) return status;
            float expected = (float)number, actual = nx_cell_vector_at(cell, dimension++);
            if (memcmp(&expected, &actual, sizeof expected) != 0) return NX_ERR_CORRUPT;
        }
        return NX_OK;
    }
    default: return NX_ERR_CORRUPT;
    }
}

static nx_status validate_documents(const nx_table *table) {
    builder parser; memset(&parser, 0, sizeof parser);
    parser.limits = nx_table_default_limits();
    parser.limits.max_input_bytes = HARD_INPUT; parser.limits.max_fields = HARD_FIELDS;
    parser.limits.max_field_bytes = HARD_FIELD; parser.limits.max_vector_dims = HARD_DIMS;
    nx_arena_init(&parser.parse, 4096);
    nx_status status = NX_OK;
    for (uint32_t row = 0; row < table->rows && status == NX_OK; row++) {
        nx_json *object = NULL;
        status = parse_document(&parser, nx_table_document(table, row), &object);
        if (status != NX_OK) break;
        bool seen[HARD_FIELDS] = {false};
        for (const nx_json *value = object->child; value; value = value->next) {
            uint32_t field;
            if (nx_table_find(table, value->key, &field) != NX_OK) { status = NX_ERR_CORRUPT; break; }
            seen[field] = true;
            nx_cell cell;
            status = nx_table_get(table, row, field, &cell);
            if (status == NX_OK) status = document_matches_cell(&parser, value, &cell);
            if (status != NX_OK) break;
        }
        if (!seen[0] && status == NX_OK) status = NX_ERR_CORRUPT;
        for (uint32_t field = 0; field < table->fields && status == NX_OK; field++) {
            if (seen[field]) continue;
            nx_cell cell; status = nx_table_get(table, row, field, &cell);
            if (cell.present) status = NX_ERR_CORRUPT;
        }
    }
#ifdef NX_WINDOWS
    if (parser.locale) _free_locale(parser.locale);
#else
    if (parser.locale) freelocale(parser.locale);
#endif
    nx_arena_free(&parser.parse);
    return status == NX_OK || status == NX_ERR_NOMEM ? status : NX_ERR_CORRUPT;
}

nx_status nx_table_open(nx_slice bytes, nx_table *out, nx_error *error) {
    nx_error_clear(error);
    if (out) memset(out, 0, sizeof *out);
    if (!out || (!bytes.p && bytes.n)) return failure(error, NX_ERR_INVALID, "invalid table open arguments");
    if (bytes.n < TABLE_HEADER || bytes.n > HARD_TABLE) return failure(error, NX_ERR_CORRUPT, "invalid table length");
    nx_cursor c = nx_cursor_make(bytes);
    nx_slice magic = nx_rd_bytes(&c, 8);
    if (memcmp(magic.p, table_magic, 8)) return failure(error, NX_ERR_CORRUPT, "invalid table magic");
    uint32_t version = nx_rd_u32(&c), profile = nx_rd_u32(&c);
    if (version != TABLE_VERSION || profile != TEXT_PROFILE) return failure(error, NX_ERR_VERSION, "unsupported table format or text profile");
    uint32_t rows = nx_rd_u32(&c), fields = nx_rd_u32(&c);
    if (rows > HARD_ROWS || fields > HARD_FIELDS || (rows == 0) != (fields == 0)
        || nx_rd_u32(&c) != 0 || nx_rd_u32(&c) != 0) goto corrupt;
    uint64_t dir_off = nx_rd_u64(&c), dir_len = nx_rd_u64(&c);
    uint64_t docs_off = nx_rd_u64(&c), docs_len = nx_rd_u64(&c);
    uint64_t ids_off = nx_rd_u64(&c), ids_len = nx_rd_u64(&c);
    uint64_t data_off = nx_rd_u64(&c), data_len = nx_rd_u64(&c), total = nx_rd_u64(&c);
    uint32_t header_crc = nx_rd_u32(&c), directory_crc = nx_rd_u32(&c), data_crc = nx_rd_u32(&c);
    if (c.err || total != bytes.n || !zero_bytes(nx_rd_bytes(&c, 12))) goto corrupt;
    size_t next = TABLE_HEADER;
    if (dir_len != (uint64_t)fields * FIELD_BYTES || docs_len != (uint64_t)rows * 16
        || ids_len != (uint64_t)rows * 4
        || !consume_section(bytes, &next, dir_off, dir_len)
        || !consume_section(bytes, &next, docs_off, docs_len)
        || !consume_section(bytes, &next, ids_off, ids_len)
        || !consume_section(bytes, &next, data_off, data_len) || next != bytes.n) goto corrupt;
    uint8_t header[TABLE_HEADER]; memcpy(header, bytes.p, TABLE_HEADER); nx_st32(header + 104, 0);
    if (nx_crc32c(0, header, TABLE_HEADER) != header_crc
        || nx_crc32c(0, bytes.p + TABLE_HEADER, (size_t)data_off - TABLE_HEADER) != directory_crc
        || nx_crc32c(0, bytes.p + (size_t)data_off, (size_t)data_len) != data_crc) goto corrupt;
    nx_table table = {bytes, rows, fields};
    next = (size_t)data_off;
    for (uint32_t row = 0; row < rows; row++) {
        nx_cursor doc = nx_cursor_make(nx_slice_sub(bytes, (size_t)docs_off, (size_t)docs_len));
        (void)nx_rd_seek(&doc, (size_t)row * 16);
        uint64_t off = nx_rd_u64(&doc), len = nx_rd_u64(&doc);
        if (!len || len > HARD_INPUT || !consume_section(bytes, &next, off, len)) goto corrupt;
        nx_slice document = nx_slice_sub(bytes, (size_t)off, (size_t)len);
        if (document.p[0] != '{' || document.p[document.n - 1] != '}'
            || nx_utf8_validate(document, NULL) != NX_OK) goto corrupt;
    }
    if (next - (size_t)data_off > HARD_INPUT) goto corrupt;
    for (uint32_t f = 0; f < fields; f++) {
        field_record r = read_record(bytes, f);
        if (!r.name_len || r.name_len > HARD_FIELD || r.type > NX_FIELD_VECTOR || r.reserved
            || (r.type == NX_FIELD_VECTOR ? (!r.dims || r.dims > HARD_DIMS) : r.dims != 0)
            || !consume_section(bytes, &next, r.name_off, r.name_len)
            || !consume_section(bytes, &next, r.col_off, r.col_len)
            || !consume_section(bytes, &next, r.var_off, r.var_len)
            || !consume_section(bytes, &next, r.bsi_off, r.bsi_len)) goto corrupt;
        nx_slice name = nx_slice_sub(bytes, (size_t)r.name_off, (size_t)r.name_len);
        if (!valid_name(name)) goto corrupt;
        if (f == 0 && (r.type != NX_FIELD_TEXT || !nx_slice_eq(name, nx_slice_cstr("_id")))) goto corrupt;
        for (uint32_t prior = 0; prior < f; prior++) {
            field_record p = read_record(bytes, prior);
            if (nx_slice_eq(name, nx_slice_sub(bytes, (size_t)p.name_off, (size_t)p.name_len))) goto corrupt;
        }
        if (validate_column(&table, r) != NX_OK) goto corrupt;
    }
    if (next != bytes.n) goto corrupt;
    nx_slice previous = nx_slice_make(NULL, 0);
    nx_cursor ids = nx_cursor_make(nx_slice_sub(bytes, (size_t)ids_off, (size_t)ids_len));
    for (uint32_t i = 0; i < rows; i++) {
        uint32_t row = nx_rd_u32(&ids);
        if (row >= rows) goto corrupt;
        nx_slice id = nx_table_id(&table, row);
        if (!valid_name(id) || (i && slice_order(previous, id) >= 0)) goto corrupt;
        previous = id;
    }
    nx_status documents_status = validate_documents(&table);
    if (documents_status == NX_ERR_NOMEM) return failure(error, NX_ERR_NOMEM, "table document validation allocation failed");
    if (documents_status != NX_OK) goto corrupt;
    *out = table;
    return NX_OK;
corrupt:
    return failure(error, NX_ERR_CORRUPT, "table checksum, schema, offsets or cell validation failed");
}
