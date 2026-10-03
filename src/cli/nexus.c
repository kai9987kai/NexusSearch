/* Small offline interface to immutable searchable snapshots. */
#include "nexus/nexus.h"
#include "core/nx_mem.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#define INPUT_LIMIT ((size_t)64 * 1024 * 1024)
#define SNAPSHOT_LIMIT ((size_t)256 * 1024 * 1024)

static void usage(FILE *stream) {
    fputs("NexusSearch " NEXUS_VERSION " - offline document search\n"
          "Usage:\n"
          "  nexus build INPUT.jsonl OUTPUT.nxs\n"
          "  nexus search SNAPSHOT.nxs QUERY [--scan]\n"
          "  nexus explain SNAPSHOT.nxs QUERY\n"
          "  nexus stats SNAPSHOT.nxs\n"
          "  nexus --help\n"
          "  nexus --version\n"
          "Quote QUERY as one shell argument. --scan selects the filter oracle.\n"
          "Build replaces OUTPUT only after complete validation.\n", stream);
}

static int report(const char *action, nx_status status, const nx_error *error) {
    fprintf(stderr, "nexus: %s: %s", action, nx_status_str(status));
    if (error && error->msg[0]) {
        if (error->pos >= 0) fprintf(stderr, " at byte %" PRId64, error->pos);
        fprintf(stderr, ": %s", error->msg);
    }
    fputc('\n', stderr);
    return 1;
}

static int emit(const nx_buf *json) {
    if ((json->len && fwrite(json->data, 1, json->len, stdout) != json->len) ||
        fputc('\n', stdout) == EOF || fflush(stdout) != 0)
        return report("write output", NX_ERR_IO, NULL);
    return 0;
}

/* Input is validated UTF-8; escape JSON syntax and every control byte. */
static void string_json(nx_buf *out, nx_slice value) {
    static const char hex[] = "0123456789abcdef";
    nx_buf_put_u8(out, '"');
    for (size_t i = 0; i < value.n; ++i) {
        uint8_t c = value.p[i];
        if (c == '"' || c == '\\') {
            nx_buf_put_u8(out, '\\');
            nx_buf_put_u8(out, c);
        } else if (c < 0x20) {
            char escaped[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            nx_buf_put(out, escaped, sizeof(escaped));
        } else nx_buf_put_u8(out, c);
    }
    nx_buf_put_u8(out, '"');
}

static const char *type_name(nx_field_type type) {
    switch (type) {
    case NX_FIELD_NULL: return "null";
    case NX_FIELD_INT: return "int";
    case NX_FIELD_FLOAT: return "float";
    case NX_FIELD_BOOL: return "bool";
    case NX_FIELD_TEXT: return "text";
    case NX_FIELD_VECTOR: return "vector";
    default: return "unknown";
    }
}

static int build(const char *input, const char *output) {
    uint8_t *data = NULL;
    size_t size = 0;
    nx_status status = nx_file_read(input, INPUT_LIMIT, &data, &size);
    if (status != NX_OK) return report("read JSONL", status, NULL);
    nx_buf bytes;
    nx_buf_init(&bytes);
    nx_error error;
    nx_error_clear(&error);
    status = nx_table_build(nx_slice_make(data, size), NULL, &bytes, &error);
    nx_free(data);
    nx_table table;
    if (status == NX_OK && bytes.len > SNAPSHOT_LIMIT) status = NX_ERR_LIMIT;
    if (status == NX_OK) status = nx_table_open(nx_buf_slice(&bytes), &table, &error);
    if (status != NX_OK) {
        nx_buf_free(&bytes);
        return report("build snapshot", status, &error);
    }
    /* Prepare the response before publishing the snapshot. */
    nx_buf response;
    nx_buf_init(&response);
    nx_buf_printf(&response, "{\"rows\":%" PRIu32 ",\"fields\":%" PRIu32
                  ",\"bytes\":%zu}", table.rows, table.fields, bytes.len);
    if (response.oom) status = NX_ERR_NOMEM;
    if (status == NX_OK) status = nx_file_write_atomic(output, bytes.data, bytes.len);
    nx_buf_free(&bytes);
    int result = status == NX_OK ? emit(&response) : report("save snapshot", status, NULL);
    nx_buf_free(&response);
    return result;
}

static int stats(const nx_table *table) {
    nx_buf json;
    nx_buf_init(&json);
    nx_buf_printf(&json, "{\"version\":\"" NEXUS_VERSION "\",\"rows\":%" PRIu32
                  ",\"fields\":[", table->rows);
    nx_status status = NX_OK;
    for (uint32_t i = 0; i < table->fields; ++i) {
        nx_field field;
        status = nx_table_field(table, i, &field);
        if (status != NX_OK) break;
        if (i) nx_buf_put_u8(&json, ',');
        nx_buf_put_str(&json, "{\"name\":");
        string_json(&json, field.name);
        nx_buf_printf(&json, ",\"type\":\"%s\"", type_name(field.type));
        if (field.type == NX_FIELD_VECTOR)
            nx_buf_printf(&json, ",\"dimensions\":%" PRIu32, field.dims);
        nx_buf_put_u8(&json, '}');
    }
    nx_buf_put_str(&json, "]}");
    if (status == NX_OK && json.oom) status = NX_ERR_NOMEM;
    int result = status == NX_OK ? emit(&json) : report("read schema", status, NULL);
    nx_buf_free(&json);
    return result;
}

static int search(const nx_table *table, const char *text, bool scan, bool explain) {
    nx_buf query;
    nx_buf_init(&query);
    if (explain) nx_buf_put_str(&query, "EXPLAIN ");
    nx_buf_put_str(&query, text);
    if (query.oom) {
        nx_buf_free(&query);
        return report("prepare query", NX_ERR_NOMEM, NULL);
    }
    nx_search_options options = nx_search_default_options();
    options.indexed = !scan;
    nx_search_result result = {0};
    nx_error error;
    nx_error_clear(&error);
    nx_status status = nx_search(table, nx_buf_slice(&query), &options, &result, &error);
    nx_buf_free(&query);
    if (status != NX_OK) return report("query", status, &error);
    nx_buf json;
    nx_buf_init(&json);
    status = nx_search_json(&result, &json);
    int exit_code = status == NX_OK ? emit(&json) : report("encode results", status, NULL);
    nx_buf_free(&json);
    nx_search_result_free(&result);
    return exit_code;
}

static int run(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "help"))) {
        usage(stdout);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts("NexusSearch " NEXUS_VERSION);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "build")) return build(argv[2], argv[3]);
    bool want_stats = argc == 3 && !strcmp(argv[1], "stats");
    bool want_explain = argc == 4 && !strcmp(argv[1], "explain");
    bool want_search = (argc == 4 || (argc == 5 && !strcmp(argv[4], "--scan"))) &&
                       !strcmp(argv[1], "search");
    if (!want_stats && !want_explain && !want_search) {
        usage(stderr);
        return 2;
    }
    nx_mmap mapping = {0};
    nx_status status = nx_mmap_open(argv[2], SNAPSHOT_LIMIT, &mapping);
    if (status != NX_OK) return report("open snapshot", status, NULL);
    nx_table table;
    nx_error error;
    nx_error_clear(&error);
    status = nx_table_open(nx_slice_make(mapping.data, mapping.size), &table, &error);
    int result;
    if (status != NX_OK) result = report("validate snapshot", status, &error);
    else if (want_stats) result = stats(&table);
    else result = search(&table, argv[3], argc == 5, want_explain);
    nx_mmap_close(&mapping);
    return result;
}

#ifdef _WIN32
/* Preserve original Unicode command-line paths instead of ANSI argv loss. */
int wmain(int argc, wchar_t **wide_argv) {
    (void)_setmode(_fileno(stdout), _O_BINARY);
    char **argv = (char **)nx_calloc((size_t)argc, sizeof(*argv));
    if (!argv) return report("prepare arguments", NX_ERR_NOMEM, NULL);
    int result = 1;
    int initialized = 0;
    for (int i = 0; i < argc; ++i) {
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1,
                                        NULL, 0, NULL, NULL);
        if (bytes <= 0) {
            result = report("decode arguments", NX_ERR_INVALID, NULL);
            goto cleanup;
        }
        argv[i] = (char *)nx_malloc((size_t)bytes);
        if (!argv[i]) {
            result = report("prepare arguments", NX_ERR_NOMEM, NULL);
            goto cleanup;
        }
        ++initialized;
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1,
                                 argv[i], bytes, NULL, NULL)) {
            result = report("decode arguments", NX_ERR_INVALID, NULL);
            goto cleanup;
        }
    }
    result = run(argc, argv);
cleanup:
    for (int i = 0; i < initialized; ++i) nx_free(argv[i]);
    nx_free(argv);
    return result;
}
#else
int main(int argc, char **argv) { return run(argc, argv); }
#endif
