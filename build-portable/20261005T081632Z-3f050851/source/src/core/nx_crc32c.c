#include "nx_crc32c.h"
#include "nx_simd.h"
#include <stdatomic.h>

#if defined(NX_X86_64)
#  include <nmmintrin.h>
#  define NX_CRC_X86 1
#elif defined(NX_ARM64) && defined(__ARM_FEATURE_CRC32)
#  include <arm_acle.h>
#  define NX_CRC_ARM 1
#endif

#define CRC_POLY_REFLECTED 0x82F63B78u      /* bit-reversed 0x1EDC6F41 */

/* Block sizes (bytes per stream) of the 3-way interleaved hardware loop. The crc32
 * instruction has latency 3 / throughput 1, so three independent streams keep the
 * unit saturated; the partial CRCs are recombined with precomputed "append L zero
 * bytes" operators. */
#define HW_LONG  8192u
#define HW_SHORT 256u

/* ------------------------------------------------------------- GF(2)[x]/P algebra
 * Elements are stored reflected: bit 31 is the coefficient of x^0, bit 0 of x^31. */
static uint32_t gf_mul_x(uint32_t a) { return (a >> 1) ^ ((a & 1u) ? CRC_POLY_REFLECTED : 0u); }

static uint32_t gf_mul(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < 32; i++) {
        if (a & (0x80000000u >> i)) r ^= b;
        b = gf_mul_x(b);
    }
    return r;
}

/* base^e mod P by square-and-multiply. */
static uint32_t gf_pow(uint32_t base, uint64_t e) {
    uint32_t r = 0x80000000u;               /* x^0 */
    while (e) {
        if (e & 1u) r = gf_mul(r, base);
        base = gf_mul(base, base);
        e >>= 1;
    }
    return r;
}

#define X_POW_8 0x00800000u                 /* x^8: appending one zero byte multiplies the register by it */

/* ---------------------------------------------------------------------- tables */
static uint32_t g_tab[8][256];              /* slicing-by-8 */
static uint32_t g_shift_long[4][256];       /* register -> register after HW_LONG zero bytes */
static uint32_t g_shift_short[4][256];
static atomic_int g_state;                  /* 0 = uninit, 1 = initialising, 2 = ready */

static void shift_table_build(uint32_t t[4][256], size_t zero_bytes) {
    uint32_t k = gf_pow(X_POW_8, zero_bytes);
    for (uint32_t b = 0; b < 4; b++)
        for (uint32_t v = 0; v < 256; v++) t[b][v] = gf_mul(v << (8 * b), k);
}

static void tables_init_slow(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_state, &expected, 1)) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1u) ? (c >> 1) ^ CRC_POLY_REFLECTED : (c >> 1);
            g_tab[0][i] = c;
        }
        for (size_t k = 1; k < 8; k++)
            for (size_t i = 0; i < 256; i++) g_tab[k][i] = (g_tab[k - 1][i] >> 8) ^ g_tab[0][g_tab[k - 1][i] & 0xFFu];
        shift_table_build(g_shift_long, HW_LONG);
        shift_table_build(g_shift_short, HW_SHORT);
        atomic_store(&g_state, 2);
    } else {
        while (atomic_load(&g_state) != 2) { }
    }
}
static void tables_ready(void) { if (NX_UNLIKELY(atomic_load_explicit(&g_state, memory_order_acquire) != 2)) tables_init_slow(); }

/* C11 does not permit adding const to a nested array pointer implicitly. */
static uint32_t shift_apply(uint32_t t[4][256], uint32_t c) {
    return t[0][c & 0xFFu] ^ t[1][(c >> 8) & 0xFFu] ^ t[2][(c >> 16) & 0xFFu] ^ t[3][c >> 24];
}

/* -------------------------------------------------------------------- software */
uint32_t nx_crc32c_sw(uint32_t crc, const void *data, size_t n) {
    if (n == 0) return crc;
    tables_ready();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = ~crc;
    while (n >= 8) {
        uint32_t lo = nx_ld32(p) ^ c, hi = nx_ld32(p + 4);
        c = g_tab[7][lo & 0xFFu] ^ g_tab[6][(lo >> 8) & 0xFFu] ^ g_tab[5][(lo >> 16) & 0xFFu] ^ g_tab[4][lo >> 24] ^
            g_tab[3][hi & 0xFFu] ^ g_tab[2][(hi >> 8) & 0xFFu] ^ g_tab[1][(hi >> 16) & 0xFFu] ^ g_tab[0][hi >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) c = g_tab[0][(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return ~c;
}

/* -------------------------------------------------------------------- hardware */
#if defined(NX_CRC_X86)
NX_TARGET("sse4.2")
static uint32_t crc_hw_impl(uint32_t crc, const uint8_t *p, size_t n) {
    uint64_t c = (uint32_t)~crc;
    while (n >= 3 * (size_t)HW_LONG) {
        uint64_t c0 = c, c1 = 0, c2 = 0;
        for (size_t i = 0; i < HW_LONG; i += 8) {
            c0 = _mm_crc32_u64(c0, nx_ld64(p + i));
            c1 = _mm_crc32_u64(c1, nx_ld64(p + HW_LONG + i));
            c2 = _mm_crc32_u64(c2, nx_ld64(p + 2 * (size_t)HW_LONG + i));
        }
        uint32_t m = shift_apply(g_shift_long, (uint32_t)c0) ^ (uint32_t)c1;
        c = shift_apply(g_shift_long, m) ^ (uint32_t)c2;
        p += 3 * (size_t)HW_LONG;
        n -= 3 * (size_t)HW_LONG;
    }
    while (n >= 3 * (size_t)HW_SHORT) {
        uint64_t c0 = c, c1 = 0, c2 = 0;
        for (size_t i = 0; i < HW_SHORT; i += 8) {
            c0 = _mm_crc32_u64(c0, nx_ld64(p + i));
            c1 = _mm_crc32_u64(c1, nx_ld64(p + HW_SHORT + i));
            c2 = _mm_crc32_u64(c2, nx_ld64(p + 2 * (size_t)HW_SHORT + i));
        }
        uint32_t m = shift_apply(g_shift_short, (uint32_t)c0) ^ (uint32_t)c1;
        c = shift_apply(g_shift_short, m) ^ (uint32_t)c2;
        p += 3 * (size_t)HW_SHORT;
        n -= 3 * (size_t)HW_SHORT;
    }
    while (n >= 8) {
        c = _mm_crc32_u64(c, nx_ld64(p));
        p += 8;
        n -= 8;
    }
    uint32_t c32 = (uint32_t)c;
    while (n--) c32 = _mm_crc32_u8(c32, *p++);
    return ~c32;
}
static bool hw_supported(void) { return nx_cpu()->sse42; }
#  define HW_NAME "hw-sse4.2"
#elif defined(NX_CRC_ARM)
static uint32_t crc_hw_impl(uint32_t crc, const uint8_t *p, size_t n) {
    uint32_t c = ~crc;
    while (n >= 8) {
        c = __crc32cd(c, nx_ld64(p));
        p += 8;
        n -= 8;
    }
    while (n--) c = __crc32cb(c, *p++);
    return ~c;
}
static bool hw_supported(void) { return true; }     /* the feature macro is a compile-time guarantee */
#  define HW_NAME "hw-armv8"
#else
static uint32_t crc_hw_impl(uint32_t crc, const uint8_t *p, size_t n) { return nx_crc32c_sw(crc, p, n); }
static bool hw_supported(void) { return false; }
#  define HW_NAME "sw-slice8"
#endif

bool nx_crc32c_hw_available(void) {
#if defined(NX_CRC_X86) || defined(NX_CRC_ARM)
    return hw_supported();
#else
    return false;
#endif
}

uint32_t nx_crc32c_hw(uint32_t crc, const void *data, size_t n) {
    if (n == 0) return crc;
    if (!nx_crc32c_hw_available()) return nx_crc32c_sw(crc, data, n);
    tables_ready();
    return crc_hw_impl(crc, (const uint8_t *)data, n);
}

/* ------------------------------------------------------------------- dispatch */
static bool dispatch_hw(void) {
    if (!nx_crc32c_hw_available()) return false;
    return nx_simd_level_get() != NX_SIMD_SCALAR;      /* NX_SIMD=scalar forces the software path */
}

uint32_t nx_crc32c(uint32_t crc, const void *data, size_t n) {
    if (n == 0) return crc;
    return dispatch_hw() ? nx_crc32c_hw(crc, data, n) : nx_crc32c_sw(crc, data, n);
}

const char *nx_crc32c_impl_name(void) { return dispatch_hw() ? HW_NAME : "sw-slice8"; }

uint32_t nx_crc32c_combine(uint32_t crc_a, uint32_t crc_b, size_t len_b) {
    if (len_b == 0) return crc_a;
    return gf_mul(crc_a, gf_pow(X_POW_8, len_b)) ^ crc_b;
}
