#include "core/nx_config.h"
#include "nx_test.h"
#include "seg/nx_table.h"
#include "core/nx_crc32c.h"
#include <float.h>

static const char sample[] =
    " {\"i\":-9223372036854775808,\"_id\":\"z\",\"f\":0.1,\"b\":true,\"t\":\"a\\u0000b\",\"v\":[1,-2.5],\"n\":null} \r\n"
    "{\"_id\":\"a\",\"i\":9223372036854775807,\"f\":-0.0,\"b\":false,\"t\":\"\",\"v\":[3.25,0],\"late\":null}\n"
    "\n{\"_id\":\"\\u03bb\",\"i\":null,\"n\":null,\"late\":7}\n";

static bool built(const char *source, nx_buf *bytes, nx_table *table) {
    nx_buf_init(bytes);
    nx_error error;
    nx_status status = nx_table_build(nx_slice_cstr(source), NULL, bytes, &error);
    NX_CHECK_MSG(status == NX_OK, "build failed: %.30s: %.160s", nx_status_str(status), error.msg);
    if (status != NX_OK) return false;
    status = nx_table_open(nx_buf_slice(bytes), table, &error);
    NX_CHECK_MSG(status == NX_OK, "open failed: %.30s: %.160s", nx_status_str(status), error.msg);
    return status == NX_OK;
}
static uint32_t field_number(const nx_table *table, const char *name) {
    uint32_t field = UINT32_MAX;
    NX_CHECK_OK(nx_table_find(table, nx_slice_cstr(name), &field));
    return field;
}

static void test_table_roundtrip(void) {
    nx_buf bytes; nx_table table;
    if (!built(sample, &bytes, &table)) { nx_buf_free(&bytes); return; }
    NX_CHECK_EQ_U(table.rows, 3); NX_CHECK_EQ_U(table.fields, 8);
    NX_CHECK(nx_slice_eq(nx_table_id(&table, 0), nx_slice_cstr("z")));
    NX_CHECK(nx_slice_eq(nx_table_id(&table, 1), nx_slice_cstr("a")));
    NX_CHECK(nx_slice_eq(nx_table_id(&table, 2), nx_slice_cstr("\xce\xbb")));
    NX_CHECK(nx_slice_eq(nx_table_document(&table, 2), nx_slice_cstr("{\"_id\":\"\\u03bb\",\"i\":null,\"n\":null,\"late\":7}")));
    nx_field info; NX_CHECK_OK(nx_table_field(&table, 0, &info));
    NX_CHECK(nx_slice_eq(info.name, nx_slice_cstr("_id"))); NX_CHECK_EQ_I(info.type, NX_FIELD_TEXT);
    uint32_t integer = field_number(&table, "i"), real = field_number(&table, "f");
    uint32_t boolean = field_number(&table, "b"), text = field_number(&table, "t");
    uint32_t vector = field_number(&table, "v"), missing = field_number(&table, "n");
    uint32_t late = field_number(&table, "late");
    nx_cell cell;
    NX_CHECK_OK(nx_table_get(&table, 0, integer, &cell)); NX_CHECK(cell.present); NX_CHECK_EQ_I(cell.type, NX_FIELD_INT);
    NX_CHECK_EQ_I(cell.as.integer, INT64_MIN);
    NX_CHECK_OK(nx_table_get(&table, 1, integer, &cell)); NX_CHECK_EQ_I(cell.as.integer, INT64_MAX);
    NX_CHECK_OK(nx_table_get(&table, 2, integer, &cell)); NX_CHECK(!cell.present); NX_CHECK_EQ_I(cell.type, NX_FIELD_INT);
    NX_CHECK_OK(nx_table_get(&table, 0, real, &cell)); NX_CHECK_NEAR(cell.as.real, 0.1, 0.0);
    NX_CHECK_OK(nx_table_get(&table, 1, real, &cell));
    uint64_t zero_bits; memcpy(&zero_bits, &cell.as.real, sizeof zero_bits);
    NX_CHECK_EQ_U(zero_bits, UINT64_C(0x8000000000000000));
    NX_CHECK_OK(nx_table_get(&table, 0, boolean, &cell)); NX_CHECK(cell.present && cell.as.boolean);
    NX_CHECK_OK(nx_table_get(&table, 1, boolean, &cell)); NX_CHECK(cell.present && !cell.as.boolean);
    NX_CHECK_OK(nx_table_get(&table, 0, text, &cell)); NX_CHECK(nx_slice_eq(cell.as.text, nx_slice_make("a\0b", 3)));
    NX_CHECK_OK(nx_table_get(&table, 1, text, &cell)); NX_CHECK(cell.present && cell.as.text.n == 0);
    NX_CHECK_OK(nx_table_get(&table, 2, text, &cell)); NX_CHECK(!cell.present);
    NX_CHECK_OK(nx_table_field(&table, vector, &info)); NX_CHECK_EQ_U(info.dims, 2);
    NX_CHECK_OK(nx_table_get(&table, 0, vector, &cell));
    NX_CHECK_NEAR(nx_cell_vector_at(&cell, 0), 1, 0); NX_CHECK_NEAR(nx_cell_vector_at(&cell, 1), -2.5, 0);
    NX_CHECK_NEAR(nx_cell_vector_at(&cell, 2), 0, 0);
    NX_CHECK_OK(nx_table_get(&table, 2, vector, &cell)); NX_CHECK(!cell.present); NX_CHECK_NEAR(nx_cell_vector_at(&cell, 0), 0, 0);
    NX_CHECK_OK(nx_table_field(&table, missing, &info)); NX_CHECK_EQ_I(info.type, NX_FIELD_NULL);
    NX_CHECK_OK(nx_table_get(&table, 0, missing, &cell)); NX_CHECK(!cell.present);
    NX_CHECK_OK(nx_table_get(&table, 2, late, &cell)); NX_CHECK(cell.present); NX_CHECK_EQ_I(cell.as.integer, 7);
    nx_bsi index; NX_CHECK_OK(nx_table_int_index(&table, integer, &index));
    int64_t indexed = 0; bool present = false;
    NX_CHECK_OK(nx_bsi_get(&index, 0, &indexed, &present)); NX_CHECK(present); NX_CHECK_EQ_I(indexed, INT64_MIN);
    NX_CHECK_OK(nx_bsi_get(&index, 2, &indexed, &present)); NX_CHECK(!present);
    NX_CHECK_EQ_I(nx_table_int_index(&table, text, &index), NX_ERR_TYPE);
    NX_CHECK_EQ_I(nx_table_get(&table, 3, 0, &cell), NX_ERR_INVALID); NX_CHECK(!cell.present);
    NX_CHECK_EQ_I(nx_table_field(&table, table.fields, &info), NX_ERR_INVALID);
    uint32_t not_found = 0;
    NX_CHECK_EQ_I(nx_table_find(&table, nx_slice_cstr("absent"), &not_found), NX_ERR_NOT_FOUND);
    NX_CHECK_EQ_U(not_found, UINT32_MAX);
    NX_CHECK_EQ_U(nx_table_id(&table, table.rows).n, 0); NX_CHECK_EQ_U(nx_table_document(&table, table.rows).n, 0);
    /* The borrowed view need not be SIMD- or naturally-aligned. */
    nx_buf shifted; nx_buf_init(&shifted); nx_buf_put_u8(&shifted, 9); nx_buf_put(&shifted, bytes.data, bytes.len);
    nx_table unaligned; NX_CHECK_OK(nx_table_open(nx_slice_make(shifted.data + 1, bytes.len), &unaligned, NULL));
    NX_CHECK_OK(nx_table_get(&unaligned, 1, integer, &cell)); NX_CHECK_EQ_I(cell.as.integer, INT64_MAX);
    nx_buf_free(&shifted); nx_buf_free(&bytes);
}

static void test_table_empty_and_limits(void) {
    const char *empty[] = {"", " \t\r\n\r\n "};
    for (size_t i = 0; i < NX_ARRAY_LEN(empty); i++) {
        nx_buf bytes; nx_table table;
        if (built(empty[i], &bytes, &table)) {
            NX_CHECK_EQ_U(table.rows, 0); NX_CHECK_EQ_U(table.fields, 0); NX_CHECK_EQ_U(bytes.len, 128);
        }
        nx_buf_free(&bytes);
    }
    nx_buf bytes; nx_buf_init(&bytes);
    nx_table_limits zero = {0};
    NX_CHECK_OK(nx_table_build(nx_slice_make(NULL, 0), &zero, &bytes, NULL));
    nx_buf_clear(&bytes);
    const char input[] = "{\"_id\":\"x\",\"v\":[1,2]}\n{\"_id\":\"y\"}";
    nx_table_limits limits = nx_table_default_limits();
    limits.max_input_bytes = sizeof input - 2;
    NX_CHECK_EQ_I(nx_table_build(nx_slice_cstr(input), &limits, &bytes, NULL), NX_ERR_LIMIT);
    limits = nx_table_default_limits(); limits.max_rows = 1;
    NX_CHECK_EQ_I(nx_table_build(nx_slice_cstr(input), &limits, &bytes, NULL), NX_ERR_LIMIT);
    limits = nx_table_default_limits(); limits.max_fields = 1;
    NX_CHECK_EQ_I(nx_table_build(nx_slice_cstr(input), &limits, &bytes, NULL), NX_ERR_LIMIT);
    limits = nx_table_default_limits(); limits.max_field_bytes = 2;
    NX_CHECK_EQ_I(nx_table_build(nx_slice_cstr(input), &limits, &bytes, NULL), NX_ERR_LIMIT);
    limits = nx_table_default_limits(); limits.max_vector_dims = 1;
    NX_CHECK_EQ_I(nx_table_build(nx_slice_cstr(input), &limits, &bytes, NULL), NX_ERR_LIMIT);
    limits = nx_table_default_limits(); limits.max_fields = SIZE_MAX;
    NX_CHECK_EQ_I(nx_table_build(nx_slice_cstr(input), &limits, &bytes, NULL), NX_ERR_LIMIT);
    NX_CHECK_EQ_U(bytes.len, 0);
    nx_buf_free(&bytes);
}

static void test_table_schema_rejections(void) {
    struct bad_case { const char *source; nx_status status; } cases[] = {
        {"{}", NX_ERR_INVALID}, {"[]", NX_ERR_TYPE}, {"null", NX_ERR_TYPE},
        {"{\"_id\":null}", NX_ERR_INVALID}, {"{\"_id\":3}", NX_ERR_INVALID},
        {"{\"_id\":\"\"}", NX_ERR_INVALID}, {"{\"_id\":\"a\\u0000\"}", NX_ERR_INVALID},
        {"{\"_id\":\"a\"}\n{\"_id\":\"a\"}", NX_ERR_EXISTS},
        {"{\"_id\":\"a\",\"x\":1}\n{\"_id\":\"b\",\"x\":1.0}", NX_ERR_TYPE},
        {"{\"_id\":\"a\",\"x\":true}\n{\"_id\":\"b\",\"x\":1}", NX_ERR_TYPE},
        {"{\"_id\":\"a\",\"x\":{}}", NX_ERR_UNSUPPORTED},
        {"{\"_id\":\"a\",\"x\":[]}", NX_ERR_TYPE},
        {"{\"_id\":\"a\",\"x\":[true]}", NX_ERR_TYPE},
        {"{\"_id\":\"a\",\"x\":[1]}\n{\"_id\":\"b\",\"x\":[1,2]}", NX_ERR_TYPE},
        {"{\"_id\":\"a\",\"x\":9223372036854775808}", NX_ERR_TYPE},
        {"{\"_id\":\"a\",\"x\":-9223372036854775809}", NX_ERR_TYPE},
        {"{\"_id\":\"a\",\"x\":1e999}", NX_ERR_TYPE},
        {"{\"_id\":\"a\",\"x\":[1e39]}", NX_ERR_TYPE},
        {"{\"_id\":\"a\",\"\":0}", NX_ERR_INVALID},
        {"{\"_id\":\"a\",\"x\\u0000\":0}", NX_ERR_INVALID},
        {"{\"_id\":\"a\",\"x\":1,\"x\":2}", NX_ERR_PARSE},
        {"{\"_id\":\"\xc0\xaf\"}", NX_ERR_PARSE},
        {"{\"_id\":\"a\"}\n{", NX_ERR_PARSE}
    };
    for (size_t i = 0; i < NX_ARRAY_LEN(cases); i++) {
        nx_buf bytes; nx_buf_init(&bytes); nx_buf_put_str(&bytes, "sentinel");
        nx_error error;
        nx_status status = nx_table_build(nx_slice_cstr(cases[i].source), NULL, &bytes, &error);
        NX_CHECK_MSG(status == cases[i].status, "case %zu: got %.25s, expected %.25s: %.140s", i,
                     nx_status_str(status), nx_status_str(cases[i].status), error.msg);
        NX_CHECK_EQ_U(bytes.len, 8); NX_CHECK(memcmp(bytes.data, "sentinel", 8) == 0);
        NX_CHECK(error.code == status && error.msg[0] != 0);
        nx_buf_free(&bytes);
    }
}

/* Format-specific tests intentionally refresh checksums: validation must still
 * reject inconsistent offsets, schema, cells and integer indexes. */
static void refresh_crc(uint8_t *data, size_t size, size_t original_data_off) {
    nx_st32(data + 104, 0);
    nx_st32(data + 108, nx_crc32c(0, data + 128, original_data_off - 128));
    nx_st32(data + 112, nx_crc32c(0, data + original_data_off, size - original_data_off));
    nx_st32(data + 104, nx_crc32c(0, data, 128));
}
static size_t record_u64(const nx_buf *bytes, uint32_t field, size_t member) {
    return (size_t)nx_ld64(bytes->data + 128 + (size_t)field * 80 + member);
}
static void check_rejected(nx_slice bytes) {
    nx_table table = {nx_slice_cstr("not cleared"), 9, 9}; nx_error error;
    nx_status status = nx_table_open(bytes, &table, &error);
    NX_CHECK(status == NX_ERR_CORRUPT || status == NX_ERR_VERSION);
    NX_CHECK(table.bytes.p == NULL && table.bytes.n == 0 && table.rows == 0 && table.fields == 0);
    NX_CHECK(error.code == status && error.msg[0] != 0);
}
static void test_table_corruption(void) {
    nx_buf bytes; nx_table table;
    if (!built(sample, &bytes, &table)) { nx_buf_free(&bytes); return; }
    for (size_t length = 0; length < bytes.len; length++) check_rejected(nx_slice_make(bytes.data, length));
    uint8_t *mutated = nx_malloc(bytes.len + 1);
    if (!mutated) { nx_buf_free(&bytes); NX_CHECK(false); return; }
    nx_rng rng = nx_test_rng(824);
    for (size_t i = 0; i < nx_test_iters(1200); i++) {
        memcpy(mutated, bytes.data, bytes.len);
        mutated[nx_rng_below(&rng, bytes.len)] ^= (uint8_t)(1u << nx_rng_below(&rng, 8));
        check_rejected(nx_slice_make(mutated, bytes.len));
    }
    memcpy(mutated, bytes.data, bytes.len); mutated[bytes.len] = 0;
    check_rejected(nx_slice_make(mutated, bytes.len + 1));
    size_t data_off = (size_t)nx_ld64(bytes.data + 80);
    uint32_t integer = field_number(&table, "i"), real = field_number(&table, "f");
    uint32_t text = field_number(&table, "t"), vector = field_number(&table, "v");
    uint32_t missing = field_number(&table, "n"), boolean = field_number(&table, "b");
    for (unsigned corruption = 0; corruption < 20; corruption++) {
        memcpy(mutated, bytes.data, bytes.len);
        size_t off;
        switch (corruption) {
        case 0: nx_st64(mutated + 128 + (size_t)integer * 80 + 24, UINT64_MAX); break;
        case 1: nx_st64(mutated + 128 + (size_t)integer * 80 + 32, 0); break;
        case 2: nx_st32(mutated + 128 + (size_t)integer * 80 + 16, UINT32_MAX); break;
        case 3: nx_st32(mutated + 128 + (size_t)vector * 80 + 20, 0); break;
        case 4: nx_st64(mutated + 128 + (size_t)integer * 80 + 72, 1); break;
        case 5: mutated[record_u64(&bytes, integer, 24)] = 2; break;
        case 6: nx_st64(mutated + record_u64(&bytes, real, 24) + table.rows, UINT64_C(0x7ff8000000000000)); break;
        case 7: nx_st32(mutated + record_u64(&bytes, vector, 24) + table.rows, UINT32_C(0x7f800000)); break;
        case 8: mutated[record_u64(&bytes, missing, 24)] = 1; break;
        case 9: mutated[record_u64(&bytes, boolean, 24) + table.rows] = 2; break;
        case 10: nx_st64(mutated + record_u64(&bytes, text, 24) + table.rows + 8, UINT64_MAX); break;
        case 11: mutated[record_u64(&bytes, integer, 0)] = 'f'; break; /* duplicate field name */
        case 12: mutated[record_u64(&bytes, 0, 40)] = 'a'; break; /* duplicate ID */
        case 13: mutated[data_off] = '/'; break;
        case 14: off = (size_t)nx_ld64(bytes.data + 64); nx_st32(mutated + off, table.rows); break;
        case 15: mutated[record_u64(&bytes, integer, 24) + table.rows] ^= 1; break; /* BSI mismatch */
        case 16: {
            off = record_u64(&bytes, integer, 56); size_t len = record_u64(&bytes, integer, 64);
            mutated[off + 40] ^= 1; /* mutate lowest value plane; repair its own checksum too */
            uint32_t crc = nx_crc32c(0, mutated + off, 28);
            crc = nx_crc32c(crc, mutated + off + 32, len - 32); nx_st32(mutated + off + 28, crc);
            break;
        }
        case 17: mutated[data_off + 4] = '?'; break; /* interior JSON syntax */
        case 18: mutated[data_off + 6] = '8'; break; /* valid JSON, contradictory exact integer */
        default: {
            /* Source document row zero starts with {"i":..., then _id. */
            const char marker[] = "\"_id\":\"z\"";
            for (size_t i = data_off; i + sizeof marker - 1 <= bytes.len; i++) {
                if (memcmp(mutated + i, marker, sizeof marker - 1) == 0) {
                    mutated[i + sizeof marker - 3] = 'y'; break;
                }
            }
            break;
        }
        }
        refresh_crc(mutated, bytes.len, data_off);
        check_rejected(nx_slice_make(mutated, bytes.len));
    }
    for (size_t i = 0; i < nx_test_iters(1200); i++) {
        memcpy(mutated, bytes.data, bytes.len);
        mutated[nx_rng_below(&rng, bytes.len)] ^= (uint8_t)(1u << nx_rng_below(&rng, 8));
        refresh_crc(mutated, bytes.len, data_off);
        nx_table opened;
        nx_status status = nx_table_open(nx_slice_make(mutated, bytes.len), &opened, NULL);
        NX_CHECK(status == NX_OK || status == NX_ERR_CORRUPT || status == NX_ERR_VERSION);
        if (status == NX_OK) {
            for (uint32_t row = 0; row < opened.rows; row++) {
                NX_CHECK(nx_table_id(&opened, row).n != 0);
                for (uint32_t f = 0; f < opened.fields; f++) {
                    nx_cell cell; NX_CHECK_OK(nx_table_get(&opened, row, f, &cell));
                }
            }
        }
    }
    nx_free(mutated); nx_buf_free(&bytes);
}

static void test_table_integer_oracle(void) {
    nx_rng rng = nx_test_rng(4126);
    for (size_t trial = 0; trial < nx_test_iters(45); trial++) {
        enum { ROWS = 80 };
        int64_t values[ROWS]; bool present[ROWS];
        nx_buf source, bytes; nx_buf_init(&source); nx_buf_init(&bytes);
        for (size_t row = 0; row < ROWS; row++) {
            uint64_t raw = nx_rng_u64(&rng); memcpy(&values[row], &raw, 8);
            present[row] = nx_rng_below(&rng, 4) != 0;
            nx_buf_printf(&source, "{\"_id\":\"row%zu\",\"i\":", row);
            if (present[row]) nx_buf_printf(&source, "%lld", (long long)values[row]);
            else nx_buf_put_str(&source, "null");
            nx_buf_put_str(&source, "}\n");
        }
        nx_table table;
        nx_status status = nx_table_build(nx_buf_slice(&source), NULL, &bytes, NULL);
        NX_CHECK_OK(status);
        if (status == NX_OK) {
            NX_CHECK_OK(nx_table_open(nx_buf_slice(&bytes), &table, NULL));
            uint32_t field = field_number(&table, "i"); nx_bsi index;
            NX_CHECK_OK(nx_table_int_index(&table, field, &index));
            for (uint32_t row = 0; row < ROWS; row++) {
                nx_cell cell; NX_CHECK_OK(nx_table_get(&table, row, field, &cell));
                NX_CHECK(cell.present == present[row]);
                if (present[row]) NX_CHECK_EQ_I(cell.as.integer, values[row]);
                int64_t actual; bool exists;
                NX_CHECK_OK(nx_bsi_get(&index, row, &actual, &exists));
                NX_CHECK(exists == present[row]);
                if (exists) NX_CHECK_EQ_I(actual, values[row]);
            }
        }
        nx_buf_free(&source); nx_buf_free(&bytes);
    }
}

static nx_status table_oom_scenario(void *context) {
    (void)context;
    nx_buf destination; nx_buf_init(&destination); nx_buf_put_str(&destination, "sentinel");
    if (destination.oom) { nx_buf_free(&destination); return NX_ERR_NOMEM; }
    nx_status status = nx_table_build(nx_slice_cstr(sample), NULL, &destination, NULL);
    if (status != NX_OK) {
        NX_CHECK_EQ_U(destination.len, 8); NX_CHECK(memcmp(destination.data, "sentinel", 8) == 0);
    } else {
        nx_table table;
        status = nx_table_open(nx_slice_make(destination.data + 8, destination.len - 8), &table, NULL);
    }
    nx_buf_free(&destination);
    return status;
}
static nx_status open_oom_scenario(void *context) {
    nx_buf *bytes = context;
    nx_table table = {nx_slice_cstr("must clear"), 1, 1};
    nx_status status = nx_table_open(nx_buf_slice(bytes), &table, NULL);
    if (status != NX_OK) NX_CHECK(table.bytes.p == NULL && table.rows == 0 && table.fields == 0);
    return status;
}
static void test_table_oom_and_no_alloc_accessors(void) {
    nx_test_oom_sweep(table_oom_scenario, NULL, 300);
    nx_buf bytes; nx_table table;
    if (!built(sample, &bytes, &table)) { nx_buf_free(&bytes); return; }
    nx_test_oom_sweep(open_oom_scenario, &bytes, 200);
    uint64_t before = nx_mem_alloc_counter(); nx_mem_fail_after(0);
    nx_cell cell; NX_CHECK_OK(nx_table_get(&table, 0, field_number(&table, "i"), &cell));
    NX_CHECK_EQ_I(cell.as.integer, INT64_MIN);
    nx_mem_fail_after(-1); NX_CHECK_EQ_U(nx_mem_alloc_counter(), before);
    nx_buf_free(&bytes);
}

typedef struct reader_context { const nx_table *table; uint32_t field; bool success; } reader_context;
static void read_repeatedly(void *argument) {
    reader_context *context = argument;
    context->success = true;
    for (size_t iteration = 0; iteration < 10000; iteration++) {
        nx_cell cell;
        if (nx_table_get(context->table, 0, context->field, &cell) != NX_OK
            || !cell.present || cell.as.integer != INT64_MIN
            || !nx_slice_eq(nx_table_id(context->table, 1), nx_slice_cstr("a"))) {
            context->success = false; return;
        }
    }
}
static void test_table_concurrent_readers(void) {
    nx_buf bytes; nx_table table;
    if (!built(sample, &bytes, &table)) { nx_buf_free(&bytes); return; }
    nx_thread threads[4]; reader_context contexts[4]; bool started[4];
    uint32_t field = field_number(&table, "i");
    for (size_t i = 0; i < 4; i++) {
        contexts[i].table = &table; contexts[i].field = field; contexts[i].success = false;
        started[i] = nx_thread_start(&threads[i], read_repeatedly, &contexts[i]); NX_CHECK(started[i]);
    }
    for (size_t i = 0; i < 4; i++) {
        if (started[i]) nx_thread_join(&threads[i]);
        NX_CHECK(contexts[i].success);
    }
    nx_buf_free(&bytes);
}

int main(void) {
    NX_RUN(test_table_roundtrip);
    NX_RUN(test_table_empty_and_limits);
    NX_RUN(test_table_schema_rejections);
    NX_RUN(test_table_corruption);
    NX_RUN(test_table_integer_oracle);
    NX_RUN(test_table_oom_and_no_alloc_accessors);
    NX_RUN(test_table_concurrent_readers);
    return nx_test_summary();
}
