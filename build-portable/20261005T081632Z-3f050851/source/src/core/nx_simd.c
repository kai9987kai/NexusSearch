#include "nx_simd.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <ctype.h>

static nx_cpu_features g_feat;
static nx_simd_level g_level;
static atomic_int g_state;           /* 0 = uninit, 1 = initialising, 2 = ready */

#if defined(NX_X86_64) && NX_GNUC
static void detect(nx_cpu_features *f) {
    __builtin_cpu_init();
    f->sse42 = __builtin_cpu_supports("sse4.2") != 0;
    f->popcnt = __builtin_cpu_supports("popcnt") != 0;
    f->avx2 = __builtin_cpu_supports("avx2") != 0;
    f->fma = __builtin_cpu_supports("fma") != 0;
    f->bmi2 = __builtin_cpu_supports("bmi2") != 0;
    f->f16c = __builtin_cpu_supports("f16c") != 0;
    f->avx512f = __builtin_cpu_supports("avx512f") != 0;
    f->avx512bw = __builtin_cpu_supports("avx512bw") != 0;
    f->avx512vl = __builtin_cpu_supports("avx512vl") != 0;
    f->avx512dq = __builtin_cpu_supports("avx512dq") != 0;
    f->avx512vnni = __builtin_cpu_supports("avx512vnni") != 0;
}
#elif defined(NX_X86_64) && NX_MSVC
static void detect(nx_cpu_features *f) {
    int r[4];
    __cpuid(r, 1);
    f->sse42 = (r[2] >> 20) & 1;
    f->popcnt = (r[2] >> 23) & 1;
    f->fma = (r[2] >> 12) & 1;
    f->f16c = (r[2] >> 29) & 1;
    bool osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    bool ymm_ok = false, zmm_ok = false;
    if (osxsave && avx) {
        unsigned long long xcr0 = _xgetbv(0);
        ymm_ok = (xcr0 & 6) == 6;
        zmm_ok = (xcr0 & 0xE6) == 0xE6;
    }
    __cpuidex(r, 7, 0);
    f->avx2 = ymm_ok && ((r[1] >> 5) & 1);
    f->bmi2 = (r[1] >> 8) & 1;
    f->avx512f = zmm_ok && ((r[1] >> 16) & 1);
    f->avx512dq = zmm_ok && ((r[1] >> 17) & 1);
    f->avx512bw = zmm_ok && ((r[1] >> 30) & 1);
    f->avx512vl = zmm_ok && ((r[1] >> 31) & 1);
    f->avx512vnni = zmm_ok && ((r[2] >> 11) & 1);
    if (!ymm_ok) f->fma = f->f16c = false;
}
#elif defined(NX_ARM64)
static void detect(nx_cpu_features *f) { f->neon = true; }
#else
static void detect(nx_cpu_features *f) { (void)f; }
#endif

static nx_simd_level host_level(const nx_cpu_features *f) {
    if (f->neon) return NX_SIMD_NEON;
    if (f->avx512f && f->avx512bw && f->avx512vl && f->avx512dq && f->avx2 && f->fma) return NX_SIMD_AVX512;
    if (f->avx2 && f->fma && f->bmi2 && f->popcnt) return NX_SIMD_AVX2;
    if (f->sse42 && f->popcnt) return NX_SIMD_SSE42;
    return NX_SIMD_SCALAR;
}

static void init_once(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_state, &expected, 1)) {
        nx_cpu_features f; memset(&f, 0, sizeof f);
        detect(&f);
        nx_simd_level lv = host_level(&f);
        const char *cap = getenv("NX_SIMD");
        if (cap) {
            nx_simd_level c = lv;
            if (!strcmp(cap, "scalar")) c = NX_SIMD_SCALAR;
            else if (!strcmp(cap, "sse42")) c = NX_SIMD_SSE42;
            else if (!strcmp(cap, "avx2")) c = NX_SIMD_AVX2;
            if (c < lv || lv == NX_SIMD_NEON) { if (lv != NX_SIMD_NEON || c == NX_SIMD_SCALAR) lv = c; }
        }
        g_feat = f; g_level = lv;
        atomic_store(&g_state, 2);
    } else {
        while (atomic_load(&g_state) != 2) { }
    }
}

const nx_cpu_features *nx_cpu(void) { if (atomic_load(&g_state) != 2) init_once(); return &g_feat; }
nx_simd_level nx_simd_level_get(void) { if (atomic_load(&g_state) != 2) init_once(); return g_level; }

const char *nx_simd_level_name(nx_simd_level l) {
    switch (l) {
    case NX_SIMD_SCALAR: return "scalar";
    case NX_SIMD_SSE42: return "sse4.2";
    case NX_SIMD_AVX2: return "avx2";
    case NX_SIMD_AVX512: return "avx512";
    case NX_SIMD_NEON: return "neon";
    }
    return "?";
}
