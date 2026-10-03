/* nx_config.h - platform / compiler detection and portability helpers.
 *
 * Every NexusSearch translation unit includes this first. Rules for the whole
 * code base (see docs/CODING_STANDARD.md):
 *   - C11, no VLAs, no GNU statement-expressions in portable code.
 *   - Fixed-width types only for anything sized or persisted. NEVER use `long`
 *     (it is 32-bit on Windows). Use size_t / uint64_t / int64_t.
 *   - Little-endian only (asserted below). On-disk formats are little-endian.
 *   - All unaligned access goes through the nx_ld / nx_st helpers (memcpy based).
 */
#ifndef NX_CONFIG_H
#define NX_CONFIG_H

/* MinGW: use the C99-conforming printf family (%zu, %lld, %llu, %I64-free).
 * Must be defined before ANY system header, so nx_config.h is always the first
 * include of every translation unit (the build also passes it with -D). */
#if defined(__MINGW32__) && !defined(__USE_MINGW_ANSI_STDIO)
#  define __USE_MINGW_ANSI_STDIO 1
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

/* ---- platform -------------------------------------------------------- */
#if defined(_WIN32)
#  define NX_WINDOWS 1
#else
#  define NX_POSIX 1
#  if defined(__APPLE__)
#    define NX_MACOS 1
#  elif defined(__linux__)
#    define NX_LINUX 1
#  endif
#endif

/* ---- compiler -------------------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
#  define NX_GNUC 1
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#  define NX_MSVC 1
#  include <intrin.h>
#endif

/* ---- architecture ---------------------------------------------------- */
#if defined(__x86_64__) || defined(_M_X64)
#  define NX_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define NX_ARM64 1
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#  error "NexusSearch supports little-endian targets only (on-disk formats are little-endian)"
#endif

/* ---- attributes ------------------------------------------------------ */
#if NX_GNUC
#  define NX_LIKELY(x)        __builtin_expect(!!(x), 1)
#  define NX_UNLIKELY(x)      __builtin_expect(!!(x), 0)
#  define NX_NOINLINE         __attribute__((noinline))
#  define NX_ALWAYS_INLINE    static inline __attribute__((always_inline))
#  define NX_UNUSED           __attribute__((unused))
#  define NX_NORETURN         __attribute__((noreturn))
#  if defined(__MINGW32__) && !defined(__clang__)
#    define NX_PRINTF(f, a)   __attribute__((format(gnu_printf, f, a)))   /* %zu etc. (ANSI stdio) */
#  else
#    define NX_PRINTF(f, a)   __attribute__((format(printf, f, a)))
#  endif
#  define NX_PREFETCH(p)      __builtin_prefetch((const void *)(p), 0, 3)
#  define NX_PREFETCH_W(p)    __builtin_prefetch((const void *)(p), 1, 3)
#  define NX_RESTRICT         __restrict__
#  define NX_ALIGNED(n)       __attribute__((aligned(n)))
#  define NX_TARGET(t)        __attribute__((target(t)))
#else
#  define NX_LIKELY(x)        (x)
#  define NX_UNLIKELY(x)      (x)
#  define NX_NOINLINE         __declspec(noinline)
#  define NX_ALWAYS_INLINE    static __forceinline
#  define NX_UNUSED
#  define NX_NORETURN         __declspec(noreturn)
#  define NX_PRINTF(f, a)
#  define NX_PREFETCH(p)      ((void)(p))
#  define NX_PREFETCH_W(p)    ((void)(p))
#  define NX_RESTRICT         __restrict
#  define NX_ALIGNED(n)       __declspec(align(n))
#  define NX_TARGET(t)
#endif

/* Exported symbols for the shared library build (bindings / dlopen users). */
#if defined(NX_BUILD_SHARED)
#  if NX_WINDOWS
#    define NX_API __declspec(dllexport)
#  elif NX_GNUC
#    define NX_API __attribute__((visibility("default")))
#  else
#    define NX_API
#  endif
#elif defined(NX_USE_SHARED) && NX_WINDOWS
#  define NX_API __declspec(dllimport)
#else
#  define NX_API
#endif

#define NX_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

#define NX_CACHELINE 64
#define NX_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define NX_MIN(a, b) ((a) < (b) ? (a) : (b))
#define NX_MAX(a, b) ((a) > (b) ? (a) : (b))
#define NX_CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

NX_STATIC_ASSERT(sizeof(void *) == 8, "NexusSearch requires a 64-bit target");
NX_STATIC_ASSERT(sizeof(size_t) == sizeof(uint64_t), "size_t must be 64-bit");

/* ---- alignment helpers ------------------------------------------------ */
NX_ALWAYS_INLINE size_t nx_align_up(size_t x, size_t a) { return (x + (a - 1)) & ~(a - 1); } /* a: power of 2 */
NX_ALWAYS_INLINE bool nx_is_pow2(uint64_t x) { return x && !(x & (x - 1)); }

/* ---- unaligned little-endian loads/stores (target is LE) -------------- */
NX_ALWAYS_INLINE uint16_t nx_ld16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
NX_ALWAYS_INLINE uint32_t nx_ld32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
NX_ALWAYS_INLINE uint64_t nx_ld64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
NX_ALWAYS_INLINE float    nx_ldf32(const void *p) { float v; memcpy(&v, p, 4); return v; }
NX_ALWAYS_INLINE double   nx_ldf64(const void *p) { double v; memcpy(&v, p, 8); return v; }
NX_ALWAYS_INLINE void nx_st16(void *p, uint16_t v) { memcpy(p, &v, 2); }
NX_ALWAYS_INLINE void nx_st32(void *p, uint32_t v) { memcpy(p, &v, 4); }
NX_ALWAYS_INLINE void nx_st64(void *p, uint64_t v) { memcpy(p, &v, 8); }
NX_ALWAYS_INLINE void nx_stf32(void *p, float v) { memcpy(p, &v, 4); }
NX_ALWAYS_INLINE void nx_stf64(void *p, double v) { memcpy(p, &v, 8); }

/* ---- bit tricks -------------------------------------------------------- */
/* popcount */
NX_ALWAYS_INLINE int nx_popcnt64(uint64_t x) {
#if NX_GNUC
    return __builtin_popcountll(x);
#elif NX_MSVC && defined(NX_X86_64)
    return (int)__popcnt64(x);
#else
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
#endif
}
/* count trailing zeros; x MUST be non-zero */
NX_ALWAYS_INLINE int nx_ctz64(uint64_t x) {
#if NX_GNUC
    return __builtin_ctzll(x);
#elif NX_MSVC
    unsigned long i; _BitScanForward64(&i, x); return (int)i;
#else
    int n = 0; while (!(x & 1)) { x >>= 1; n++; } return n;
#endif
}
/* count leading zeros; x MUST be non-zero */
NX_ALWAYS_INLINE int nx_clz64(uint64_t x) {
#if NX_GNUC
    return __builtin_clzll(x);
#elif NX_MSVC
    unsigned long i; _BitScanReverse64(&i, x); return 63 - (int)i;
#else
    int n = 0; while (!(x & (1ULL << 63))) { x <<= 1; n++; } return n;
#endif
}
/* floor(log2(x)); x MUST be non-zero */
NX_ALWAYS_INLINE int nx_log2_64(uint64_t x) { return 63 - nx_clz64(x); }
/* number of bits needed to represent x (0 -> 0) */
NX_ALWAYS_INLINE int nx_bit_width64(uint64_t x) { return x ? 64 - nx_clz64(x) : 0; }

/* ---- saturating / checked arithmetic (size computations from untrusted input) */
NX_ALWAYS_INLINE bool nx_mul_overflow(size_t a, size_t b, size_t *out) {
#if NX_GNUC
    return __builtin_mul_overflow(a, b, out);
#else
    if (a && b > SIZE_MAX / a) return true;
    *out = a * b; return false;
#endif
}
NX_ALWAYS_INLINE bool nx_add_overflow(size_t a, size_t b, size_t *out) {
#if NX_GNUC
    return __builtin_add_overflow(a, b, out);
#else
    if (a > SIZE_MAX - b) return true;
    *out = a + b; return false;
#endif
}

#endif /* NX_CONFIG_H */
