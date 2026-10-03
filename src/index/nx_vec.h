/* Float32 vector primitives with double accumulation and runtime AVX2 dispatch.
 * All buffers are borrowed and must contain n naturally aligned elements.
 * No additional SIMD alignment is required.
 * Public checked calls reject nonfinite inputs and dimensions outside 1..65536.
 * Cosine similarity is 0 if either vector has zero norm; otherwise [-1,1].
 * SQ8 is per-vector min/max uniform quantization, not RaBitQ or an ANN index.
 * Codes and decoded outputs belong to the caller; conversion allocates nothing.
 * Scalar/dispatched reductions can differ by <=1e-12 relative to sum magnitudes
 * from accumulation order; no speed claim is made for emulated x64 execution.
 * No mutable state beyond the existing thread-safe CPU dispatch singleton. */
#ifndef NX_VEC_H
#define NX_VEC_H
#include "core/nx_config.h"
#include "core/nx_status.h"
#define NX_VEC_MAX_DIMS 65536u
typedef enum nx_vec_metric { NX_VEC_DOT, NX_VEC_L2SQ, NX_VEC_COSINE } nx_vec_metric;
typedef struct nx_sq8 { float minimum; double step; } nx_sq8;
NX_API nx_status nx_vec_score(const float *a, const float *b, size_t n, nx_vec_metric metric, double *out);
NX_API nx_status nx_vec_score_scalar(const float *a, const float *b, size_t n, nx_vec_metric metric, double *out);
NX_API const char *nx_vec_impl_name(void);
/* Failures leave output arrays unchanged; metadata is cleared on encode error. */
NX_API nx_status nx_vec_sq8_encode(const float *v, size_t n, uint8_t *codes, nx_sq8 *metadata);
NX_API nx_status nx_vec_sq8_decode(const uint8_t *codes, size_t n, nx_sq8 metadata, float *v);
#endif
