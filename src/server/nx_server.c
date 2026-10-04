#include "server/nx_server.h"
#include "server/nx_web_ui.h"
#include "core/nx_mem.h"
#include "core/nx_json.h"
#include "core/nx_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET nx_socket_t;
#define NX_INVALID_SOCKET INVALID_SOCKET
#define NX_CLOSESOCKET closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int nx_socket_t;
#define NX_INVALID_SOCKET (-1)
#define NX_CLOSESOCKET close
#endif

#define HTTP_REQ_MAX (64u * 1024u)
#define HTTP_LINE_MAX 8192u

static volatile sig_atomic_t g_server_stop = 0;

static void on_sigint(int sig) {
    (void)sig;
    g_server_stop = 1;
}

static const char *field_type_str(nx_field_type type) {
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

static void escape_json_str(nx_buf *out, const char *p, size_t n) {
    static const char hex[] = "0123456789abcdef";
    nx_buf_put_u8(out, '"');
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = (uint8_t)p[i];
        if (c == '"' || c == '\\') {
            nx_buf_put_u8(out, '\\');
            nx_buf_put_u8(out, c);
        } else if (c == '\n') {
            nx_buf_put_str(out, "\\n");
        } else if (c == '\r') {
            nx_buf_put_str(out, "\\r");
        } else if (c == '\t') {
            nx_buf_put_str(out, "\\t");
        } else if (c < 0x20) {
            char esc[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            nx_buf_put(out, esc, sizeof(esc));
        } else {
            nx_buf_put_u8(out, c);
        }
    }
    nx_buf_put_u8(out, '"');
}

nx_status nx_server_stats_json(const nx_table *table, nx_buf *out) {
    if (!table || !out) return NX_ERR_INVALID;
    nx_buf_printf(out, "{\"version\":\"0.2.0\",\"rows\":%" PRIu32 ",\"fields\":[", table->rows);
    for (uint32_t i = 0; i < table->fields; ++i) {
        nx_field field;
        nx_status status = nx_table_field(table, i, &field);
        if (status != NX_OK) return status;
        if (i) nx_buf_put_u8(out, ',');
        nx_buf_put_str(out, "{\"name\":");
        escape_json_str(out, (const char *)field.name.p, field.name.n);
        nx_buf_printf(out, ",\"type\":\"%s\"", field_type_str(field.type));
        if (field.type == NX_FIELD_VECTOR) {
            nx_buf_printf(out, ",\"dimensions\":%" PRIu32, field.dims);
        }
        nx_buf_put_u8(out, '}');
    }
    nx_buf_put_str(out, "]}");
    return out->oom ? NX_ERR_NOMEM : NX_OK;
}

nx_status nx_server_query_json(const nx_table *table, const char *query_str,
                              bool scan, bool explain, nx_buf *out) {
    if (!table || !query_str || !out) return NX_ERR_INVALID;
    nx_buf qbuf;
    nx_buf_init(&qbuf);
    if (explain && strncmp(query_str, "EXPLAIN", 7) != 0) {
        nx_buf_put_str(&qbuf, "EXPLAIN ");
    }
    nx_buf_put_str(&qbuf, query_str);
    if (qbuf.oom) {
        nx_buf_free(&qbuf);
        return NX_ERR_NOMEM;
    }

    nx_search_options options = nx_search_default_options();
    options.indexed = !scan;
    nx_search_result result = {0};
    nx_error error;
    nx_error_clear(&error);

    nx_status status = nx_search(table, nx_buf_slice(&qbuf), &options, &result, &error);
    nx_buf_free(&qbuf);

    if (status != NX_OK) {
        nx_buf_printf(out, "{\"error\":\"%s", nx_status_str(status));
        if (error.msg[0]) {
            nx_buf_printf(out, ": %s", error.msg);
        }
        nx_buf_put_str(out, "\"}");
        return NX_OK;
    }

    status = nx_search_json(&result, out);
    nx_search_result_free(&result);
    return status;
}

static void url_decode(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    size_t d = 0;
    for (size_t i = 0; i < src_len && d + 1 < dst_cap; ++i) {
        if (src[i] == '+' ) {
            dst[d++] = ' ';
        } else if (src[i] == '%' && i + 2 < src_len) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            char *end = NULL;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                dst[d++] = (char)val;
                i += 2;
            } else {
                dst[d++] = src[i];
            }
        } else {
            dst[d++] = src[i];
        }
    }
    if (dst_cap > 0) dst[d] = '\0';
}

static void send_http_response(nx_socket_t sock, int status_code, const char *status_text,
                               const char *content_type, const char *body, size_t body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                        "Access-Control-Allow-Headers: Content-Type\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        status_code, status_text, content_type, body_len);
    if (hlen > 0) {
        (void)send(sock, header, hlen, 0);
    }
    if (body && body_len > 0) {
        (void)send(sock, body, (int)body_len, 0);
    }
}

static void send_sse_headers(nx_socket_t sock) {
    const char headers[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "\r\n";
    (void)send(sock, headers, (int)strlen(headers), 0);
}

static void send_sse_event(nx_socket_t sock, const char *event_name, const char *data, size_t data_len) {
    char buf[128];
    int elen = snprintf(buf, sizeof(buf), "event: %s\ndata: ", event_name);
    if (elen > 0) (void)send(sock, buf, elen, 0);
    if (data && data_len > 0) (void)send(sock, data, (int)data_len, 0);
    (void)send(sock, "\n\n", 2, 0);
}

static void handle_http_client(const nx_table *table, nx_socket_t client) {
    char req[HTTP_REQ_MAX];
    int recvd = 0;
    while (recvd < (int)(sizeof(req) - 1)) {
        int n = recv(client, req + recvd, (int)(sizeof(req) - 1 - (size_t)recvd), 0);
        if (n <= 0) break;
        recvd += n;
        req[recvd] = '\0';
        char *header_end = strstr(req, "\r\n\r\n");
        if (header_end) {
            char *cl = strstr(req, "Content-Length:");
            if (!cl) cl = strstr(req, "content-length:");
            if (cl) {
                int content_len = atoi(cl + 15);
                size_t body_so_far = (size_t)(req + recvd - (header_end + 4));
                if (body_so_far >= (size_t)content_len) break;
            } else {
                break;
            }
        }
    }
    if (recvd <= 0) {
        NX_CLOSESOCKET(client);
        return;
    }
    req[recvd] = '\0';

    char method[16] = {0};
    char path[2048] = {0};
    if (sscanf(req, "%15s %2047s", method, path) != 2) {
        send_http_response(client, 400, "Bad Request", "application/json", "{\"error\":\"bad request\"}", 22);
        NX_CLOSESOCKET(client);
        return;
    }

    if (!strcmp(method, "OPTIONS")) {
        send_http_response(client, 204, "No Content", "text/plain", "", 0);
        NX_CLOSESOCKET(client);
        return;
    }

    char *query_part = strchr(path, '?');
    if (query_part) {
        *query_part = '\0';
        query_part++;
    }

    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        send_http_response(client, 200, "OK", "text/html; charset=utf-8",
                           (const char *)nx_web_ui_html, nx_web_ui_html_len);
    } else if (!strcmp(path, "/health")) {
        const char msg[] = "{\"status\":\"ok\",\"version\":\"0.2.0\"}";
        send_http_response(client, 200, "OK", "application/json; charset=utf-8", msg, strlen(msg));
    } else if (!strcmp(path, "/api/stats")) {
        nx_buf json;
        nx_buf_init(&json);
        nx_status st = nx_server_stats_json(table, &json);
        if (st == NX_OK && json.data) {
            send_http_response(client, 200, "OK", "application/json; charset=utf-8",
                               (const char *)json.data, json.len);
        } else {
            send_http_response(client, 500, "Internal Error", "application/json", "{\"error\":\"failed\"}", 18);
        }
        nx_buf_free(&json);
    } else if (!strcmp(path, "/api/search") || !strcmp(path, "/api/explain")) {
        bool is_explain = !strcmp(path, "/api/explain");
        char q_decoded[2048] = {0};
        bool scan = false;

        if (!strcmp(method, "GET") && query_part) {
            char *param = query_part;
            while (param && *param) {
                char *next = strchr(param, '&');
                if (next) *next = '\0';
                if (!strncmp(param, "q=", 2)) {
                    url_decode(q_decoded, sizeof(q_decoded), param + 2, strlen(param + 2));
                } else if (!strncmp(param, "scan=1", 6) || !strncmp(param, "scan=true", 9)) {
                    scan = true;
                }
                param = next ? next + 1 : NULL;
            }
        } else if (!strcmp(method, "POST")) {
            char *body = strstr(req, "\r\n\r\n");
            if (body) {
                body += 4;
                char *q_key = strstr(body, "\"query\":");
                if (q_key) {
                    char *val_start = strchr(q_key + 8, '"');
                    if (val_start) {
                        val_start++;
                        char *val_end = strchr(val_start, '"');
                        if (val_end) {
                            size_t qlen = (size_t)(val_end - val_start);
                            if (qlen < sizeof(q_decoded)) {
                                memcpy(q_decoded, val_start, qlen);
                                q_decoded[qlen] = '\0';
                            }
                        }
                    }
                }
                if (strstr(body, "\"scan\":true") || strstr(body, "\"scan\": 1")) {
                    scan = true;
                }
            }
        }

        if (!q_decoded[0]) {
            send_http_response(client, 400, "Bad Request", "application/json",
                               "{\"error\":\"missing query parameter 'q'\"}", 37);
        } else {
            nx_buf res;
            nx_buf_init(&res);
            (void)nx_server_query_json(table, q_decoded, scan, is_explain, &res);
            send_http_response(client, 200, "OK", "application/json; charset=utf-8",
                               res.data ? (const char *)res.data : "{}", res.len ? res.len : 2);
            nx_buf_free(&res);
        }
    } else if (!strcmp(path, "/api/stream")) {
        char q_decoded[2048] = {0};
        bool scan = false;

        if (!strcmp(method, "GET") && query_part) {
            char *param = query_part;
            while (param && *param) {
                char *next = strchr(param, '&');
                if (next) *next = '\0';
                if (!strncmp(param, "q=", 2)) {
                    url_decode(q_decoded, sizeof(q_decoded), param + 2, strlen(param + 2));
                } else if (!strncmp(param, "scan=1", 6) || !strncmp(param, "scan=true", 9)) {
                    scan = true;
                }
                param = next ? next + 1 : NULL;
            }
        } else if (!strcmp(method, "POST")) {
            char *body = strstr(req, "\r\n\r\n");
            if (body) {
                body += 4;
                char *q_key = strstr(body, "\"query\":");
                if (q_key) {
                    char *val_start = strchr(q_key + 8, '"');
                    if (val_start) {
                        val_start++;
                        char *val_end = strchr(val_start, '"');
                        if (val_end) {
                            size_t qlen = (size_t)(val_end - val_start);
                            if (qlen < sizeof(q_decoded)) {
                                memcpy(q_decoded, val_start, qlen);
                                q_decoded[qlen] = '\0';
                            }
                        }
                    }
                }
                if (strstr(body, "\"scan\":true") || strstr(body, "\"scan\": 1")) {
                    scan = true;
                }
            }
        }

        if (!q_decoded[0]) {
            send_http_response(client, 400, "Bad Request", "application/json",
                               "{\"error\":\"missing query parameter 'q'\"}", 37);
        } else {
            send_sse_headers(client);
            nx_buf qbuf;
            nx_buf_init(&qbuf);
            nx_buf_put_str(&qbuf, q_decoded);
            nx_search_options options = nx_search_default_options();
            options.indexed = !scan;
            nx_search_result result = {0};
            nx_error error;
            nx_error_clear(&error);

            nx_status status = nx_search(table, nx_buf_slice(&qbuf), &options, &result, &error);
            nx_buf_free(&qbuf);

            if (status != NX_OK) {
                char errbuf[256];
                int elen = snprintf(errbuf, sizeof(errbuf), "{\"error\":\"%s\"}", nx_status_str(status));
                send_sse_event(client, "error", errbuf, (size_t)elen);
            } else {
                char startbuf[128];
                int slen = snprintf(startbuf, sizeof(startbuf), "{\"hits\":%zu,\"total\":%zu}", result.count, result.total);
                send_sse_event(client, "start", startbuf, (size_t)slen);

                for (size_t i = 0; i < result.count; i++) {
                    nx_search_hit *h = &result.hits[i];
                    nx_buf hit_buf;
                    nx_buf_init(&hit_buf);
                    nx_buf_printf(&hit_buf, "{\"row\":%" PRIu32 ",\"score\":%.6f,\"id\":", h->row, h->score);
                    escape_json_str(&hit_buf, (const char *)h->id.p, h->id.n);
                    nx_buf_put_str(&hit_buf, ",\"document\":");
                    nx_buf_put(&hit_buf, h->document.p, h->document.n);
                    nx_buf_put_u8(&hit_buf, '}');
                    send_sse_event(client, "hit", (const char *)hit_buf.data, hit_buf.len);
                    nx_buf_free(&hit_buf);
                }

                char donebuf[128];
                int dlen = snprintf(donebuf, sizeof(donebuf), "{\"count\":%zu}", result.count);
                send_sse_event(client, "done", donebuf, (size_t)dlen);
                nx_search_result_free(&result);
            }
        }
    } else if (!strcmp(path, "/api/doc")) {
        uint32_t row = UINT32_MAX;
        if (query_part) {
            if (!strncmp(query_part, "row=", 4)) {
                row = (uint32_t)strtoul(query_part + 4, NULL, 10);
            }
        }
        if (row < table->rows) {
            nx_slice doc = nx_table_document(table, row);
            send_http_response(client, 200, "OK", "application/json; charset=utf-8",
                               (const char *)doc.p, doc.n);
        } else {
            send_http_response(client, 404, "Not Found", "application/json", "{\"error\":\"doc not found\"}", 23);
        }
    } else {
        send_http_response(client, 404, "Not Found", "application/json", "{\"error\":\"not found\"}", 21);
    }

#ifdef _WIN32
    shutdown(client, SD_SEND);
    char drain[256];
    while (recv(client, drain, sizeof(drain), 0) > 0) {}
#endif
    NX_CLOSESOCKET(client);
}


nx_status nx_server_run_http(const nx_table *table, const nx_server_config *config) {
    if (!table) return NX_ERR_INVALID;
    const char *host = (config && config->host) ? config->host : "127.0.0.1";
    uint16_t port = (config && config->port) ? config->port : 8080;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NX_ERR_IO;
#endif

    nx_socket_t server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == NX_INVALID_SOCKET) {
#ifdef _WIN32
        WSACleanup();
#endif
        return NX_ERR_IO;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr(host);
#else
    inet_pton(AF_INET, host, &addr.sin_addr);
#endif

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        NX_CLOSESOCKET(server);
#ifdef _WIN32
        WSACleanup();
#endif
        return NX_ERR_IO;
    }

    if (listen(server, 64) != 0) {
        NX_CLOSESOCKET(server);
#ifdef _WIN32
        WSACleanup();
#endif
        return NX_ERR_IO;
    }

    g_server_stop = 0;
    signal(SIGINT, on_sigint);

    fprintf(stderr, "\n  ============================================================\n"
                    "  NexusSearch 0.2 HTTP & Web UI Server\n"
                    "  Listening on: http://%s:%u\n"
                    "  Snapshot:     %" PRIu32 " rows, %" PRIu32 " fields\n"
                    "  Press Ctrl+C to stop.\n"
                    "  ============================================================\n\n",
            host, (unsigned)port, table->rows, table->fields);

    while (!g_server_stop) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server, &read_fds);
        struct timeval tv = {1, 0}; /* 1 second timeout for interrupt checking */
        int sel = select((int)server + 1, &read_fds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(server, &read_fds)) {
            struct sockaddr_in client_addr;
#ifdef _WIN32
            int client_len = sizeof(client_addr);
#else
            socklen_t client_len = sizeof(client_addr);
#endif
            nx_socket_t client = accept(server, (struct sockaddr *)&client_addr, &client_len);
            if (client != NX_INVALID_SOCKET) {
                handle_http_client(table, client);
            }
        }
    }

    fprintf(stderr, "\nNexusSearch server shutting down...\n");
    NX_CLOSESOCKET(server);
#ifdef _WIN32
    WSACleanup();
#endif
    return NX_OK;
}

/* MCP stdio server */
static void mcp_send_response(const char *id_raw, size_t id_len, const char *content_json) {
    nx_buf msg;
    nx_buf_init(&msg);
    nx_buf_put_str(&msg, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id_raw && id_len) {
        nx_buf_put(&msg, id_raw, id_len);
    } else {
        nx_buf_put_str(&msg, "null");
    }
    nx_buf_put_str(&msg, ",\"result\":");
    nx_buf_put_str(&msg, content_json);
    nx_buf_put_str(&msg, "}\n");
    if (msg.data && msg.len) {
        fwrite(msg.data, 1, msg.len, stdout);
        fflush(stdout);
    }
    nx_buf_free(&msg);
}

nx_status nx_server_run_mcp(const nx_table *table) {
    if (!table) return NX_ERR_INVALID;

    char line[HTTP_REQ_MAX];
    nx_arena arena;
    nx_arena_init(&arena, 8192);

    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = '\0';
        if (!len) continue;

        nx_arena_reset(&arena);
        nx_json *root = NULL;
        nx_json_limits limits = nx_json_default_limits();
        nx_status st = nx_json_parse(&arena, nx_slice_make((const uint8_t *)line, len),
                                     &limits, &root, NULL);
        if (st != NX_OK || !root || root->kind != NX_JSON_OBJECT) continue;

        const nx_json *id_node = nx_json_get_cstr(root, "id");
        nx_slice id_slice = {0};
        if (id_node) {
            if (id_node->kind == NX_JSON_NUMBER) id_slice = id_node->as.number.raw;
            else if (id_node->kind == NX_JSON_STRING) id_slice = id_node->as.string;
        }

        const nx_json *method_node = nx_json_get_cstr(root, "method");
        if (!method_node || method_node->kind != NX_JSON_STRING) continue;
        nx_slice m = method_node->as.string;

        if (nx_slice_eq(m, nx_slice_cstr("initialize"))) {
            const char init_res[] =
                "{\"protocolVersion\":\"2024-11-05\","
                "\"capabilities\":{\"tools\":{}},"
                "\"serverInfo\":{\"name\":\"nexus-search\",\"version\":\"0.2.0\"}}";
            mcp_send_response((const char *)id_slice.p, id_slice.n, init_res);
        } else if (nx_slice_eq(m, nx_slice_cstr("notifications/initialized"))) {
            /* Handshake completed; no response required */
        } else if (nx_slice_eq(m, nx_slice_cstr("ping"))) {
            mcp_send_response((const char *)id_slice.p, id_slice.n, "{}");
        } else if (nx_slice_eq(m, nx_slice_cstr("tools/list"))) {
            const char tools_res[] =
                "{\"tools\":["
                "{\"name\":\"nexus_search\",\"description\":\"Execute a NexusQL query against the local indexed document snapshot. Supports BM25 text, numeric filters, regex, fuzzy, and vector cosine ranking.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The NexusQL query string\"},\"scan\":{\"type\":\"boolean\",\"description\":\"Use unindexed scan oracle\"}},\"required\":[\"query\"]}},"
                "{\"name\":\"nexus_stats\",\"description\":\"Retrieve table schema, row count, and indexed field types.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                "{\"name\":\"nexus_explain\",\"description\":\"Explain and validate a NexusQL query plan.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The NexusQL query string\"}},\"required\":[\"query\"]}},"
                "{\"name\":\"nexus_get_document\",\"description\":\"Retrieve original document JSON by row index.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"row\":{\"type\":\"integer\",\"description\":\"The 0-based document row index\"}},\"required\":[\"row\"]}}"
                "]}";
            mcp_send_response((const char *)id_slice.p, id_slice.n, tools_res);
        } else if (nx_slice_eq(m, nx_slice_cstr("tools/call"))) {
            const nx_json *params = nx_json_get_cstr(root, "params");
            const nx_json *tname = params ? nx_json_get_cstr(params, "name") : NULL;
            const nx_json *args = params ? nx_json_get_cstr(params, "arguments") : NULL;

            if (tname && tname->kind == NX_JSON_STRING) {
                nx_buf text_content;
                nx_buf_init(&text_content);

                if (nx_slice_eq(tname->as.string, nx_slice_cstr("nexus_search"))) {
                    const nx_json *q_node = args ? nx_json_get_cstr(args, "query") : NULL;
                    const nx_json *scan_node = args ? nx_json_get_cstr(args, "scan") : NULL;
                    bool scan = scan_node && scan_node->kind == NX_JSON_BOOL && scan_node->as.boolean;
                    if (q_node && q_node->kind == NX_JSON_STRING) {
                        char *q_str = nx_arena_alloc(&arena, q_node->as.string.n + 1, 1);
                        if (q_str) {
                            memcpy(q_str, q_node->as.string.p, q_node->as.string.n);
                            q_str[q_node->as.string.n] = '\0';
                            (void)nx_server_query_json(table, q_str, scan, false, &text_content);
                        }
                    } else {
                        nx_buf_put_str(&text_content, "{\"error\":\"query parameter required\"}");
                    }
                } else if (nx_slice_eq(tname->as.string, nx_slice_cstr("nexus_stats"))) {
                    (void)nx_server_stats_json(table, &text_content);
                } else if (nx_slice_eq(tname->as.string, nx_slice_cstr("nexus_explain"))) {
                    const nx_json *q_node = args ? nx_json_get_cstr(args, "query") : NULL;
                    if (q_node && q_node->kind == NX_JSON_STRING) {
                        char *q_str = nx_arena_alloc(&arena, q_node->as.string.n + 1, 1);
                        if (q_str) {
                            memcpy(q_str, q_node->as.string.p, q_node->as.string.n);
                            q_str[q_node->as.string.n] = '\0';
                            (void)nx_server_query_json(table, q_str, false, true, &text_content);
                        }
                    }
                } else if (nx_slice_eq(tname->as.string, nx_slice_cstr("nexus_get_document"))) {
                    const nx_json *row_node = args ? nx_json_get_cstr(args, "row") : NULL;
                    if (row_node && row_node->kind == NX_JSON_NUMBER && row_node->as.number.exact_i64) {
                        uint32_t r = (uint32_t)row_node->as.number.i64;
                        if (r < table->rows) {
                            nx_slice doc = nx_table_document(table, r);
                            nx_buf_put(&text_content, doc.p, doc.n);
                        } else {
                            nx_buf_put_str(&text_content, "{\"error\":\"row out of bounds\"}");
                        }
                    }
                }

                nx_buf wrapped;
                nx_buf_init(&wrapped);
                nx_buf_put_str(&wrapped, "{\"content\":[{\"type\":\"text\",\"text\":");
                escape_json_str(&wrapped, (const char *)text_content.data, text_content.len);
                nx_buf_put_str(&wrapped, "}]}");
                mcp_send_response((const char *)id_slice.p, id_slice.n, (const char *)wrapped.data);
                nx_buf_free(&wrapped);
                nx_buf_free(&text_content);
            }
        }
    }

    nx_arena_free(&arena);
    return NX_OK;
}
