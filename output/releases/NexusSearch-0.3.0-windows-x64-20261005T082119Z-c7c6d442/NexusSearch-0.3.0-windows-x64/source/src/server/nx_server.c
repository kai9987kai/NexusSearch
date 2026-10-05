#include "server/nx_server.h"
#include "server/nx_web_ui.h"
#include "engine/nx_search_index.h"
#include "nexus/version.h"
#include "core/nx_mem.h"
#include "core/nx_json.h"
#include "core/nx_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>
#include <limits.h>

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
#include <sys/time.h>
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
    nx_buf_printf(out, "{\"version\":\"" NEXUS_VERSION "\",\"rows\":%" PRIu32 ",\"fields\":[", table->rows);
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

static nx_status server_query_json(const nx_table *table, const nx_search_index *index,
                                   const char *query_str, bool scan, bool explain, nx_buf *out) {
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

    nx_status status = index ? nx_search_index_search(index, nx_buf_slice(&qbuf), &options, &result, &error) :
        nx_search(table, nx_buf_slice(&qbuf), &options, &result, &error);
    nx_buf_free(&qbuf);

    if (status != NX_OK) {
        nx_buf_put_str(out, "{\"error\":");
        const char *message = error.msg[0] ? error.msg : nx_status_str(status);
        escape_json_str(out, message, strlen(message));
        nx_buf_put_u8(out, '}');
        return out->oom ? NX_ERR_NOMEM : NX_OK;
    }

    status = nx_search_json(&result, out);
    nx_search_result_free(&result);
    return status;
}

nx_status nx_server_query_json(const nx_table *table, const char *query_str,
                              bool scan, bool explain, nx_buf *out) {
    return server_query_json(table, NULL, query_str, scan, explain, out);
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool url_decode(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    size_t d = 0;
    if (!dst_cap) return false;
    for (size_t i = 0; i < src_len; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '+') c = ' ';
        else if (c == '%') {
            if (i + 2 >= src_len) return false;
            int hi = hex_digit((unsigned char)src[i + 1]);
            int lo = hex_digit((unsigned char)src[i + 2]);
            if (hi < 0 || lo < 0) return false;
            c = (unsigned char)(hi * 16 + lo);
            i += 2;
        }
        if (!c || d + 1 >= dst_cap) return false;
        dst[d++] = (char)c;
    }
    dst[d] = '\0';
    return nx_utf8_validate(nx_slice_make((const uint8_t *)dst, d), NULL) == NX_OK;
}

static bool read_query(const char *req, const char *method, char *params,
                       char *query, size_t cap, bool *scan) {
    *scan = false;
    query[0] = '\0';
    if (!strcmp(method, "GET")) {
        bool seen_query = false, seen_scan = false;
        for (char *param = params; param && *param;) {
            char *next = strchr(param, '&');
            if (next) *next = '\0';
            if (!strncmp(param, "q=", 2)) {
                if (seen_query || !url_decode(query, cap, param + 2, strlen(param + 2))) return false;
                seen_query = true;
            } else if (!strncmp(param, "scan=", 5)) {
                if (seen_scan) return false;
                seen_scan = true;
                if (!strcmp(param + 5, "1") || !strcmp(param + 5, "true")) *scan = true;
                else if (strcmp(param + 5, "0") && strcmp(param + 5, "false")) return false;
            }
            param = next ? next + 1 : NULL;
        }
        return seen_query && query[0];
    }
    if (strcmp(method, "POST")) return false;
    const char *body = strstr(req, "\r\n\r\n");
    if (!body) return false;
    body += 4;
    nx_arena arena;
    nx_arena_init(&arena, 4096);
    nx_json *root = NULL;
    nx_json_limits limits = nx_json_default_limits();
    limits.max_input_bytes = HTTP_REQ_MAX;
    limits.max_nodes = 256;
    limits.max_depth = 8;
    limits.max_string_bytes = HTTP_REQ_MAX;
    limits.max_work = HTTP_REQ_MAX * 16u;
    bool ok = nx_json_parse(&arena, nx_slice_cstr(body), &limits, &root, NULL) == NX_OK;
    const nx_json *value = ok ? nx_json_get_cstr(root, "query") : NULL;
    const nx_json *scan_value = ok ? nx_json_get_cstr(root, "scan") : NULL;
    ok = value && value->kind == NX_JSON_STRING && value->as.string.n > 0 &&
         value->as.string.n < cap && !memchr(value->as.string.p, 0, value->as.string.n) &&
         (!scan_value || scan_value->kind == NX_JSON_BOOL);
    if (ok) {
        memcpy(query, value->as.string.p, value->as.string.n);
        query[value->as.string.n] = '\0';
        if (scan_value) *scan = scan_value->as.boolean;
    }
    nx_arena_free(&arena);
    return ok;
}

static bool send_all(nx_socket_t sock, const char *data, size_t length) {
    while (length) {
        int chunk = length > INT_MAX ? INT_MAX : (int)length;
#ifdef MSG_NOSIGNAL
        int sent = (int)send(sock, data, (size_t)chunk, MSG_NOSIGNAL);
#else
        int sent = (int)send(sock, data, chunk, 0);
#endif
        if (sent <= 0) return false;
        data += sent;
        length -= (size_t)sent;
    }
    return true;
}

static void send_http_response(nx_socket_t sock, int status_code, const char *status_text,
                               const char *content_type, const char *body, size_t body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        status_code, status_text, content_type, body_len);
    if (hlen <= 0 || (size_t)hlen >= sizeof(header) || !send_all(sock, header, (size_t)hlen)) return;
    if (body && body_len > 0) {
        (void)send_all(sock, body, body_len);
    }
}

static void send_sse_headers(nx_socket_t sock) {
    const char headers[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    (void)send_all(sock, headers, strlen(headers));
}

static void send_sse_event(nx_socket_t sock, const char *event_name, const char *data, size_t data_len) {
    char buf[128];
    int elen = snprintf(buf, sizeof(buf), "event: %s\ndata: ", event_name);
    if (elen > 0) (void)send_all(sock, buf, (size_t)elen);
    if (data && data_len > 0) (void)send_all(sock, data, data_len);
    (void)send_all(sock, "\n\n", 2);
}

static bool header_name(const char *p, size_t n, const char *name) {
    if (strlen(name) != n) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        if (c != name[i]) return false;
    }
    return true;
}

/* Reject ambiguous framing instead of guessing where a request ends. */
static bool content_length(const char *req, const char *end, size_t *length) {
    const char *line = strstr(req, "\r\n");
    bool seen_length = false;
    *length = 0;
    if (!line) return false;
    line += 2;
    while (line < end) {
        const char *next = strstr(line, "\r\n");
        const char *colon = next ? memchr(line, ':', (size_t)(next - line)) : NULL;
        if (!next || next > end || !colon || colon == line) return false;
        if (header_name(line, (size_t)(colon - line), "transfer-encoding")) return false;
        if (header_name(line, (size_t)(colon - line), "content-length")) {
            if (seen_length) return false;
            seen_length = true;
            const char *p = colon + 1;
            while (p < next && (*p == ' ' || *p == '\t')) ++p;
            if (p == next || *p < '0' || *p > '9') return false;
            size_t value = 0;
            while (p < next && *p >= '0' && *p <= '9') {
                value = value * 10 + (size_t)(*p++ - '0');
                if (value >= HTTP_REQ_MAX) return false;
            }
            while (p < next && (*p == ' ' || *p == '\t')) ++p;
            if (p != next) return false;
            *length = value;
        }
        line = next + 2;
    }
    return true;
}

/* Keep the local document service same-origin and reject DNS-rebinding hosts. */
static bool allowed_origin(nx_socket_t client, const char *req) {
    struct sockaddr_in local;
#ifdef _WIN32
    int local_len = sizeof(local);
#else
    socklen_t local_len = sizeof(local);
#endif
    if (getsockname(client, (struct sockaddr *)&local, &local_len)) return false;
    char address[INET_ADDRSTRLEN], numeric_host[64], localhost[64];
    if (!inet_ntop(AF_INET, &local.sin_addr, address, sizeof(address))) return false;
    unsigned port = (unsigned)ntohs(local.sin_port);
    (void)snprintf(numeric_host, sizeof(numeric_host), "%s:%u", address, port);
    (void)snprintf(localhost, sizeof(localhost), "localhost:%u", port);
    nx_slice host = {0}, origin = {0};
    const char *line = strstr(req, "\r\n");
    if (!line) return false;
    line += 2;
    while (*line && strncmp(line, "\r\n", 2)) {
        const char *next = strstr(line, "\r\n");
        const char *colon = next ? memchr(line, ':', (size_t)(next - line)) : NULL;
        if (!colon) return false;
        const char *value = colon + 1, *end = next;
        while (value < end && (*value == ' ' || *value == '\t')) ++value;
        while (end > value && (end[-1] == ' ' || end[-1] == '\t')) --end;
        nx_slice field = nx_slice_make((const uint8_t *)value, (size_t)(end - value));
        if (header_name(line, (size_t)(colon - line), "host")) {
            if (host.p || !field.n) return false;
            host = field;
        } else if (header_name(line, (size_t)(colon - line), "origin")) {
            if (origin.p || !field.n) return false;
            origin = field;
        }
        line = next + 2;
    }
    bool loopback = (ntohl(local.sin_addr.s_addr) >> 24) == 127;
    if (!nx_slice_eq(host, nx_slice_cstr(numeric_host)) &&
        !(loopback && nx_slice_eq(host, nx_slice_cstr(localhost))) &&
        !(port == 80 && (nx_slice_eq(host, nx_slice_cstr(address)) ||
                       (loopback && nx_slice_eq(host, nx_slice_cstr("localhost")))))) return false;
    if (!origin.p) return true;
    return origin.n == host.n + 7 && !memcmp(origin.p, "http://", 7) &&
           !memcmp(origin.p + 7, host.p, host.n);
}

static void handle_http_client(const nx_table *table, const nx_search_index *index, nx_socket_t client) {
#ifdef _WIN32
    DWORD timeout = 5000;
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
#else
    struct timeval timeout = {5, 0};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    char req[HTTP_REQ_MAX];
    int recvd = 0;
    bool complete = false;
    while (recvd < (int)(sizeof(req) - 1)) {
        int n = recv(client, req + recvd, (int)(sizeof(req) - 1 - (size_t)recvd), 0);
        if (n <= 0) break;
        if (memchr(req + recvd, 0, (size_t)n)) break;
        recvd += n;
        req[recvd] = '\0';
        char *header_end = strstr(req, "\r\n\r\n");
        if (header_end) {
            size_t body_length;
            if (!content_length(req, header_end, &body_length)) break;
            size_t header_length = (size_t)(header_end + 4 - req);
            if (body_length >= sizeof(req) - header_length) break;
            if ((size_t)recvd >= header_length + body_length) {
                complete = (size_t)recvd == header_length + body_length;
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
    char version[16] = {0};
    int consumed = 0;
    if (!complete || sscanf(req, "%15s %2047s %15s%n", method, path, version, &consumed) != 3 ||
        (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0")) ||
        strncmp(req + consumed, "\r\n", 2) || path[0] != '/') {
        const char message[] = "{\"error\":\"bad request\"}";
        send_http_response(client, 400, "Bad Request", "application/json", message, sizeof(message) - 1);
        NX_CLOSESOCKET(client);
        return;
    }

    if (!strcmp(method, "OPTIONS")) {
        if (!allowed_origin(client, req)) {
            const char message[] = "{\"error\":\"origin or host not allowed\"}";
            send_http_response(client, 403, "Forbidden", "application/json", message, sizeof(message) - 1);
            NX_CLOSESOCKET(client);
            return;
        }
        send_http_response(client, 204, "No Content", "text/plain", "", 0);
        NX_CLOSESOCKET(client);
        return;
    }

    if (!allowed_origin(client, req)) {
        const char message[] = "{\"error\":\"origin or host not allowed\"}";
        send_http_response(client, 403, "Forbidden", "application/json", message, sizeof(message) - 1);
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
        const char msg[] = "{\"status\":\"ok\",\"version\":\"" NEXUS_VERSION "\"}";
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

        bool query_ok = read_query(req, method, query_part, q_decoded, sizeof(q_decoded), &scan);

        if (!query_ok) {
            send_http_response(client, 400, "Bad Request", "application/json",
                               "{\"error\":\"invalid or missing query\"}", 36);
        } else {
            nx_buf res;
            nx_buf_init(&res);
            (void)server_query_json(table, index, q_decoded, scan, is_explain, &res);
            send_http_response(client, 200, "OK", "application/json; charset=utf-8",
                               res.data ? (const char *)res.data : "{}", res.len ? res.len : 2);
            nx_buf_free(&res);
        }
    } else if (!strcmp(path, "/api/stream")) {
        char q_decoded[2048] = {0};
        bool scan = false;

        bool query_ok = read_query(req, method, query_part, q_decoded, sizeof(q_decoded), &scan);

        if (!query_ok) {
            send_http_response(client, 400, "Bad Request", "application/json",
                               "{\"error\":\"invalid or missing query\"}", 36);
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

            nx_status status = index ? nx_search_index_search(index, nx_buf_slice(&qbuf), &options, &result, &error) :
                nx_search(table, nx_buf_slice(&qbuf), &options, &result, &error);
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
        if (query_part && !strncmp(query_part, "row=", 4)) {
            const char *p = query_part + 4;
            uint64_t value = 0;
            bool valid = *p >= '0' && *p <= '9';
            while (valid && *p >= '0' && *p <= '9') {
                value = value * 10 + (uint64_t)(*p++ - '0');
                if (value > UINT32_MAX) valid = false;
            }
            if (valid && !*p) row = (uint32_t)value;
        }
        if (row < table->rows) {
            nx_slice doc = nx_table_document(table, row);
            send_http_response(client, 200, "OK", "application/json; charset=utf-8",
                               (const char *)doc.p, doc.n);
        } else {
            const char message[] = "{\"error\":\"doc not found\"}";
            send_http_response(client, 404, "Not Found", "application/json", message, sizeof(message) - 1);
        }
    } else {
        send_http_response(client, 404, "Not Found", "application/json", "{\"error\":\"not found\"}", 21);
    }

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

    nx_search_index *index = NULL;
    nx_error index_error; nx_error_clear(&index_error);
    nx_status index_status = nx_search_index_build(table, &index, &index_error);
    if (index_status != NX_OK) {
        fprintf(stderr, "Text preparation unavailable (%s); using exact scan fallback.\n",
                index_error.msg[0] ? index_error.msg : nx_status_str(index_status));
    }

    g_server_stop = 0;
    signal(SIGINT, on_sigint);

    fprintf(stderr, "\n  ============================================================\n"
                    "  NexusSearch " NEXUS_VERSION " HTTP & Web UI Server\n"
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
                handle_http_client(table, index, client);
            }
        }
    }

    fprintf(stderr, "\nNexusSearch server shutting down...\n");
    NX_CLOSESOCKET(server);
    nx_search_index_free(index);
#ifdef _WIN32
    WSACleanup();
#endif
    return NX_OK;
}

/* MCP stdio server: bounded, newline-delimited JSON-RPC subset. */
#define MCP_QUERY_MAX 2048u

static nx_status mcp_send(nx_slice id, const char *member, nx_slice value) {
    nx_buf msg;
    nx_buf_init(&msg);
    nx_buf_put_str(&msg, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id.n) nx_buf_put(&msg, id.p, id.n);
    else nx_buf_put_str(&msg, "null");
    nx_buf_put_str(&msg, member);
    nx_buf_put(&msg, value.p, value.n);
    nx_buf_put_str(&msg, "}\n");
    nx_status status = NX_OK;
    if (msg.oom) status = NX_ERR_NOMEM;
    else if (fwrite(msg.data, 1, msg.len, stdout) != msg.len || fflush(stdout) != 0)
        status = NX_ERR_IO;
    nx_buf_free(&msg);
    return status;
}

static nx_status mcp_result(nx_slice id, nx_slice value) {
    return mcp_send(id, ",\"result\":", value);
}

static nx_status mcp_error(nx_slice id, int code, const char *message) {
    nx_buf error;
    nx_buf_init(&error);
    nx_buf_printf(&error, "{\"code\":%d,\"message\":", code);
    escape_json_str(&error, message, strlen(message));
    nx_buf_put_u8(&error, '}');
    nx_status status = error.oom ? NX_ERR_NOMEM :
        mcp_send(id, ",\"error\":", nx_buf_slice(&error));
    nx_buf_free(&error);
    return status;
}

/* Drain an oversized frame through its newline; never dispatch its tail.
 * A nonempty final fragment without a newline is also rejected. */
static int mcp_read_frame(char *line, size_t capacity, size_t *length) {
    bool overflow = false;
    *length = 0;
    int ch;
    while ((ch = fgetc(stdin)) != EOF) {
        if (ch == '\n') return overflow ? -1 : 1;
        if (*length == capacity) overflow = true;
        else line[(*length)++] = (char)ch;
    }
    return (overflow || *length) ? -1 : 0;
}

static nx_status mcp_call(const nx_table *table, const nx_search_index *index,
                          nx_slice id, const nx_json *params) {
    const nx_json *name = nx_json_get_cstr(params, "name");
    const nx_json *args = nx_json_get_cstr(params, "arguments");
    if (!name || name->kind != NX_JSON_STRING ||
        (args && args->kind != NX_JSON_OBJECT))
        return mcp_error(id, -32602, "Expected tool name and object arguments");

    bool search = nx_slice_eq(name->as.string, nx_slice_cstr("nexus_search"));
    bool explain = nx_slice_eq(name->as.string, nx_slice_cstr("nexus_explain"));
    bool stats = nx_slice_eq(name->as.string, nx_slice_cstr("nexus_stats"));
    bool document = nx_slice_eq(name->as.string, nx_slice_cstr("nexus_get_document"));
    if (!search && !explain && !stats && !document)
        return mcp_error(id, -32602, "Unknown tool");

    nx_buf content;
    nx_buf_init(&content);
    nx_status status = NX_OK;
    bool is_error = false;
    if (search || explain) {
        const nx_json *query = nx_json_get_cstr(args, "query");
        const nx_json *scan = nx_json_get_cstr(args, "scan");
        if (!query || query->kind != NX_JSON_STRING || !query->as.string.n ||
            query->as.string.n >= MCP_QUERY_MAX ||
            memchr(query->as.string.p, 0, query->as.string.n) ||
            (scan && scan->kind != NX_JSON_BOOL))
            return mcp_error(id, -32602, "Expected a nonempty query under 2048 UTF-8 bytes and boolean scan");
        char query_text[MCP_QUERY_MAX];
        memcpy(query_text, query->as.string.p, query->as.string.n);
        query_text[query->as.string.n] = '\0';
        status = server_query_json(table, index, query_text,
            scan && scan->as.boolean, explain, &content);
        /* The helper deliberately represents query execution errors as JSON. */
        is_error = content.len >= 9 && !memcmp(content.data, "{\"error\":", 9);
    } else if (stats) {
        status = nx_server_stats_json(table, &content);
    } else {
        const nx_json *row = nx_json_get_cstr(args, "row");
        if (!row || row->kind != NX_JSON_NUMBER || !row->as.number.exact_i64 ||
            row->as.number.i64 < 0 || (uint64_t)row->as.number.i64 >= table->rows)
            return mcp_error(id, -32602, "Expected an integer row within the snapshot");
        nx_slice doc = nx_table_document(table, (uint32_t)row->as.number.i64);
        nx_buf_put(&content, doc.p, doc.n);
    }
    if (status != NX_OK || content.oom) {
        nx_buf_free(&content);
        return mcp_error(id, -32603, "Tool execution failed");
    }
    nx_buf wrapped;
    nx_buf_init(&wrapped);
    nx_buf_put_str(&wrapped, "{\"content\":[{\"type\":\"text\",\"text\":");
    escape_json_str(&wrapped, (const char *)content.data, content.len);
    nx_buf_printf(&wrapped, "}],\"isError\":%s}", is_error ? "true" : "false");
    status = wrapped.oom ? NX_ERR_NOMEM : mcp_result(id, nx_buf_slice(&wrapped));
    nx_buf_free(&wrapped);
    nx_buf_free(&content);
    return status;
}

nx_status nx_server_run_mcp(const nx_table *table) {
    if (!table) return NX_ERR_INVALID;
    nx_search_index *index = NULL;
    nx_error index_error; nx_error_clear(&index_error);
    nx_status index_status = nx_search_index_build(table, &index, &index_error);
    if (index_status != NX_OK)
        fprintf(stderr, "Text preparation unavailable (%s); using exact scan fallback.\n",
                index_error.msg[0] ? index_error.msg : nx_status_str(index_status));
    char line[HTTP_REQ_MAX];
    nx_arena arena;
    nx_arena_init(&arena, 8192);
    nx_status result = NX_OK;
    for (;;) {
        size_t len = 0;
        int frame = mcp_read_frame(line, sizeof(line), &len);
        if (!frame) {
            if (ferror(stdin)) result = NX_ERR_IO;
            break;
        }
        if (frame < 0) {
            result = mcp_error(nx_slice_make(NULL, 0), -32700,
                               "Frame exceeds 65536 bytes or lacks a newline");
            if (result != NX_OK) break;
            continue;
        }
        nx_arena_reset(&arena);
        nx_json *root = NULL;
        nx_json_limits limits = nx_json_default_limits();
        limits.max_input_bytes = sizeof(line);
        limits.max_nodes = 4096;
        limits.max_depth = 32;
        limits.max_string_bytes = sizeof(line);
        limits.max_work = 4u * 1024u * 1024u;
        nx_status status = nx_json_parse(&arena,
            nx_slice_make((const uint8_t *)line, len), &limits, &root, NULL);
        if (status != NX_OK) {
            result = mcp_error(nx_slice_make(NULL, 0), -32700, "Parse error");
            if (result != NX_OK) break;
            continue;
        }
        const nx_json *version = nx_json_get_cstr(root, "jsonrpc");
        const nx_json *method = nx_json_get_cstr(root, "method");
        const nx_json *id_node = nx_json_get_cstr(root, "id");
        const nx_json *params = nx_json_get_cstr(root, "params");
        bool valid_id = !id_node || id_node->kind == NX_JSON_NULL ||
            id_node->kind == NX_JSON_NUMBER || id_node->kind == NX_JSON_STRING;
        if (!root || root->kind != NX_JSON_OBJECT || !version ||
            version->kind != NX_JSON_STRING ||
            !nx_slice_eq(version->as.string, nx_slice_cstr("2.0")) ||
            !method || method->kind != NX_JSON_STRING || !valid_id ||
            (params && params->kind != NX_JSON_OBJECT && params->kind != NX_JSON_ARRAY)) {
            result = mcp_error(nx_slice_make(NULL, 0), -32600, "Invalid Request");
            if (result != NX_OK) break;
            continue;
        }
        /* Notifications never receive a response, including unknown methods. */
        if (!id_node) continue;
        /* Echo the original JSON token: decoded string bytes lack their quotes
         * and escaping, and number conversion would lose large integer IDs. */
        nx_slice id = nx_slice_make((const uint8_t *)line + id_node->offset, id_node->length);
        if (params && params->kind != NX_JSON_OBJECT) {
            result = mcp_error(id, -32602, "Expected named object parameters");
        } else if (nx_slice_eq(method->as.string, nx_slice_cstr("initialize"))) {
            const char init_res[] =
                "{\"protocolVersion\":\"2024-11-05\","
                "\"capabilities\":{\"tools\":{}},"
                "\"serverInfo\":{\"name\":\"nexus-search\",\"version\":\"" NEXUS_VERSION "\"}}";
            result = mcp_result(id, nx_slice_cstr(init_res));
        } else if (nx_slice_eq(method->as.string, nx_slice_cstr("ping"))) {
            result = mcp_result(id, nx_slice_cstr("{}"));
        } else if (nx_slice_eq(method->as.string, nx_slice_cstr("tools/list"))) {
            const char tools_res[] =
                "{\"tools\":["
                "{\"name\":\"nexus_search\",\"description\":\"Execute NexusQL against the local immutable snapshot using BM25 text, exact filters, and exact cosine vectors.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":2047},\"scan\":{\"type\":\"boolean\"}},\"required\":[\"query\"]}},"
                "{\"name\":\"nexus_stats\",\"description\":\"Retrieve snapshot schema and row count.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                "{\"name\":\"nexus_explain\",\"description\":\"Explain and validate a NexusQL query plan.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":2047}},\"required\":[\"query\"]}},"
                "{\"name\":\"nexus_get_document\",\"description\":\"Retrieve original document JSON by row index.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"row\":{\"type\":\"integer\",\"minimum\":0}},\"required\":[\"row\"]}}"
                "]}";
            result = mcp_result(id, nx_slice_cstr(tools_res));
        } else if (nx_slice_eq(method->as.string, nx_slice_cstr("tools/call"))) {
            result = mcp_call(table, index, id, params);
        } else {
            result = mcp_error(id, -32601, "Method not found");
        }
        if (result != NX_OK) break;
    }
    nx_arena_free(&arena);
    nx_search_index_free(index);
    return result;
}
