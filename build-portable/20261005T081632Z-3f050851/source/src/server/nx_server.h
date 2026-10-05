/* NexusSearch HTTP REST, Web UI, and Model Context Protocol (MCP) server.
 * Implements dependency-free HTTP/1.1 and stdio JSON-RPC 2.0 servers in C11. */
#ifndef NX_SERVER_H
#define NX_SERVER_H

#include "core/nx_config.h"
#include "core/nx_status.h"
#include "seg/nx_table.h"
#include "engine/nx_search.h"

typedef struct nx_server_config {
    const char *host;      /* e.g. "127.0.0.1" */
    uint16_t port;         /* e.g. 8080 */
    bool verbose;
} nx_server_config;

/* Run HTTP search server for the open table snapshot.
 * Serves interactive Web UI at GET /, REST API at /api/search, /api/stats, /api/explain.
 * Blocks until interrupted (e.g. SIGINT) or socket error. */
NX_API nx_status nx_server_run_http(const nx_table *table, const nx_server_config *config);

/* Run Model Context Protocol (MCP) server over standard I/O (stdin/stdout).
 * Implements JSON-RPC 2.0 with tools: nexus_search, nexus_stats, nexus_explain, nexus_get_document.
 * Blocks until EOF on stdin or error. */
NX_API nx_status nx_server_run_mcp(const nx_table *table);

/* Internal / utility: format snapshot schema as JSON. */
NX_API nx_status nx_server_stats_json(const nx_table *table, nx_buf *out);

/* Internal / utility: execute query and format as JSON. */
NX_API nx_status nx_server_query_json(const nx_table *table, const char *query_str,
                                     bool scan, bool explain, nx_buf *out);

#endif
