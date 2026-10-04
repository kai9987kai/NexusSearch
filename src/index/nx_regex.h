/* Bounded byte-oriented Thompson NFA for boolean substring search. Supports
 * literals, ., ^, $, classes/ranges/negation, (), |, * + ?, {m,n}, {m,}, {m},
 * ASCII \d \s \w and complements, \n \r \t \xHH and escaped punctuation.
 * i folds ASCII; s includes newlines in dot; m makes anchors line-relative.
 * Unsupported backreferences, lookaround, lazy quantifiers and Unicode classes
 * fail compilation. UTF-8 literals match bytes, dot matches ONE BYTE, not a rune.
 * Compiled objects are immutable/thread-safe. All text is borrowed for the call;
 * Compilation caps: 4096 pattern bytes/states, 128 nested groups, 256 tree
 * depth and 1024 explicit repeats. Expressions exceeding any cap return LIMIT.
 * result is an owned object freed with nx_regex_free. Match work is bounded,
 * NX_ERR_LIMIT must never be interpreted as a negative result. */
#ifndef NX_REGEX_H
#define NX_REGEX_H
#include "core/nx_config.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"
typedef struct nx_regex nx_regex;
#define NX_RE_ICASE 1u
#define NX_RE_DOTALL 2u
#define NX_RE_MULTILINE 4u
#define NX_RE_MAX_PATTERN 4096u
#define NX_RE_MAX_STATES 4096u
#define NX_RE_DEFAULT_WORK UINT64_C(10000000)
NX_API nx_status nx_regex_compile(nx_slice pattern, uint32_t flags, nx_regex **out, nx_error *error);
NX_API void nx_regex_free(nx_regex *re);
/* max_work == 0 selects default. Clears matched on error. */
NX_API nx_status nx_regex_match(const nx_regex *re, nx_slice text, uint64_t max_work, bool *matched);
/* Same semantics, with actual state/transition work reported even on LIMIT.
 * used is optional and cleared before any allocation or argument failure. */
NX_API nx_status nx_regex_match_counted(const nx_regex *re, nx_slice text, uint64_t max_work,
                                      bool *matched, uint64_t *used);
#endif
