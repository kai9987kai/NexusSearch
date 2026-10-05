/* nx_status.h - error codes shared by every module. */
#ifndef NX_STATUS_H
#define NX_STATUS_H

#include "nx_config.h"

typedef enum nx_status {
    NX_OK = 0,
    NX_ERR_NOMEM,        /* allocation failed                                  */
    NX_ERR_INVALID,      /* bad argument / API misuse                          */
    NX_ERR_IO,           /* OS-level I/O failure                               */
    NX_ERR_CORRUPT,      /* on-disk data failed validation / checksum          */
    NX_ERR_NOT_FOUND,
    NX_ERR_EXISTS,
    NX_ERR_PARSE,        /* query / JSON / file-format syntax error            */
    NX_ERR_UNSUPPORTED,
    NX_ERR_LIMIT,        /* configured limit exceeded (depth, size, count)     */
    NX_ERR_BUSY,
    NX_ERR_CANCELLED,
    NX_ERR_TIMEOUT,
    NX_ERR_VERSION,      /* file written by an incompatible format version     */
    NX_ERR_TYPE,         /* query/schema type mismatch (e.g. range on bool)    */
    NX_ERR_INTERNAL      /* invariant violated - a bug                         */
} nx_status;

NX_API const char *nx_status_str(nx_status s);

/* Rich error record for parsers / binders: code + human message + byte offset
 * into the offending input (-1 if not applicable). Always NUL-terminated. */
typedef struct nx_error {
    nx_status code;
    int64_t   pos;
    int64_t   len;       /* length of the offending span, 0 if unknown */
    char      msg[240];
} nx_error;

NX_API void nx_error_clear(nx_error *e);
NX_API void nx_error_set(nx_error *e, nx_status code, int64_t pos, int64_t len, const char *fmt, ...) NX_PRINTF(5, 6);

#endif /* NX_STATUS_H */
