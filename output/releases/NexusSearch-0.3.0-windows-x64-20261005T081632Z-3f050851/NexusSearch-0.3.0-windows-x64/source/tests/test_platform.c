#include "core/nx_config.h"
#include "nx_test.h"
#include "core/nx_file.h"
#include "core/nx_crc32c.h"

static char test_path[256];

static uint32_t crc_oracle(uint32_t crc, const uint8_t *p, size_t n) {
    uint32_t c = ~crc;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (unsigned b = 0; b < 8; b++) c = (c >> 1) ^ ((c & 1u) ? 0x82f63b78u : 0u);
    }
    return ~c;
}

/* Detect wrong polynomial, complement, streaming seed or interleave shifts. */
static void test_crc_oracle_and_boundaries(void) {
    uint8_t bytes[65568];
    nx_rng rng = nx_test_rng(71);
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)nx_rng_u64(&rng);
    const size_t lens[] = {0, 1, 7, 8, 9, 255, 256, 767, 768, 769, 8191, 8192,
                           24575, 24576, 24577, 49152, 65536};
    NX_CHECK_EQ_U(nx_crc32c(0, "123456789", 9), 0xe3069283u);
    NX_CHECK_EQ_U(nx_crc32c(123, NULL, 0), 123);
    for (size_t a = 0; a < 16; a++) {
        for (size_t j = 0; j < NX_ARRAY_LEN(lens); j++) {
            size_t n = lens[j], split = n / 3;
            uint32_t seed = (uint32_t)nx_rng_u64(&rng);
            uint32_t expected = crc_oracle(seed, bytes + a, n);
            NX_CHECK_EQ_U(nx_crc32c_sw(seed, bytes + a, n), expected);
            NX_CHECK_EQ_U(nx_crc32c_hw(seed, bytes + a, n), expected);
            NX_CHECK_EQ_U(nx_crc32c(seed, bytes + a, n), expected);
            uint32_t left = nx_crc32c(seed, bytes + a, split);
            uint32_t right = nx_crc32c(0, bytes + a + split, n - split);
            NX_CHECK_EQ_U(nx_crc32c_combine(left, right, n - split), expected);
            NX_CHECK_EQ_U(nx_crc32c(left, bytes + a + split, n - split), expected);
            NX_CHECK_EQ_U(nx_crc32c_unmask(nx_crc32c_mask(seed)), seed);
        }
    }
}

/* Detect non-atomic replacement, mutable old mappings, and missing read caps. */
static void test_file_mapping_replacement(void) {
    nx_mmap old = {0}, fresh = {0};
    uint8_t *bytes = NULL;
    size_t n = 0;
    NX_REQUIRE(nx_file_write_atomic(test_path, "old bytes", 9) == NX_OK);
    NX_REQUIRE(nx_mmap_open(test_path, 9, &old) == NX_OK);
    NX_CHECK_EQ_U(old.size, 9);
    NX_CHECK(memcmp(old.data, "old bytes", 9) == 0);
    NX_CHECK_OK(nx_file_write_atomic(test_path, "new", 3));
    NX_CHECK(memcmp(old.data, "old bytes", 9) == 0);
    NX_CHECK_OK(nx_mmap_open(test_path, 3, &fresh));
    NX_CHECK_EQ_U(fresh.size, 3);
    if (fresh.data) NX_CHECK(memcmp(fresh.data, "new", 3) == 0);
    nx_mmap_close(&old); nx_mmap_close(&fresh); nx_mmap_close(&fresh);
    NX_CHECK_EQ_I(nx_mmap_open(test_path, 2, &fresh), NX_ERR_LIMIT);
    NX_CHECK(fresh.data == NULL && fresh.size == 0);
    NX_CHECK_EQ_I(nx_file_read(test_path, 2, &bytes, &n), NX_ERR_LIMIT);
    NX_CHECK(bytes == NULL && n == 0);
    NX_CHECK_OK(nx_file_read(test_path, 3, &bytes, &n));
    NX_CHECK_EQ_U(n, 3);
    if (bytes) NX_CHECK(memcmp(bytes, "new", 3) == 0);
    nx_free(bytes);
    NX_CHECK_OK(nx_file_write_atomic(test_path, NULL, 0));
    NX_CHECK_OK(nx_mmap_open(test_path, 0, &fresh));
    NX_CHECK(fresh.data == NULL && fresh.size == 0);
    nx_mmap_close(&fresh);
    NX_CHECK_OK(nx_file_read(test_path, 0, &bytes, &n));
    NX_CHECK(bytes == NULL && n == 0);
    NX_CHECK_OK(nx_file_remove(test_path));
    NX_CHECK_EQ_I(nx_file_remove(test_path), NX_ERR_NOT_FOUND);
    NX_CHECK_EQ_I(nx_mmap_open(test_path, SIZE_MAX, &fresh), NX_ERR_NOT_FOUND);
}

static nx_status file_oom(void *ctx) {
    const char *path = (const char *)ctx;
    nx_status st = nx_file_write_atomic(path, "oom", 3);
    if (st != NX_OK) return st;
    uint8_t *bytes = NULL; size_t n = 0;
    st = nx_file_read(path, 100, &bytes, &n);
    nx_free(bytes);
    return st;
}
static void test_file_oom_and_invalid(void) {
    nx_mmap m = {0}; uint8_t *p = NULL; size_t n = 0;
    NX_CHECK_EQ_I(nx_file_read(NULL, 0, &p, &n), NX_ERR_INVALID);
    NX_CHECK_EQ_I(nx_file_read(test_path, 0, NULL, &n), NX_ERR_INVALID);
    NX_CHECK_EQ_I(nx_mmap_open(test_path, 0, NULL), NX_ERR_INVALID);
    NX_CHECK_EQ_I(nx_mmap_open("", 0, &m), NX_ERR_INVALID);
    NX_CHECK_EQ_I(nx_file_write_atomic(test_path, NULL, 2), NX_ERR_INVALID);
    nx_test_oom_sweep(file_oom, test_path, 20);
    NX_CHECK_OK(nx_file_remove(test_path));
}

int main(void) {
    (void)snprintf(test_path, sizeof(test_path), "nx-platform-%llu-\xc3\xa9.bin", (unsigned long long)nx_now_ns());
    NX_RUN(test_crc_oracle_and_boundaries);
    NX_RUN(test_file_mapping_replacement);
    NX_RUN(test_file_oom_and_invalid);
    (void)nx_file_remove(test_path);
    return nx_test_summary();
}
