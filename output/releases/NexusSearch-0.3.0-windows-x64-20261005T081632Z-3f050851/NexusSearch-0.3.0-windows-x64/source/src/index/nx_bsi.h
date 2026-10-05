/* Exact signed-int64 bit-sliced index. Immutable borrowed bytes; no mutable
 * global state. Present=NULL means all rows present, otherwise each byte must
 * be 0 or 1. Filters exclude missing rows, including NE. Signed values flip
 * their sign bit before slicing, preserving the complete int64 ordering.
 *
 * Format v1: 32-byte LE header (magic, version, rows, words, planes=65,
 * header_size=32, payload_size, CRC32C), then presence and 64 low-to-high
 * bitplanes, each words*8 bytes. CRC covers header[0:28] then payload.
 * Missing rows and unused tail bits must be zero in every value plane.
 * Builds and filters append transactionally: failures preserve existing bytes
 * and length (the destination's sticky OOM flag may be set). Inputs are
 * borrowed for each call; opened views borrow unchanged bytes for their life.
 * open allocates nothing, clears *out on failure, and rejects trailing bytes. */
#ifndef NX_BSI_H
#define NX_BSI_H
#include "core/nx_buf.h"
#include "core/nx_status.h"
#include "index/nx_bitmap.h"
typedef enum nx_compare { NX_CMP_EQ, NX_CMP_NE, NX_CMP_LT, NX_CMP_LE, NX_CMP_GT, NX_CMP_GE } nx_compare;
typedef struct nx_bsi { nx_slice bytes; uint32_t rows, words; } nx_bsi;
#define NX_BSI_MAX_ROWS 1000000u
/* values may be NULL only when rows=0. Missing-row values are not read. */
NX_API nx_status nx_bsi_build(const int64_t *values, const uint8_t *present, size_t rows, nx_buf *out);
NX_API nx_status nx_bsi_open(nx_slice bytes, nx_bsi *out);
/* Read one row without allocation. Missing rows return value=0, present=false.
 * Row must be below index->rows. Outputs are cleared on error. */
NX_API nx_status nx_bsi_get(const nx_bsi *index, uint32_t row, int64_t *value, bool *present);
NX_API nx_status nx_bsi_filter(const nx_bsi *index, nx_compare op, int64_t value, nx_buf *out);
#endif
