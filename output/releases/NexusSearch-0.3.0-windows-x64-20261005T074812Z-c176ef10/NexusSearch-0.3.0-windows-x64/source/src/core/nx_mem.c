#include "nx_mem.h"
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>

/* ---- block header ------------------------------------------------------- */
#if defined(NX_MEM_DEBUG)
#  define NX_BLOCK_MAGIC 0x4B4C42454D4E58ULL /* "XNMEBLK" */
#  define NX_TAIL_CANARY 0xC0FFEEA5DEADBEEFULL
typedef struct nx_hdr {
    uint64_t magic;
    void *raw;
    size_t size;
    const char *file;
    int32_t line;
    int32_t pad;
    struct nx_hdr *prev, *next;
    uint64_t seq;
} nx_hdr;
NX_STATIC_ASSERT(sizeof(nx_hdr) == 64, "debug header must be 64 bytes");
#  define NX_TAIL_SIZE 8
#else
typedef struct nx_hdr {
    void *raw;
    size_t size;
} nx_hdr;
NX_STATIC_ASSERT(sizeof(nx_hdr) == 16, "header must be 16 bytes");
#  define NX_TAIL_SIZE 0
#endif

#define NX_HDR_SIZE ((size_t)sizeof(nx_hdr))

static atomic_uint_fast64_t g_live_allocs;
static atomic_uint_fast64_t g_live_bytes;
static atomic_uint_fast64_t g_total_allocs;
static atomic_uint_fast64_t g_peak_bytes;
static atomic_int_fast64_t  g_fail_countdown = -1;
static atomic_uint_fast64_t g_alloc_attempts;

#if defined(NX_MEM_DEBUG)
static atomic_flag g_reg_lock = ATOMIC_FLAG_INIT;
static nx_hdr *g_reg_head;
static uint64_t g_seq;
static void reg_lock(void) { while (atomic_flag_test_and_set_explicit(&g_reg_lock, memory_order_acquire)) { } }
static void reg_unlock(void) { atomic_flag_clear_explicit(&g_reg_lock, memory_order_release); }
#endif

NX_NORETURN void nx_panic(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("\nnexus PANIC: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
    abort();
}

static bool should_fail(void) {
    atomic_fetch_add_explicit(&g_alloc_attempts, 1, memory_order_relaxed);
    int_fast64_t c = atomic_load_explicit(&g_fail_countdown, memory_order_relaxed);
    while (c >= 0) {
        if (atomic_compare_exchange_weak_explicit(&g_fail_countdown, &c, c == 0 ? -1 : c - 1,
                                                  memory_order_relaxed, memory_order_relaxed))
            return c == 0;
    }
    return false;
}

static void *alloc_core(size_t n, size_t align, bool zero, const char *file, int line) {
    if (should_fail()) return NULL;
    if (align < 16) align = 16;
    if (!nx_is_pow2(align)) return NULL;
    size_t total;
    if (nx_add_overflow(n, NX_HDR_SIZE + NX_TAIL_SIZE, &total)) return NULL;
    if (nx_add_overflow(total, align, &total)) return NULL;
    uint8_t *raw = (uint8_t *)malloc(total);
    if (!raw) return NULL;
    uint8_t *user = (uint8_t *)nx_align_up((size_t)(raw + NX_HDR_SIZE), align);
    nx_hdr *h = (nx_hdr *)(user - NX_HDR_SIZE);
    h->raw = raw;
    h->size = n;
#if defined(NX_MEM_DEBUG)
    h->magic = NX_BLOCK_MAGIC;
    h->file = file;
    h->line = line;
    h->pad = 0;
    uint64_t canary = NX_TAIL_CANARY;
    memcpy(user + n, &canary, sizeof canary);
    if (!zero) memset(user, 0xCD, n); /* poison uninitialised memory */
    reg_lock();
    h->prev = NULL;
    h->next = g_reg_head;
    h->seq = ++g_seq;
    if (g_reg_head) g_reg_head->prev = h;
    g_reg_head = h;
    reg_unlock();
#else
    (void)file; (void)line;
#endif
    if (zero) memset(user, 0, n);
    atomic_fetch_add_explicit(&g_live_allocs, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_total_allocs, 1, memory_order_relaxed);
    uint_fast64_t lb = atomic_fetch_add_explicit(&g_live_bytes, n, memory_order_relaxed) + n;
    uint_fast64_t pk = atomic_load_explicit(&g_peak_bytes, memory_order_relaxed);
    while (lb > pk && !atomic_compare_exchange_weak_explicit(&g_peak_bytes, &pk, lb, memory_order_relaxed, memory_order_relaxed)) { }
    return user;
}

void *nx_malloc_at(size_t n, const char *file, int line) { return alloc_core(n, 16, false, file, line); }

void *nx_calloc_at(size_t count, size_t size, const char *file, int line) {
    size_t n;
    if (nx_mul_overflow(count, size, &n)) return NULL;
    return alloc_core(n, 16, true, file, line);
}

void *nx_aligned_alloc_at(size_t align, size_t n, const char *file, int line) {
    return alloc_core(n, align, false, file, line);
}

#if defined(NX_MEM_DEBUG)
static void verify_block(nx_hdr *h, const char *what) {
    if (h->magic != NX_BLOCK_MAGIC)
        nx_panic("heap corruption: bad block header in %s (double free or underrun?) at %p", what, (void *)(h + 1));
    uint64_t canary;
    memcpy(&canary, (uint8_t *)(h + 1) + h->size, sizeof canary);
    if (canary != NX_TAIL_CANARY)
        nx_panic("heap corruption: buffer overrun detected in %s; block of %zu bytes allocated at %s:%d",
                 what, h->size, h->file ? h->file : "?", h->line);
}
#endif

void nx_free(void *p) {
    if (!p) return;
    nx_hdr *h = (nx_hdr *)((uint8_t *)p - NX_HDR_SIZE);
    size_t n = h->size;
#if defined(NX_MEM_DEBUG)
    verify_block(h, "nx_free");
    reg_lock();
    if (h->prev) h->prev->next = h->next; else g_reg_head = h->next;
    if (h->next) h->next->prev = h->prev;
    reg_unlock();
    void *raw = h->raw;
    memset(p, 0xDD, n);          /* poison freed memory */
    h->magic = 0xDEADDEADDEADDEADULL;
    free(raw);
#else
    void *raw = h->raw;
    free(raw);
#endif
    atomic_fetch_sub_explicit(&g_live_allocs, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&g_live_bytes, n, memory_order_relaxed);
}

void *nx_realloc_at(void *p, size_t n, const char *file, int line) {
    if (!p) return nx_malloc_at(n, file, line);
    nx_hdr *h = (nx_hdr *)((uint8_t *)p - NX_HDR_SIZE);
#if defined(NX_MEM_DEBUG)
    verify_block(h, "nx_realloc");
#endif
    size_t old = h->size;
    void *q = alloc_core(n, 16, false, file, line);
    if (!q) return NULL;                 /* original block untouched */
    memcpy(q, p, old < n ? old : n);
    nx_free(p);
    return q;
}

char *nx_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *r = (char *)nx_malloc_at(len + 1, __FILE__, __LINE__);
    if (!r) return NULL;
    memcpy(r, s, len);
    r[len] = 0;
    return r;
}
char *nx_strdup(const char *s) { return s ? nx_strndup(s, strlen(s)) : NULL; }

nx_mem_stats nx_mem_get_stats(void) {
    nx_mem_stats s;
    s.live_allocs = atomic_load_explicit(&g_live_allocs, memory_order_relaxed);
    s.live_bytes = atomic_load_explicit(&g_live_bytes, memory_order_relaxed);
    s.total_allocs = atomic_load_explicit(&g_total_allocs, memory_order_relaxed);
    s.peak_bytes = atomic_load_explicit(&g_peak_bytes, memory_order_relaxed);
    return s;
}

void nx_mem_fail_after(int64_t n) { atomic_store_explicit(&g_fail_countdown, n, memory_order_relaxed); }
uint64_t nx_mem_alloc_counter(void) { return atomic_load_explicit(&g_alloc_attempts, memory_order_relaxed); }

size_t nx_mem_check_all(void) {
#if defined(NX_MEM_DEBUG)
    size_t bad = 0;
    reg_lock();
    for (nx_hdr *h = g_reg_head; h; h = h->next) {
        uint64_t canary;
        memcpy(&canary, (uint8_t *)(h + 1) + h->size, sizeof canary);
        if (h->magic != NX_BLOCK_MAGIC || canary != NX_TAIL_CANARY) bad++;
    }
    reg_unlock();
    return bad;
#else
    return 0;
#endif
}

size_t nx_mem_dump_leaks(void *file) {
#if defined(NX_MEM_DEBUG)
    FILE *f = file ? (FILE *)file : stderr;
    size_t n = 0;
    reg_lock();
    for (nx_hdr *h = g_reg_head; h; h = h->next) {
        if (n < 20) fprintf(f, "  leak: %zu bytes allocated at %s:%d\n", h->size, h->file ? h->file : "?", h->line);
        n++;
    }
    reg_unlock();
    if (n > 20) fprintf(f, "  ... and %zu more\n", n - 20);
    return n;
#else
    (void)file;
    return 0;
#endif
}
