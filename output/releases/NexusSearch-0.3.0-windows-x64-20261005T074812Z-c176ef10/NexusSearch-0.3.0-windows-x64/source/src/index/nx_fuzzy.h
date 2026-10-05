/* Bounded Levenshtein distance over Unicode scalar values, unit insertion /
 * deletion / substitution costs. Case-sensitive; normalize before calling if
 * desired. No transpositions, grapheme segmentation or diacritic equivalence.
 * Inputs are borrowed UTF-8 (max 16 KiB each). out receives the exact distance
 * when <= maximum, otherwise maximum+1. Invalid UTF-8 returns CORRUPT, allocation
 * failure NOMEM, maximum >64 or oversized input LIMIT. Errors clear out.
 * Local scratch is freed before return; no global state. */
#ifndef NX_FUZZY_H
#define NX_FUZZY_H
#include "core/nx_config.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"
NX_API nx_status nx_fuzzy_distance(nx_slice a, nx_slice b, uint32_t maximum, uint32_t *out);
#endif
