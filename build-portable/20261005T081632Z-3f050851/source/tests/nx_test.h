/* nx_test.h - tiny single-header test harness (no dependencies).
 *
 *   static void test_foo(void) { NX_CHECK(1 + 1 == 2); }
 *   int main(void) { NX_RUN(test_foo); return nx_test_summary(); }
 *
 * Every test is wrapped with leak detection (live allocation count must return
 * to its starting value) and heap-canary verification, because this toolchain
 * has no AddressSanitizer. Environment knobs:
 *   NX_TEST_SEED=<n>    seed for nx_test_rng() (printed on failure)
 *   NX_TEST_SCALE=<n>   multiplies iteration counts from nx_test_iters()
 *   NX_TEST_FILTER=<s>  only run tests whose name contains <s>
 */
#ifndef NX_TEST_H
#define NX_TEST_H

#include "core/nx_config.h"   /* must precede system headers (MinGW printf mode) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "core/nx_mem.h"
#include "core/nx_hash.h"
#include "core/nx_status.h"
#include "core/nx_thread.h"

static int nx_t_total, nx_t_failed_tests, nx_t_checks, nx_t_cur_failed;
static uint64_t nx_t_seed_v;

static uint64_t nx_test_seed(void) {
    if (!nx_t_seed_v) {
        const char *s = getenv("NX_TEST_SEED");
        nx_t_seed_v = s ? strtoull(s, NULL, 0) : 0x5EEDC0DEULL;
        if (!nx_t_seed_v) nx_t_seed_v = 1;
    }
    return nx_t_seed_v;
}
static NX_UNUSED size_t nx_test_iters(size_t base) {
    const char *s = getenv("NX_TEST_SCALE");
    size_t k = s ? (size_t)strtoull(s, NULL, 0) : 1;
    return base * (k ? k : 1);
}
/* A fresh deterministic RNG; `salt` separates streams inside one test. */
static NX_UNUSED nx_rng nx_test_rng(uint64_t salt) { nx_rng r; nx_rng_seed(&r, nx_test_seed() ^ (salt * 0x9E3779B97F4A7C15ULL)); return r; }

static void nx_test_fail_at(const char *file, int line, const char *what) {
    nx_t_cur_failed++;
    fprintf(stderr, "    FAIL %s:%d: %s\n", file, line, what);
}

#define NX_CHECK(c) do { nx_t_checks++; if (!(c)) nx_test_fail_at(__FILE__, __LINE__, #c); } while (0)
#define NX_REQUIRE(c) do { nx_t_checks++; if (!(c)) { nx_test_fail_at(__FILE__, __LINE__, #c); return; } } while (0)
#define NX_CHECK_MSG(c, ...) do { nx_t_checks++; if (!(c)) { char nx_m_[256]; snprintf(nx_m_, sizeof nx_m_, __VA_ARGS__); nx_test_fail_at(__FILE__, __LINE__, nx_m_); } } while (0)
#define NX_CHECK_EQ_I(a, b) do { long long nx_a_ = (long long)(a), nx_b_ = (long long)(b); nx_t_checks++; \
    if (nx_a_ != nx_b_) { char nx_m_[256]; snprintf(nx_m_, sizeof nx_m_, "%s == %s  (got %lld vs %lld)", #a, #b, nx_a_, nx_b_); nx_test_fail_at(__FILE__, __LINE__, nx_m_); } } while (0)
#define NX_CHECK_EQ_U(a, b) do { unsigned long long nx_a_ = (unsigned long long)(a), nx_b_ = (unsigned long long)(b); nx_t_checks++; \
    if (nx_a_ != nx_b_) { char nx_m_[256]; snprintf(nx_m_, sizeof nx_m_, "%s == %s  (got %llu vs %llu)", #a, #b, nx_a_, nx_b_); nx_test_fail_at(__FILE__, __LINE__, nx_m_); } } while (0)
#define NX_CHECK_EQ_S(a, b) do { const char *nx_a_ = (a), *nx_b_ = (b); nx_t_checks++; \
    if (!nx_a_ || !nx_b_ || strcmp(nx_a_, nx_b_) != 0) { char nx_m_[512]; snprintf(nx_m_, sizeof nx_m_, "%s == %s  (got \"%s\" vs \"%s\")", #a, #b, nx_a_ ? nx_a_ : "(null)", nx_b_ ? nx_b_ : "(null)"); nx_test_fail_at(__FILE__, __LINE__, nx_m_); } } while (0)
#define NX_CHECK_NEAR(a, b, eps) do { double nx_a_ = (double)(a), nx_b_ = (double)(b); nx_t_checks++; \
    if (!(fabs(nx_a_ - nx_b_) <= (double)(eps))) { char nx_m_[256]; snprintf(nx_m_, sizeof nx_m_, "%s ~= %s  (got %.9g vs %.9g)", #a, #b, nx_a_, nx_b_); nx_test_fail_at(__FILE__, __LINE__, nx_m_); } } while (0)
#define NX_CHECK_OK(st) do { nx_status nx_s_ = (st); nx_t_checks++; \
    if (nx_s_ != NX_OK) { char nx_m_[256]; snprintf(nx_m_, sizeof nx_m_, "%s == NX_OK (got %s)", #st, nx_status_str(nx_s_)); nx_test_fail_at(__FILE__, __LINE__, nx_m_); } } while (0)

static void nx_test_run(const char *name, void (*fn)(void)) {
    const char *filter = getenv("NX_TEST_FILTER");
    if (filter && !strstr(name, filter)) return;
    nx_t_total++;
    nx_t_cur_failed = 0;
    nx_mem_stats before = nx_mem_get_stats();
    uint64_t t0 = nx_now_ns();
    fn();
    uint64_t t1 = nx_now_ns();
    nx_mem_stats after = nx_mem_get_stats();
    if (nx_mem_check_all() != 0) { nx_t_cur_failed++; fprintf(stderr, "    FAIL heap canary corrupted\n"); }
    if (after.live_allocs != before.live_allocs) {
        nx_t_cur_failed++;
        fprintf(stderr, "    FAIL leak: %lld allocation(s), %lld bytes still live after test\n",
                (long long)(after.live_allocs - before.live_allocs), (long long)(after.live_bytes - before.live_bytes));
        nx_mem_dump_leaks(NULL);
    }
    if (nx_t_cur_failed) { nx_t_failed_tests++; fprintf(stderr, "[FAIL] %s  (reproduce: NX_TEST_SEED=%llu)\n", name, (unsigned long long)nx_test_seed()); }
    else printf("[ OK ] %-52s %8.2f ms\n", name, (double)(t1 - t0) / 1e6);
    fflush(stdout);
}
#define NX_RUN(fn) nx_test_run(#fn, fn)

static NX_UNUSED int nx_test_summary(void) {
    printf("\n%d test(s), %d failed, %d check(s)\n", nx_t_total, nx_t_failed_tests, nx_t_checks);
    return nx_t_failed_tests ? 1 : 0;
}

/* OOM sweep: run self-contained scenario f(ctx) with the n-th allocation forced
 * to fail, for n = 0,1,2,... until a run completes without hitting the injected
 * failure. f must clean up everything it allocated whether it succeeds or not,
 * and must return NX_ERR_NOMEM (or NX_OK if it legitimately degraded). */
typedef nx_status (*nx_oom_fn)(void *ctx);
static NX_UNUSED void nx_test_oom_sweep(nx_oom_fn f, void *ctx, int max_n) {
    for (int n = 0; n < max_n; n++) {
        nx_mem_stats b = nx_mem_get_stats();
        uint64_t c0 = nx_mem_alloc_counter();
        nx_mem_fail_after(n);
        nx_status st = f(ctx);
        nx_mem_fail_after(-1);
        uint64_t used = nx_mem_alloc_counter() - c0;
        nx_mem_stats a = nx_mem_get_stats();
        NX_CHECK_MSG(a.live_allocs == b.live_allocs, "OOM sweep n=%d leaked %lld allocation(s)", n, (long long)(a.live_allocs - b.live_allocs));
        if ((uint64_t)n >= used) { NX_CHECK_MSG(st == NX_OK, "OOM sweep: scenario failed (%s) with no injected failure", nx_status_str(st)); return; }
        NX_CHECK_MSG(st == NX_ERR_NOMEM || st == NX_OK, "OOM sweep n=%d returned %s (expected NOMEM)", n, nx_status_str(st));
    }
    NX_CHECK_MSG(0, "OOM sweep exceeded max_n=%d allocations", max_n);
}

#endif /* NX_TEST_H */
