#include "nx_test.h"
#include "core/nx_simd.h"

static void test_detect(void) {
    const nx_cpu_features *f = nx_cpu();
    NX_REQUIRE(f != NULL);
    nx_simd_level l = nx_simd_level_get();
    NX_CHECK(l >= NX_SIMD_SCALAR && l <= NX_SIMD_NEON);
    NX_CHECK(nx_cpu() == f);                         /* stable pointer */
    if (l == NX_SIMD_AVX2) NX_CHECK(f->avx2 && f->fma && f->bmi2 && f->popcnt);
    if (l == NX_SIMD_AVX512) NX_CHECK(f->avx512f && f->avx2);
    NX_CHECK(nx_simd_level_name(l) != NULL);
    printf("    host simd level: %s\n", nx_simd_level_name(l));
}

int main(void) { NX_RUN(test_detect); return nx_test_summary(); }
