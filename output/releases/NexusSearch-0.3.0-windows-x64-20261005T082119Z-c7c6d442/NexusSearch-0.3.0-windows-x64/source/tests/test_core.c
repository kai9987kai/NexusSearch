/* Tests for the foundation layer: mem, arena, buf/cursor, hash/rng, threads. */
#include "nx_test.h"
#include "core/nx_arena.h"
#include "core/nx_buf.h"

/* ---------------------------------------------------------------- nx_mem */
static void test_mem_basic(void) {
    nx_mem_stats s0 = nx_mem_get_stats();
    void *p = nx_malloc(100);
    NX_REQUIRE(p);
    NX_CHECK(((uintptr_t)p & 15) == 0);
    nx_mem_stats s1 = nx_mem_get_stats();
    NX_CHECK_EQ_U(s1.live_allocs, s0.live_allocs + 1);
    NX_CHECK_EQ_U(s1.live_bytes, s0.live_bytes + 100);
    memset(p, 7, 100);
    p = nx_realloc(p, 5000);
    NX_REQUIRE(p);
    NX_CHECK(((unsigned char *)p)[99] == 7);
    nx_free(p);
    nx_free(NULL);
    NX_CHECK_EQ_U(nx_mem_get_stats().live_allocs, s0.live_allocs);
}

static void test_mem_aligned(void) {
    for (size_t al = 16; al <= 4096; al *= 2) {
        void *p = nx_aligned_alloc(al, 1000 + al);
        NX_REQUIRE(p);
        NX_CHECK_MSG(((uintptr_t)p & (al - 1)) == 0, "alignment %zu", al);
        memset(p, 1, 1000 + al);
        nx_free(p);
    }
    NX_CHECK(nx_aligned_alloc(24, 10) == NULL);      /* non power of two */
}

static void test_mem_calloc_overflow(void) {
    NX_CHECK(nx_calloc(SIZE_MAX / 2, 4) == NULL);
    void *z = nx_calloc(64, 8);
    NX_REQUIRE(z);
    for (int i = 0; i < 512; i++) NX_CHECK(((unsigned char *)z)[i] == 0);
    nx_free(z);
}

static void test_mem_fail_injection(void) {
    nx_mem_fail_after(2);
    void *a = nx_malloc(8), *b = nx_malloc(8), *c = nx_malloc(8), *d = nx_malloc(8);
    nx_mem_fail_after(-1);
    NX_CHECK(a && b && c == NULL && d);                 /* exactly the 3rd fails */
    nx_free(a); nx_free(b); nx_free(d);
}

static void test_mem_canary_detects_nothing_when_clean(void) {
    char *p = (char *)nx_malloc(32);
    NX_REQUIRE(p);
    memset(p, 'x', 32);                                 /* exactly in bounds */
    NX_CHECK_EQ_U(nx_mem_check_all(), 0);
    nx_free(p);
}

static void test_strdup(void) {
    char *s = nx_strdup("hello");
    NX_REQUIRE(s);
    NX_CHECK_EQ_S(s, "hello");
    nx_free(s);
    s = nx_strndup("hello world", 5);
    NX_REQUIRE(s);
    NX_CHECK_EQ_S(s, "hello");
    nx_free(s);
}

/* ------------------------------------------------------------- nx_arena */
static void test_arena(void) {
    nx_arena a;
    nx_arena_init(&a, 256);
    char *s = nx_arena_strdup(&a, "abc");
    NX_REQUIRE(s);
    NX_CHECK_EQ_S(s, "abc");
    for (int i = 1; i <= 50; i++) {
        size_t al = (size_t)1 << (i % 7);
        void *p = nx_arena_alloc(&a, (size_t)i * 13, al);
        NX_REQUIRE(p);
        NX_CHECK(((uintptr_t)p & (al - 1)) == 0);
        memset(p, 0xAB, (size_t)i * 13);
    }
    void *big = nx_arena_alloc(&a, 100000, 64);        /* oversize -> dedicated chunk */
    NX_REQUIRE(big);
    NX_CHECK(((uintptr_t)big & 63) == 0);
    memset(big, 1, 100000);
    NX_CHECK_EQ_S(s, "abc");                            /* earlier allocations survive */
    nx_arena_mark m = nx_arena_save(&a);
    for (int i = 0; i < 100; i++) NX_CHECK(nx_arena_alloc(&a, 500, 8) != NULL);
    nx_arena_restore(&a, m);
    NX_CHECK_EQ_S(s, "abc");
    NX_CHECK(!a.oom);
    nx_arena_reset(&a);
    NX_CHECK(nx_arena_alloc(&a, 10, 8) != NULL);
    nx_arena_free(&a);
}

static nx_status arena_oom_scenario(void *ctx) {
    (void)ctx;
    nx_arena a;
    nx_arena_init(&a, 128);
    for (int i = 0; i < 20; i++) nx_arena_alloc(&a, 90, 8);
    bool oom = a.oom;
    nx_arena_free(&a);
    return oom ? NX_ERR_NOMEM : NX_OK;
}
static void test_arena_oom(void) { nx_test_oom_sweep(arena_oom_scenario, NULL, 100); }

/* --------------------------------------------------------------- nx_buf */
static void test_buf_put_and_read(void) {
    nx_buf b;
    nx_buf_init(&b);
    nx_buf_put_u8(&b, 0xAB);
    nx_buf_put_u16(&b, 0x1234);
    nx_buf_put_u32(&b, 0xDEADBEEF);
    nx_buf_put_u64(&b, 0x0102030405060708ULL);
    nx_buf_put_f32(&b, 3.5f);
    nx_buf_put_f64(&b, -2.25);
    nx_buf_put_varint(&b, 0);
    nx_buf_put_varint(&b, 127);
    nx_buf_put_varint(&b, 128);
    nx_buf_put_varint(&b, UINT64_MAX);
    nx_buf_put_cstr(&b, "hi");
    NX_REQUIRE(!b.oom);

    nx_cursor c = nx_cursor_make(nx_buf_slice(&b));
    NX_CHECK_EQ_U(nx_rd_u8(&c), 0xAB);
    NX_CHECK_EQ_U(nx_rd_u16(&c), 0x1234);
    NX_CHECK_EQ_U(nx_rd_u32(&c), 0xDEADBEEFu);
    NX_CHECK_EQ_U(nx_rd_u64(&c), 0x0102030405060708ULL);
    NX_CHECK_NEAR(nx_rd_f32(&c), 3.5, 0);
    NX_CHECK_NEAR(nx_rd_f64(&c), -2.25, 0);
    NX_CHECK_EQ_U(nx_rd_varint(&c), 0);
    NX_CHECK_EQ_U(nx_rd_varint(&c), 127);
    NX_CHECK_EQ_U(nx_rd_varint(&c), 128);
    NX_CHECK_EQ_U(nx_rd_varint(&c), UINT64_MAX);
    nx_slice s = nx_rd_bytes(&c, 3);
    NX_CHECK_EQ_S((const char *)s.p, "hi");
    NX_CHECK(!c.err);
    NX_CHECK_EQ_U(nx_cursor_left(&c), 0);
    nx_rd_u8(&c);                                       /* overrun -> sticky error, returns 0 */
    NX_CHECK(c.err);
    NX_CHECK_EQ_U(nx_rd_u32(&c), 0);
    nx_buf_free(&b);
}

static void test_buf_patch_pad_printf(void) {
    nx_buf b;
    nx_buf_init(&b);
    nx_buf_put_u32(&b, 0);
    nx_buf_put_u8(&b, 1);
    nx_buf_pad_to(&b, 16);
    NX_CHECK_EQ_U(b.len, 16);
    nx_buf_patch_u32(&b, 0, 0xCAFEBABEu);
    NX_CHECK_EQ_U(nx_ld32(b.data), 0xCAFEBABEu);
    nx_buf_clear(&b);
    nx_buf_printf(&b, "%d-%s-%05.1f", 42, "x", 3.14159);
    NX_CHECK_EQ_S(nx_buf_cstr(&b), "42-x-003.1");
    NX_CHECK_EQ_U(b.len, 10);
    size_t n = 0;
    uint8_t *d = nx_buf_detach(&b, &n);
    NX_CHECK(d != NULL && n == 10);
    nx_free(d);
    nx_buf_free(&b);
}

static void test_varint_roundtrip_fuzz(void) {
    nx_rng r = nx_test_rng(1);
    nx_buf b;
    nx_buf_init(&b);
    size_t N = nx_test_iters(5000);
    uint64_t *vals = NX_NEW_ARRAY(uint64_t, N);
    NX_REQUIRE(vals);
    for (size_t i = 0; i < N; i++) {
        uint64_t v = nx_rng_u64(&r) >> (nx_rng_u64(&r) % 64);
        vals[i] = v;
        size_t before = b.len;
        nx_buf_put_varint(&b, v);
        NX_CHECK_EQ_U(b.len - before, nx_varint_size(v));
    }
    nx_cursor c = nx_cursor_make(nx_buf_slice(&b));
    for (size_t i = 0; i < N; i++) NX_CHECK_EQ_U(nx_rd_varint(&c), vals[i]);
    NX_CHECK(!c.err && nx_cursor_left(&c) == 0);
    nx_free(vals);
    nx_buf_free(&b);
}

static void test_cursor_malformed_varints(void) {
    /* 11 continuation bytes */
    uint8_t bad1[11]; memset(bad1, 0xFF, sizeof bad1);
    nx_cursor c = nx_cursor_make(nx_slice_make(bad1, sizeof bad1));
    nx_rd_varint(&c);
    NX_CHECK(c.err);
    /* 10th byte with high payload bits set -> overflow of 64 bits */
    uint8_t bad2[10]; memset(bad2, 0xFF, 9); bad2[9] = 0x7F;
    c = nx_cursor_make(nx_slice_make(bad2, sizeof bad2));
    nx_rd_varint(&c);
    NX_CHECK(c.err);
    /* truncated */
    uint8_t bad3[2] = { 0x80, 0x80 };
    c = nx_cursor_make(nx_slice_make(bad3, sizeof bad3));
    nx_rd_varint(&c);
    NX_CHECK(c.err);
    /* huge byte request never overflows pointer arithmetic */
    uint8_t ok[4] = { 1, 2, 3, 4 };
    c = nx_cursor_make(nx_slice_make(ok, 4));
    nx_slice s = nx_rd_bytes(&c, SIZE_MAX);
    NX_CHECK(s.p == NULL && s.n == 0 && c.err);
    c = nx_cursor_make(nx_slice_make(ok, 4));
    NX_CHECK(!nx_rd_seek(&c, 5));
    NX_CHECK(c.err);
}

static void test_slice_sub(void) {
    nx_slice s = nx_slice_cstr("abcdef");
    nx_slice t = nx_slice_sub(s, 2, 3);
    NX_CHECK(t.n == 3 && memcmp(t.p, "cde", 3) == 0);
    NX_CHECK(nx_slice_sub(s, 4, 3).n == 0);
    NX_CHECK(nx_slice_sub(s, 7, 0).n == 0);
    NX_CHECK(nx_slice_sub(s, SIZE_MAX, 2).n == 0);
}

static nx_status buf_oom_scenario(void *ctx) {
    (void)ctx;
    nx_buf b;
    nx_buf_init(&b);
    for (int i = 0; i < 2000; i++) nx_buf_put_u32(&b, (uint32_t)i);
    nx_buf_printf(&b, "%s", "tail");
    bool oom = b.oom;
    nx_buf_free(&b);
    return oom ? NX_ERR_NOMEM : NX_OK;
}
static void test_buf_oom(void) { nx_test_oom_sweep(buf_oom_scenario, NULL, 100); }

/* ----------------------------------------------------------- hash / rng */
static void test_hash_known_stability(void) {
    /* These values are part of the on-disk contract. If this test fails you
     * have changed the hash: you must bump the index format version instead. */
    NX_CHECK_EQ_U(nx_hash64("", 0, 0), 0x3f4f1f25ba6b4a09ULL);
    NX_CHECK_EQ_U(nx_hash64("nexus", 5, 0), 0xf444c668d7c67686ULL);
    NX_CHECK_EQ_U(nx_hash64("hello world", 11, 42), 0xeb21c71368492a6dULL);
    {
        unsigned char b[32];
        for (int i = 0; i < 32; i++) b[i] = (unsigned char)i;
        NX_CHECK_EQ_U(nx_hash64(b, 32, 7), 0x105070b8e66efc5aULL);
    }
    NX_CHECK_EQ_U(nx_mix64(1), 0xb456bcfc34c2cb2cULL);
    {
        nx_rng g; nx_rng_seed(&g, 42);
        NX_CHECK_EQ_U(nx_rng_u64(&g), 0x15780b2e0c2ec716ULL);   /* deterministic test data: pinned */
        NX_CHECK_EQ_U(nx_rng_u64(&g), 0x6104d9866d113a7eULL);
    }
    uint64_t h1 = nx_hash64("nexus", 5, 0), h2 = nx_hash64("nexus", 5, 0), h3 = nx_hash64("nexus", 5, 1);
    NX_CHECK(h1 == h2 && h1 != h3);
    NX_CHECK(nx_hash64("a", 1, 0) != nx_hash64("b", 1, 0));
    NX_CHECK(nx_hash64("ab", 2, 0) != nx_hash64("ba", 2, 0));
    NX_CHECK(nx_hash64("a\0", 2, 0) != nx_hash64("a", 1, 0));       /* length is mixed in */
    NX_CHECK(nx_mix64(0) == 0);                                      /* fmix64 fixed point */
}

static void test_hash_distribution(void) {
    /* 200k sequential keys into 1024 buckets: chi-square must be sane. */
    enum { B = 1024 };
    static uint32_t cnt[B];
    memset(cnt, 0, sizeof cnt);
    const size_t N = 200000;
    for (size_t i = 0; i < N; i++) {
        char key[24];
        int n = snprintf(key, sizeof key, "term-%zu", i);
        cnt[nx_hash64(key, (size_t)n, 0) & (B - 1)]++;
    }
    double e = (double)N / B, chi = 0;
    for (int i = 0; i < B; i++) chi += ((cnt[i] - e) * (cnt[i] - e)) / e;
    NX_CHECK_MSG(chi < 1300.0, "chi-square %.1f too high (df=1023)", chi);
}

static void test_rng(void) {
    nx_rng a, b;
    nx_rng_seed(&a, 42); nx_rng_seed(&b, 42);
    for (int i = 0; i < 100; i++) NX_CHECK(nx_rng_u64(&a) == nx_rng_u64(&b));
    nx_rng r = nx_test_rng(9);
    uint32_t hist[10] = { 0 };
    for (int i = 0; i < 100000; i++) {
        uint64_t v = nx_rng_below(&r, 10);
        NX_REQUIRE(v < 10);
        hist[v]++;
        double f = nx_rng_f64(&r);
        NX_REQUIRE(f >= 0.0 && f < 1.0);
    }
    for (int i = 0; i < 10; i++) NX_CHECK_MSG(hist[i] > 9000 && hist[i] < 11000, "bucket %d = %u", i, hist[i]);
}

static void test_bits(void) {
    NX_CHECK_EQ_I(nx_popcnt64(0), 0);
    NX_CHECK_EQ_I(nx_popcnt64(UINT64_MAX), 64);
    NX_CHECK_EQ_I(nx_ctz64(1), 0);
    NX_CHECK_EQ_I(nx_ctz64(1ULL << 63), 63);
    NX_CHECK_EQ_I(nx_clz64(1), 63);
    NX_CHECK_EQ_I(nx_clz64(1ULL << 63), 0);
    NX_CHECK_EQ_I(nx_log2_64(1000), 9);
    NX_CHECK_EQ_I(nx_bit_width64(0), 0);
    NX_CHECK_EQ_I(nx_bit_width64(255), 8);
    NX_CHECK_EQ_I(nx_bit_width64(256), 9);
    size_t o;
    NX_CHECK(nx_mul_overflow(SIZE_MAX, 2, &o));
    NX_CHECK(!nx_mul_overflow(1000, 1000, &o) && o == 1000000);
    NX_CHECK(nx_add_overflow(SIZE_MAX, 1, &o));
    NX_CHECK_EQ_U(nx_align_up(13, 8), 16);
    NX_CHECK_EQ_U(nx_align_up(16, 8), 16);
}

/* -------------------------------------------------------------- threads */
typedef struct { nx_mutex mu; int counter; atomic_int atom; } shared_t;
static void inc_task(void *a) {
    shared_t *s = (shared_t *)a;
    for (int i = 0; i < 1000; i++) {
        nx_mutex_lock(&s->mu); s->counter++; nx_mutex_unlock(&s->mu);
        atomic_fetch_add(&s->atom, 1);
    }
}

static void test_threads_and_mutex(void) {
    shared_t s; memset(&s, 0, sizeof s);
    nx_mutex_init(&s.mu);
    nx_thread th[6];
    for (int i = 0; i < 6; i++) NX_REQUIRE(nx_thread_start(&th[i], inc_task, &s));
    for (int i = 0; i < 6; i++) nx_thread_join(&th[i]);
    NX_CHECK_EQ_I(s.counter, 6000);
    NX_CHECK_EQ_I(atomic_load(&s.atom), 6000);
    NX_CHECK(nx_mutex_trylock(&s.mu));
    nx_mutex_unlock(&s.mu);
    nx_mutex_destroy(&s.mu);
}

typedef struct { nx_mutex mu; nx_cond cv; int ready; } cv_t;
static void cv_setter(void *a) { cv_t *c = (cv_t *)a; nx_sleep_ms(20); nx_mutex_lock(&c->mu); c->ready = 1; nx_cond_broadcast(&c->cv); nx_mutex_unlock(&c->mu); }
static void test_cond_and_timeout(void) {
    cv_t c; memset(&c, 0, sizeof c);
    nx_mutex_init(&c.mu); nx_cond_init(&c.cv);
    nx_mutex_lock(&c.mu);
    uint64_t t0 = nx_now_ns();
    bool signalled = nx_cond_timedwait(&c.cv, &c.mu, 30);     /* nobody signals: must time out */
    uint64_t dt = nx_now_ns() - t0;
    NX_CHECK(!signalled);
    NX_CHECK_MSG(dt >= 20ull * 1000000, "timedwait returned after only %llu ns", (unsigned long long)dt);
    nx_mutex_unlock(&c.mu);
    nx_thread th;
    NX_REQUIRE(nx_thread_start(&th, cv_setter, &c));
    nx_mutex_lock(&c.mu);
    while (!c.ready) nx_cond_wait(&c.cv, &c.mu);
    nx_mutex_unlock(&c.mu);
    nx_thread_join(&th);
    nx_cond_destroy(&c.cv); nx_mutex_destroy(&c.mu);
}

static void test_rwlock(void) {
    nx_rwlock l; nx_rwlock_init(&l);
    nx_rwlock_rdlock(&l); nx_rwlock_rdlock(&l);        /* readers share */
    nx_rwlock_rdunlock(&l); nx_rwlock_rdunlock(&l);
    nx_rwlock_wrlock(&l); nx_rwlock_wrunlock(&l);
    nx_rwlock_destroy(&l);
}

static void test_clock(void) {
    uint64_t a = nx_now_ns();
    nx_sleep_ms(15);
    uint64_t b = nx_now_ns();
    NX_CHECK(b > a && b - a >= 10ull * 1000000 && b - a < 2000ull * 1000000);
    int64_t t = nx_unix_time();
    NX_CHECK(t > 1700000000LL);                         /* after Nov 2023 */
    NX_CHECK(llabs(nx_unix_time_ms() / 1000 - t) <= 1);
}

/* --------------------------------------------------------------- pool */
static void add_one(void *a) { atomic_fetch_add((atomic_int *)a, 1); }

static void test_pool_group(void) {
    nx_pool *p = nx_pool_create(4);
    NX_REQUIRE(p);
    NX_CHECK_EQ_I(nx_pool_size(p), 4);
    atomic_int n; atomic_init(&n, 0);
    nx_group g; nx_group_init(&g, p);
    for (int i = 0; i < 5000; i++) nx_group_submit(&g, add_one, &n);
    nx_group_wait(&g);
    NX_CHECK_EQ_I(atomic_load(&n), 5000);
    NX_CHECK(nx_pool_tasks_run(p) >= 1);
    nx_pool_destroy(p);
}

typedef struct { nx_pool *pool; atomic_int *n; int depth; } nest_t;
static void nested_task(void *a) {
    nest_t *t = (nest_t *)a;
    if (t->depth > 0) {
        nest_t kids[3];
        nx_group g; nx_group_init(&g, t->pool);
        for (int i = 0; i < 3; i++) { kids[i] = *t; kids[i].depth = t->depth - 1; nx_group_submit(&g, nested_task, &kids[i]); }
        nx_group_wait(&g);                              /* must help, not deadlock */
    }
    atomic_fetch_add(t->n, 1);
}
static void test_pool_nested_fanout_single_thread(void) {
    nx_pool *p = nx_pool_create(1);                     /* worst case: every wait must help */
    NX_REQUIRE(p);
    atomic_int n; atomic_init(&n, 0);
    nest_t root = { p, &n, 5 };                         /* 3^5 leaves + inner nodes = 364 tasks */
    nx_group g; nx_group_init(&g, p);
    nx_group_submit(&g, nested_task, &root);
    nx_group_wait(&g);
    NX_CHECK_EQ_I(atomic_load(&n), 364);
    nx_pool_destroy(p);
}

typedef struct { atomic_int *sum; } pf_ctx;
static void pf_body(void *c, size_t b, size_t e) { atomic_fetch_add(((pf_ctx *)c)->sum, (int)(e - b)); }
static void test_parallel_for(void) {
    nx_pool *p = nx_pool_create(3);
    NX_REQUIRE(p);
    atomic_int sum; atomic_init(&sum, 0);
    pf_ctx ctx = { &sum };
    nx_parallel_for(p, 100000, 0, pf_body, &ctx);
    NX_CHECK_EQ_I(atomic_load(&sum), 100000);
    atomic_store(&sum, 0);
    nx_parallel_for(p, 17, 5, pf_body, &ctx);
    NX_CHECK_EQ_I(atomic_load(&sum), 17);
    atomic_store(&sum, 0);
    nx_parallel_for(NULL, 33, 0, pf_body, &ctx);        /* serial fallback */
    NX_CHECK_EQ_I(atomic_load(&sum), 33);
    nx_parallel_for(p, 0, 0, pf_body, &ctx);
    nx_pool_destroy(p);
}

static void test_pool_destroy_drains(void) {
    nx_pool *p = nx_pool_create(2);
    NX_REQUIRE(p);
    atomic_int n; atomic_init(&n, 0);
    for (int i = 0; i < 1000; i++) nx_pool_post(p, add_one, &n);
    nx_pool_destroy(p);                                 /* must run everything queued */
    NX_CHECK_EQ_I(atomic_load(&n), 1000);
}

static void test_pool_oom_never_loses_work(void) {
    nx_pool *p = nx_pool_create(2);
    NX_REQUIRE(p);
    atomic_int n; atomic_init(&n, 0);
    nx_group g; nx_group_init(&g, p);
    for (int i = 0; i < 200; i++) {
        if (i % 3 == 0) nx_mem_fail_after(0);           /* next task-node allocation fails -> runs inline */
        nx_group_submit(&g, add_one, &n);
        nx_mem_fail_after(-1);
    }
    nx_group_wait(&g);
    NX_CHECK_EQ_I(atomic_load(&n), 200);
    nx_pool_destroy(p);
}

int main(void) {
    NX_RUN(test_mem_basic);
    NX_RUN(test_mem_aligned);
    NX_RUN(test_mem_calloc_overflow);
    NX_RUN(test_mem_fail_injection);
    NX_RUN(test_mem_canary_detects_nothing_when_clean);
    NX_RUN(test_strdup);
    NX_RUN(test_arena);
    NX_RUN(test_arena_oom);
    NX_RUN(test_buf_put_and_read);
    NX_RUN(test_buf_patch_pad_printf);
    NX_RUN(test_varint_roundtrip_fuzz);
    NX_RUN(test_cursor_malformed_varints);
    NX_RUN(test_slice_sub);
    NX_RUN(test_buf_oom);
    NX_RUN(test_hash_known_stability);
    NX_RUN(test_hash_distribution);
    NX_RUN(test_rng);
    NX_RUN(test_bits);
    NX_RUN(test_threads_and_mutex);
    NX_RUN(test_cond_and_timeout);
    NX_RUN(test_rwlock);
    NX_RUN(test_clock);
    NX_RUN(test_pool_group);
    NX_RUN(test_pool_nested_fanout_single_thread);
    NX_RUN(test_parallel_for);
    NX_RUN(test_pool_destroy_drains);
    NX_RUN(test_pool_oom_never_loses_work);
    return nx_test_summary();
}
