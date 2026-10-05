/* nx_hash.h - non-cryptographic hashing and PRNG.
 *
 * STABILITY CONTRACT: nx_hash64 / nx_hash32 / nx_mix64 with a fixed seed are
 * part of the on-disk format (hash-bucketed dictionaries, bloom filters,
 * vector-embedder fingerprints). Their output must never change. Per-process
 * randomised seeds are for in-memory hash tables only (hash-flood resistance).
 */
#ifndef NX_HASH_H
#define NX_HASH_H

#include "nx_config.h"

/* 64-bit finaliser (murmur3 fmix64). Bijective. */
NX_ALWAYS_INLINE uint64_t nx_mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

NX_ALWAYS_INLINE uint64_t nx_rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

/* Byte-string hash: 8 bytes/step multiply-rotate, murmur3-style finalisation. */
NX_ALWAYS_INLINE uint64_t nx_hash64(const void *data, size_t n, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = seed ^ (0x9E3779B97F4A7C15ULL * (uint64_t)(n + 1));
    while (n >= 8) {
        uint64_t w = nx_ld64(p);
        h = nx_rotl64(h ^ (w * 0x87C37B91114253D5ULL), 31) * 0x4CF5AD432745937FULL;
        p += 8; n -= 8;
    }
    uint64_t t = 0;
    for (size_t i = 0; i < n; i++) t |= (uint64_t)p[i] << (8 * i);
    h = nx_rotl64(h ^ (t * 0x87C37B91114253D5ULL), 31) * 0x4CF5AD432745937FULL;
    return nx_mix64(h);
}
NX_ALWAYS_INLINE uint32_t nx_hash32(const void *data, size_t n, uint64_t seed) {
    uint64_t h = nx_hash64(data, n, seed);
    return (uint32_t)(h ^ (h >> 32));
}
NX_ALWAYS_INLINE uint64_t nx_hash_combine(uint64_t a, uint64_t b) { return nx_mix64(a ^ (b + 0x9E3779B97F4A7C15ULL + (a << 6) + (a >> 2))); }

/* ---- PRNG: splitmix64 seeding + xoshiro256** --------------------------- */
typedef struct nx_rng { uint64_t s[4]; } nx_rng;

NX_ALWAYS_INLINE uint64_t nx_splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
NX_ALWAYS_INLINE void nx_rng_seed(nx_rng *r, uint64_t seed) {
    uint64_t sm = seed;
    for (int i = 0; i < 4; i++) r->s[i] = nx_splitmix64(&sm);
}
NX_ALWAYS_INLINE uint64_t nx_rng_u64(nx_rng *r) {
    uint64_t *s = r->s;
    uint64_t result = nx_rotl64(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t;
    s[3] = nx_rotl64(s[3], 45);
    return result;
}
/* uniform in [0, n) without modulo bias (Lemire, using 128-bit-free rejection) */
NX_ALWAYS_INLINE uint64_t nx_rng_below(nx_rng *r, uint64_t n) {
    if (n <= 1) return 0;
    uint64_t limit = UINT64_MAX - (UINT64_MAX % n);
    uint64_t v;
    do { v = nx_rng_u64(r); } while (v >= limit);
    return v % n;
}
/* uniform double in [0, 1) */
NX_ALWAYS_INLINE double nx_rng_f64(nx_rng *r) { return (double)(nx_rng_u64(r) >> 11) * (1.0 / 9007199254740992.0); }
NX_ALWAYS_INLINE float nx_rng_f32(nx_rng *r) { return (float)(nx_rng_u64(r) >> 40) * (1.0f / 16777216.0f); }

#endif /* NX_HASH_H */
