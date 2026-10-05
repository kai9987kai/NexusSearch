#include "nx_thread.h"
#include "nx_mem.h"

#if NX_WINDOWS
#  include <process.h>
#else
#  include <time.h>
#  include <errno.h>
#  include <sched.h>
#  include <unistd.h>
#endif

/* ===================================================================== locks */
#if NX_WINDOWS
void nx_mutex_init(nx_mutex *m) { InitializeSRWLock(&m->l); }
void nx_mutex_destroy(nx_mutex *m) { (void)m; }
void nx_mutex_lock(nx_mutex *m) { AcquireSRWLockExclusive(&m->l); }
bool nx_mutex_trylock(nx_mutex *m) { return TryAcquireSRWLockExclusive(&m->l) != 0; }
void nx_mutex_unlock(nx_mutex *m) { ReleaseSRWLockExclusive(&m->l); }

void nx_cond_init(nx_cond *c) { InitializeConditionVariable(&c->c); }
void nx_cond_destroy(nx_cond *c) { (void)c; }
void nx_cond_wait(nx_cond *c, nx_mutex *m) { SleepConditionVariableSRW(&c->c, &m->l, INFINITE, 0); }
bool nx_cond_timedwait(nx_cond *c, nx_mutex *m, uint32_t ms) {
    return SleepConditionVariableSRW(&c->c, &m->l, ms, 0) != 0;
}
void nx_cond_signal(nx_cond *c) { WakeConditionVariable(&c->c); }
void nx_cond_broadcast(nx_cond *c) { WakeAllConditionVariable(&c->c); }

void nx_rwlock_init(nx_rwlock *l) { InitializeSRWLock(&l->l); }
void nx_rwlock_destroy(nx_rwlock *l) { (void)l; }
void nx_rwlock_rdlock(nx_rwlock *l) { AcquireSRWLockShared(&l->l); }
void nx_rwlock_rdunlock(nx_rwlock *l) { ReleaseSRWLockShared(&l->l); }
void nx_rwlock_wrlock(nx_rwlock *l) { AcquireSRWLockExclusive(&l->l); }
void nx_rwlock_wrunlock(nx_rwlock *l) { ReleaseSRWLockExclusive(&l->l); }

typedef struct { nx_thread_fn fn; void *arg; } thr_start;
static unsigned __stdcall thr_tramp(void *p) {
    thr_start s = *(thr_start *)p;
    nx_free(p);
    s.fn(s.arg);
    return 0;
}
bool nx_thread_start(nx_thread *t, nx_thread_fn fn, void *arg) {
    thr_start *s = (thr_start *)nx_malloc(sizeof *s);
    if (!s) return false;
    s->fn = fn; s->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, thr_tramp, s, 0, NULL);
    if (!h) { nx_free(s); return false; }
    t->h = (HANDLE)h;
    return true;
}
void nx_thread_join(nx_thread *t) { WaitForSingleObject(t->h, INFINITE); CloseHandle(t->h); t->h = NULL; }
void nx_thread_yield(void) { SwitchToThread(); }
void nx_sleep_ms(uint32_t ms) { Sleep(ms); }
int nx_cpu_count(void) { SYSTEM_INFO si; GetSystemInfo(&si); return si.dwNumberOfProcessors ? (int)si.dwNumberOfProcessors : 1; }

uint64_t nx_now_ns(void) {
    static LARGE_INTEGER freq;      /* benign race: idempotent init */
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    uint64_t f = (uint64_t)freq.QuadPart, v = (uint64_t)c.QuadPart;
    return (v / f) * 1000000000ULL + ((v % f) * 1000000000ULL) / f;
}
static uint64_t filetime_unix_100ns(void) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t - 116444736000000000ULL;
}
int64_t nx_unix_time(void) { return (int64_t)(filetime_unix_100ns() / 10000000ULL); }
int64_t nx_unix_time_ms(void) { return (int64_t)(filetime_unix_100ns() / 10000ULL); }
#else
void nx_mutex_init(nx_mutex *m) { pthread_mutex_init(&m->m, NULL); }
void nx_mutex_destroy(nx_mutex *m) { pthread_mutex_destroy(&m->m); }
void nx_mutex_lock(nx_mutex *m) { pthread_mutex_lock(&m->m); }
bool nx_mutex_trylock(nx_mutex *m) { return pthread_mutex_trylock(&m->m) == 0; }
void nx_mutex_unlock(nx_mutex *m) { pthread_mutex_unlock(&m->m); }

void nx_cond_init(nx_cond *c) { pthread_cond_init(&c->c, NULL); }
void nx_cond_destroy(nx_cond *c) { pthread_cond_destroy(&c->c); }
void nx_cond_wait(nx_cond *c, nx_mutex *m) { pthread_cond_wait(&c->c, &m->m); }
bool nx_cond_timedwait(nx_cond *c, nx_mutex *m, uint32_t ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&c->c, &m->m, &ts) != ETIMEDOUT;
}
void nx_cond_signal(nx_cond *c) { pthread_cond_signal(&c->c); }
void nx_cond_broadcast(nx_cond *c) { pthread_cond_broadcast(&c->c); }

void nx_rwlock_init(nx_rwlock *l) { pthread_rwlock_init(&l->l, NULL); }
void nx_rwlock_destroy(nx_rwlock *l) { pthread_rwlock_destroy(&l->l); }
void nx_rwlock_rdlock(nx_rwlock *l) { pthread_rwlock_rdlock(&l->l); }
void nx_rwlock_rdunlock(nx_rwlock *l) { pthread_rwlock_unlock(&l->l); }
void nx_rwlock_wrlock(nx_rwlock *l) { pthread_rwlock_wrlock(&l->l); }
void nx_rwlock_wrunlock(nx_rwlock *l) { pthread_rwlock_unlock(&l->l); }

typedef struct { nx_thread_fn fn; void *arg; } thr_start;
static void *thr_tramp(void *p) {
    thr_start s = *(thr_start *)p;
    nx_free(p);
    s.fn(s.arg);
    return NULL;
}
bool nx_thread_start(nx_thread *t, nx_thread_fn fn, void *arg) {
    thr_start *s = (thr_start *)nx_malloc(sizeof *s);
    if (!s) return false;
    s->fn = fn; s->arg = arg;
    if (pthread_create(&t->t, NULL, thr_tramp, s) != 0) { nx_free(s); return false; }
    return true;
}
void nx_thread_join(nx_thread *t) { pthread_join(t->t, NULL); }
void nx_thread_yield(void) { sched_yield(); }
void nx_sleep_ms(uint32_t ms) {
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
int nx_cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }

uint64_t nx_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
int64_t nx_unix_time(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (int64_t)ts.tv_sec; }
int64_t nx_unix_time_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

/* ================================================================= thread pool */
typedef struct pool_task {
    nx_thread_fn fn;
    void *arg;
    nx_group *group;
    struct pool_task *next;
} pool_task;

struct nx_pool {
    nx_mutex mu;
    nx_cond  cv;                 /* signalled on new work; broadcast on group completion */
    pool_task *head, *tail;
    bool stop;
    int nthreads;
    nx_thread *threads;
    atomic_uint_fast64_t run_count;
};

static void group_finish(nx_group *g) {
    if (!g) return;
    if (atomic_fetch_sub_explicit(&g->pending, 1, memory_order_acq_rel) == 1) {
        nx_pool *p = g->pool;
        nx_mutex_lock(&p->mu);              /* lock-then-broadcast: no missed wakeup */
        nx_cond_broadcast(&p->cv);
        nx_mutex_unlock(&p->mu);
    }
}

static pool_task *queue_pop_locked(nx_pool *p) {
    pool_task *t = p->head;
    if (t) { p->head = t->next; if (!p->head) p->tail = NULL; }
    return t;
}

static void run_task(nx_pool *p, pool_task *t) {
    t->fn(t->arg);
    atomic_fetch_add_explicit(&p->run_count, 1, memory_order_relaxed);
    nx_group *g = t->group;
    nx_free(t);
    group_finish(g);
}

static void worker_main(void *arg) {
    nx_pool *p = (nx_pool *)arg;
    nx_mutex_lock(&p->mu);
    for (;;) {
        while (!p->head && !p->stop) nx_cond_wait(&p->cv, &p->mu);
        if (!p->head && p->stop) break;
        pool_task *t = queue_pop_locked(p);
        nx_mutex_unlock(&p->mu);
        run_task(p, t);
        nx_mutex_lock(&p->mu);
    }
    nx_mutex_unlock(&p->mu);
}

nx_pool *nx_pool_create(int nthreads) {
    if (nthreads <= 0) nthreads = nx_cpu_count();
    nx_pool *p = NX_NEW(nx_pool);
    if (!p) return NULL;
    nx_mutex_init(&p->mu);
    nx_cond_init(&p->cv);
    p->threads = NX_NEW_ARRAY(nx_thread, (size_t)nthreads);
    if (!p->threads) { nx_cond_destroy(&p->cv); nx_mutex_destroy(&p->mu); nx_free(p); return NULL; }
    for (int i = 0; i < nthreads; i++) {
        if (!nx_thread_start(&p->threads[i], worker_main, p)) break;
        p->nthreads++;
    }
    if (p->nthreads == 0) {
        nx_free(p->threads); nx_cond_destroy(&p->cv); nx_mutex_destroy(&p->mu); nx_free(p);
        return NULL;
    }
    return p;
}

void nx_pool_destroy(nx_pool *p) {
    if (!p) return;
    nx_mutex_lock(&p->mu);
    p->stop = true;
    nx_cond_broadcast(&p->cv);
    nx_mutex_unlock(&p->mu);
    for (int i = 0; i < p->nthreads; i++) nx_thread_join(&p->threads[i]);
    /* workers drain the queue before exiting; anything left (should be nothing) is freed */
    pool_task *t;
    while ((t = queue_pop_locked(p)) != NULL) nx_free(t);
    nx_free(p->threads);
    nx_cond_destroy(&p->cv);
    nx_mutex_destroy(&p->mu);
    nx_free(p);
}

int nx_pool_size(const nx_pool *p) { return p ? p->nthreads : 0; }
uint64_t nx_pool_tasks_run(const nx_pool *p) { return p ? atomic_load_explicit(&p->run_count, memory_order_relaxed) : 0; }

static bool enqueue(nx_pool *p, nx_thread_fn fn, void *arg, nx_group *g) {
    pool_task *t = (pool_task *)nx_malloc(sizeof *t);
    if (!t) return false;
    t->fn = fn; t->arg = arg; t->group = g; t->next = NULL;
    nx_mutex_lock(&p->mu);
    if (p->tail) p->tail->next = t; else p->head = t;
    p->tail = t;
    nx_cond_signal(&p->cv);
    nx_mutex_unlock(&p->mu);
    return true;
}

void nx_pool_post(nx_pool *p, nx_thread_fn fn, void *arg) {
    if (!p || !enqueue(p, fn, arg, NULL)) fn(arg);
}

void nx_group_init(nx_group *g, nx_pool *p) {
    g->pool = p;
    atomic_init(&g->pending, 0);
    atomic_init(&g->failed, 0);
}

void nx_group_submit(nx_group *g, nx_thread_fn fn, void *arg) {
    if (!g->pool) { fn(arg); return; }
    atomic_fetch_add_explicit(&g->pending, 1, memory_order_acq_rel);
    if (!enqueue(g->pool, fn, arg, g)) {
        fn(arg);                    /* allocation failed: run inline, never drop work */
        atomic_fetch_sub_explicit(&g->pending, 1, memory_order_acq_rel);
    }
}

void nx_group_fail(nx_group *g) { atomic_store_explicit(&g->failed, 1, memory_order_relaxed); }

void nx_group_wait(nx_group *g) {
    nx_pool *p = g->pool;
    if (!p) return;
    nx_mutex_lock(&p->mu);
    while (atomic_load_explicit(&g->pending, memory_order_acquire) > 0) {
        pool_task *t = queue_pop_locked(p);
        if (t) {                    /* help: run any queued task instead of sleeping */
            nx_mutex_unlock(&p->mu);
            run_task(p, t);
            nx_mutex_lock(&p->mu);
            continue;
        }
        nx_cond_wait(&p->cv, &p->mu);
    }
    /* we may have consumed a wake-up meant for a worker: pass it on */
    if (p->head) nx_cond_signal(&p->cv);
    nx_mutex_unlock(&p->mu);
}

typedef struct { nx_range_fn body; void *ctx; size_t b, e; } pf_task;
static void pf_run(void *a) { pf_task *t = (pf_task *)a; t->body(t->ctx, t->b, t->e); }

void nx_parallel_for(nx_pool *p, size_t n, size_t chunk, nx_range_fn body, void *ctx) {
    if (!n) return;
    size_t threads = p ? (size_t)nx_pool_size(p) : 1;
    if (chunk == 0) { chunk = (n + threads * 4 - 1) / (threads * 4); if (chunk == 0) chunk = 1; }
    if (!p || n <= chunk) { body(ctx, 0, n); return; }
    size_t nchunks = (n + chunk - 1) / chunk;
    pf_task *tasks = NX_NEW_ARRAY(pf_task, nchunks);
    if (!tasks) { body(ctx, 0, n); return; }
    nx_group g;
    nx_group_init(&g, p);
    for (size_t i = 0; i < nchunks; i++) {
        tasks[i].body = body; tasks[i].ctx = ctx;
        tasks[i].b = i * chunk;
        tasks[i].e = (i + 1) * chunk < n ? (i + 1) * chunk : n;
        nx_group_submit(&g, pf_run, &tasks[i]);
    }
    nx_group_wait(&g);
    nx_free(tasks);
}
