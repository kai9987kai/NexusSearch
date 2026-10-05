#include "nx_status.h"
#include <stdarg.h>
#include <stdio.h>

const char *nx_status_str(nx_status s) {
    switch (s) {
    case NX_OK: return "ok";
    case NX_ERR_NOMEM: return "out of memory";
    case NX_ERR_INVALID: return "invalid argument";
    case NX_ERR_IO: return "i/o error";
    case NX_ERR_CORRUPT: return "corrupt data";
    case NX_ERR_NOT_FOUND: return "not found";
    case NX_ERR_EXISTS: return "already exists";
    case NX_ERR_PARSE: return "parse error";
    case NX_ERR_UNSUPPORTED: return "unsupported";
    case NX_ERR_LIMIT: return "limit exceeded";
    case NX_ERR_BUSY: return "busy";
    case NX_ERR_CANCELLED: return "cancelled";
    case NX_ERR_TIMEOUT: return "timeout";
    case NX_ERR_VERSION: return "incompatible version";
    case NX_ERR_TYPE: return "type error";
    case NX_ERR_INTERNAL: return "internal error";
    }
    return "unknown status";
}

void nx_error_clear(nx_error *e) {
    if (!e) return;
    e->code = NX_OK; e->pos = -1; e->len = 0; e->msg[0] = 0;
}

void nx_error_set(nx_error *e, nx_status code, int64_t pos, int64_t len, const char *fmt, ...) {
    if (!e) return;
    e->code = code; e->pos = pos; e->len = len;
    va_list ap; va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
    e->msg[sizeof e->msg - 1] = 0;
}
