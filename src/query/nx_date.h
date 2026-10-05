/* UTC calendar conversion and explicit-precision query ranges. No allocation. */
#ifndef NX_DATE_H
#define NX_DATE_H
#include "query/nx_ast.h"
#include "core/nx_status.h"
typedef struct nx_instant { int64_t seconds; uint32_t nanos; } nx_instant;
typedef struct nx_date_range { nx_instant lo, end; } nx_date_range;
/* Resolve relative literals once against the caller's captured Unix UTC second.
 * Partial dates denote half-open calendar intervals; fractions are one ns. */
NX_API nx_status nx_date_resolve(const nx_datetime *value, int64_t now_seconds, nx_date_range *out);
/* Stored strings require YYYY-MM-DD, optionally T hh:mm[:ss[.fraction]] and
 * Z or +/-hh:mm. No zone means UTC. Invalid/non-date strings return TYPE. */
NX_API nx_status nx_date_read(nx_slice text, nx_instant *out);
NX_API bool nx_date_matches(nx_instant value, const nx_date_range *range, nx_op op);
#endif
