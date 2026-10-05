/* nx_crc32c.h - CRC-32C (Castagnoli, polynomial 0x1EDC6F41, reflected) checksums.
 *
 * Used for every on-disk integrity check (segment sections, WAL records, manifests).
 *
 * Conventions (the standard "CRC-32C" as used by iSCSI, ext4, LevelDB, Btrfs):
 *   - initial value 0, final XOR handled internally, so
 *         nx_crc32c(0, "123456789", 9) == 0xE3069283
 *   - the function is its own STREAMING API: feeding the previous result back as
 *     `crc` continues the checksum, so
 *         nx_crc32c(nx_crc32c(0, a, na), b, nb) == nx_crc32c(0, a||b, na+nb)
 *   - nx_crc32c(c, NULL, 0) == c  (n == 0 never dereferences data)
 *
 * Implementations (all produce identical results, verified against a bitwise
 * oracle in tests/test_platform.c):
 *   nx_crc32c_sw   portable slicing-by-8 (no measured throughput guarantee)
 *   nx_crc32c_hw   SSE4.2 _mm_crc32_u64 with 3-way interleaving recombined with
 *                  carry-less-multiply-free shift tables (x86-64), or the ARMv8
 *                  __crc32cd instruction (arm64 when compiled with
 *                  __ARM_FEATURE_CRC32). Falls back to the software path when the
 *                  CPU/compiler lacks it.
 *   nx_crc32c      runtime dispatch (honours the NX_SIMD=scalar cap of nx_simd.h).
 *
 * Thread-safety: every function is thread-safe and allocation-free. The lookup
 * tables are built once on first use by a thread-safe, idempotent initialiser (a
 * documented singleton in the sense of docs/CODING_STANDARD.md).
 */
#ifndef NX_CRC32C_H
#define NX_CRC32C_H

#include "nx_config.h"

NX_API uint32_t nx_crc32c(uint32_t crc, const void *data, size_t n);

/* Portable slicing-by-8 implementation (always available). */
NX_API uint32_t nx_crc32c_sw(uint32_t crc, const void *data, size_t n);

/* True when the hardware CRC path is compiled in AND supported by this CPU
 * (independent of any NX_SIMD cap, so tests can always compare it to the oracle). */
NX_API bool nx_crc32c_hw_available(void);
/* Hardware implementation; silently uses the software path if !nx_crc32c_hw_available(). */
NX_API uint32_t nx_crc32c_hw(uint32_t crc, const void *data, size_t n);

/* Name of the implementation nx_crc32c() currently dispatches to ("hw-sse4.2", "hw-armv8", "sw-slice8"). */
NX_API const char *nx_crc32c_impl_name(void);

/* CRC of the concatenation A||B from crc(A), crc(B) and len(B) - O(log len), no data access.
 * Lets large sections be checksummed in parallel chunks and merged. */
NX_API uint32_t nx_crc32c_combine(uint32_t crc_a, uint32_t crc_b, size_t len_b);

/* LevelDB-style masking: store masked CRCs inside data that is itself checksummed, so a
 * CRC of a string that embeds CRCs does not degenerate. unmask(mask(c)) == c. */
NX_ALWAYS_INLINE uint32_t nx_crc32c_mask(uint32_t crc) { return ((crc >> 15) | (crc << 17)) + 0xA282EAD8u; }
NX_ALWAYS_INLINE uint32_t nx_crc32c_unmask(uint32_t masked) {
    uint32_t r = masked - 0xA282EAD8u;
    return (r >> 17) | (r << 15);
}

#endif /* NX_CRC32C_H */
