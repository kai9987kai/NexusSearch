/* nx_simd.h - runtime CPU feature detection for SIMD kernel dispatch.
 *
 * Pattern used by every SIMD-accelerated module:
 *   - a portable scalar implementation (the oracle) that is always compiled;
 *   - ISA-specific variants in functions marked NX_TARGET("avx2,fma") (x86) or
 *     compiled for NEON (arm64) - never whole files with -mavx2;
 *   - a function-pointer table bound ONCE (thread-safe, idempotent) from
 *     nx_simd_level();
 *   - the variants are also exported by name so tests can compare them to the
 *     scalar oracle directly, independent of what the host CPU would pick.
 *
 * Process-wide override for testing/benchmarking: environment variable
 * NX_SIMD=scalar|sse42|avx2 caps the level (read once at first use).
 */
#ifndef NX_SIMD_H
#define NX_SIMD_H

#include "nx_config.h"

typedef enum nx_simd_level {
    NX_SIMD_SCALAR = 0,
    NX_SIMD_SSE42  = 1,     /* x86-64-v2 baseline: SSE4.2 + POPCNT */
    NX_SIMD_AVX2   = 2,     /* AVX2 + FMA + BMI2 + F16C (x86-64-v3) */
    NX_SIMD_AVX512 = 3,     /* AVX-512 F/BW/VL/DQ (never selected on hosts without it) */
    NX_SIMD_NEON   = 4      /* arm64 */
} nx_simd_level;

typedef struct nx_cpu_features {
    bool sse42, popcnt, avx2, fma, bmi2, f16c;
    bool avx512f, avx512bw, avx512vl, avx512dq, avx512vnni;
    bool neon;
} nx_cpu_features;

/* Detected host features (before any NX_SIMD cap). Thread-safe, cheap after the first call. */
NX_API const nx_cpu_features *nx_cpu(void);
/* Highest level the host supports AFTER applying the NX_SIMD environment cap. */
NX_API nx_simd_level nx_simd_level_get(void);
NX_API const char *nx_simd_level_name(nx_simd_level l);

#endif /* NX_SIMD_H */
