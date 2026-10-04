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
    fputs("NexusSearch " NEXUS_VERSION " - high-performance embedded document search\n"
          "Usage:\n"
          "  nexus build INPUT.jsonl OUTPUT.nxs\n"
          "  nexus search SNAPSHOT.nxs QUERY [--scan]\n"
          "  nexus explain SNAPSHOT.nxs QUERY\n"
          "  nexus stats SNAPSHOT.nxs\n"
          "  nexus serve SNAPSHOT.nxs [--port 8080] [--host 127.0.0.1]\n"
          "  nexus mcp SNAPSHOT.nxs\n"
          "  nexus repl SNAPSHOT.nxs\n"
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

static int repl(const nx_table *table) {
    printf("\n  ============================================================\n"
           "  NexusSearch " NEXUS_VERSION " Interactive Shell\n"
           "  Snapshot: %" PRIu32 " rows, %" PRIu32 " fields\n"
           "  Type query, or :help for commands, :quit to exit.\n"
           "  ============================================================\n\n",
           table->rows, table->fields);
    char line[4096];
    while (1) {
        printf("nexus> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = '\0';
        while (len && (line[0] == ' ' || line[0] == '\t')) {
            memmove(line, line + 1, len--);
        }
        if (!len) continue;
        if (!strcmp(line, ":q") || !strcmp(line, ":quit") || !strcmp(line, ":exit")) break;
        if (!strcmp(line, ":help")) {
            puts("\nCommands:\n"
                 "  :stats           Display schema and document counts\n"
                 "  :explain <q>     Explain query execution plan\n"
                 "  :scan <q>        Execute query in scan oracle mode\n"
                 "  :json <q>        Execute and output raw JSON result\n"
                 "  :doc <row>       Print raw document JSON for row\n"
                 "  :quit            Exit interactive shell\n"
                 "\nNexusQL Examples:\n"
                 "  title:search AND year:>=2025\n"
                 "  active:true SORT BY year DESC LIMIT 5\n"
                 "  embedding:[1,0,0]\n");
            continue;
        }
        if (!strcmp(line, ":stats")) {
            stats(table);
            continue;
        }
        if (!strncmp(line, ":doc ", 5)) {
            uint32_t r = (uint32_t)strtoul(line + 5, NULL, 10);
            if (r < table->rows) {
                nx_slice d = nx_table_document(table, r);
                printf("%.*s\n", (int)d.n, (const char *)d.p);
            } else {
                puts("Row index out of range.");
            }
            continue;
        }
        bool scan = false;
        bool explain = false;
        bool raw_json = false;
        const char *q = line;
        if (!strncmp(line, ":explain ", 9)) {
            explain = true;
            q = line + 9;
        } else if (!strncmp(line, ":scan ", 6)) {
            scan = true;
            q = line + 6;
        } else if (!strncmp(line, ":json ", 6)) {
            raw_json = true;
            q = line + 6;
        }

        nx_buf query;
        nx_buf_init(&query);
        if (explain && strncmp(q, "EXPLAIN", 7) != 0) nx_buf_put_str(&query, "EXPLAIN ");
        nx_buf_put_str(&query, q);
        nx_search_options options = nx_search_default_options();
        options.indexed = !scan;
        nx_search_result res = {0};
        nx_error err;
        nx_error_clear(&err);

        nx_status st = nx_search(table, nx_buf_slice(&query), &options, &res, &err);
        nx_buf_free(&query);

        if (st != NX_OK) {
            fprintf(stderr, "Error: %s", nx_status_str(st));
            if (err.msg[0]) fprintf(stderr, ": %s", err.msg);
            fputc('\n', stderr);
            continue;
        }

        if (raw_json || explain) {
            nx_buf jbuf;
            nx_buf_init(&jbuf);
            nx_search_json(&res, &jbuf);
            emit(&jbuf);
            nx_buf_free(&jbuf);
        } else {
            printf("Found %zu / %zu matches (work: %zu, scanned: %zu, idx: %zu, vec: %zu)\n",
                   res.count, res.total, res.work, res.scanned_cells, res.numeric_indexes, res.vectors_scored);
            puts("--------------------------------------------------------------------------------");
            for (size_t i = 0; i < res.count; ++i) {
                printf("#%-2zu [score: %7.4f] _id: %.*s (row %" PRIu32 ")\n",
                       i + 1, res.hits[i].score, (int)res.hits[i].id.n, (const char *)res.hits[i].id.p, res.hits[i].row);
                printf("    %.*s\n", (int)res.hits[i].document.n, (const char *)res.hits[i].document.p);
            }
            if (res.count == 0) {
                puts("  (No documents matched)");
            }
            puts("--------------------------------------------------------------------------------");
        }
        nx_search_result_free(&res);
    }
    return 0;
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
    bool want_repl = argc == 3 && (!strcmp(argv[1], "repl") || !strcmp(argv[1], "interactive"));
    bool want_mcp = argc == 3 && !strcmp(argv[1], "mcp");
    bool want_serve = argc >= 3 && !strcmp(argv[1], "serve");

    if (!want_stats && !want_explain && !want_search && !want_repl && !want_mcp && !want_serve) {
        usage(stderr);
        return 2;
    }

    const char *host = "127.0.0.1";
    uint16_t port = 8080;
    if (want_serve) {
        for (int i = 3; i < argc; ++i) {
            if ((!strcmp(argv[i], "--port") || !strcmp(argv[i], "-p")) && i + 1 < argc) {
                port = (uint16_t)atoi(argv[++i]);
            } else if ((!strcmp(argv[i], "--host") || !strcmp(argv[i], "-h")) && i + 1 < argc) {
                host = argv[++i];
            }
        }
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
    else if (want_repl) result = repl(&table);
    else if (want_mcp) result = nx_server_run_mcp(&table) == NX_OK ? 0 : 1;
    else if (want_serve) {
        nx_server_config cfg = {host, port, true};
        result = nx_server_run_http(&table, &cfg) == NX_OK ? 0 : 1;
    }
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
