/* nx_utf8.h - strict scalar UTF-8 and deterministic search-text profile v1.
 * No locale, global state, or dependencies. Read-only inputs are borrowed.
 * Normalization folds ASCII case and/or fullwidth ASCII; it is NOT Unicode
 * NFC/NFKC/casefold. Other scalars remain byte-for-byte equivalent.
 * Tokenization groups ASCII alphanumerics/underscore and non-ASCII scalars,
 * excluding the explicit Unicode whitespace/punctuation ranges documented in
 * docs/TEXT_JSON.md. This is NOT UAX #29 or linguistic segmentation.
 * Allocating results are arena-owned, valid until arena reset/free. Separate
 * arenas/buffers may be used concurrently. Errors clear result outputs.
 */
#ifndef NX_UTF8_H
#define NX_UTF8_H
#include "core/nx_config.h"
#include "core/nx_arena.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"

enum { NX_UTF8_FOLD_ASCII = 1, NX_UTF8_FOLD_WIDTH = 2 };
typedef struct nx_utf8_token {
    nx_slice text;              /* owned by the supplied arena; binary-safe */
    size_t offset, length;      /* original input byte span */
} nx_utf8_token;
typedef struct nx_utf8_tokens {
    nx_utf8_token *items;       /* arena-owned */
    size_t count;
} nx_utf8_tokens;
typedef struct nx_utf8_limits {
    size_t max_input_bytes, max_tokens, max_token_bytes;
    unsigned flags;
} nx_utf8_limits;

/* Borrowed input/output pointers. Decode advances offset only on success;
 * EOF is NX_ERR_NOT_FOUND, invalid encoding NX_ERR_CORRUPT. Encode requires
 * four writable bytes, writes length on success (zero on error). */
NX_API nx_status nx_utf8_decode(nx_slice input, size_t *offset, uint32_t *cp);
NX_API nx_status nx_utf8_encode(uint32_t cp, uint8_t out[4], size_t *length);
/* bad_offset (optional, borrowed) receives input.n on success. */
NX_API nx_status nx_utf8_validate(nx_slice input, size_t *bad_offset);
/* Borrowed arena/out; out receives an arena-owned NUL-terminated slice whose
 * length excludes the terminator and may include embedded NUL. max_bytes
 * bounds both input and output. On failure no arena allocation is retained. */
NX_API nx_status nx_utf8_normalize(nx_arena *arena, nx_slice input, unsigned flags,
                                   size_t max_bytes, nx_slice *out);
NX_API nx_utf8_limits nx_utf8_default_limits(void);
/* Borrowed arena/input/optional limits/out. On success out owns no heap:
 * items and token text belong to arena; source need not remain alive.
 * On failure no arena allocation is retained. Zero limits mean zero allowed. */
NX_API nx_status nx_utf8_tokenize(nx_arena *arena, nx_slice input,
                                  const nx_utf8_limits *limits, nx_utf8_tokens *out);
#endif
