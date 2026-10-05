/* nx_thread.h - portable threads, locks, a monotonic clock and a work-helping
 * thread pool.
 *
 * Native Win32 primitives on Windows (SRWLOCK / CONDITION_VARIABLE /
 * CreateThread), pthreads elsewhere. No dependency on winpthreads.
 *
 * The pool improves on a plain worker queue: waiting on a task group HELPS run
 * queued tasks instead of blocking, so tasks may themselves fan out sub-tasks
 * and wait for them without deadlocking even on a 1-thread pool.
 */
#ifndef NX_THREAD_H
#define NX_THREAD_H

#include "nx_config.h"
#include <stdatomic.h>

#if NX_WINDOWS
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
typedef struct nx_mutex { SRWLOCK l; } nx_mutex;
typedef struct nx_cond { CONDITION_VARIABLE c; } nx_cond;
typedef struct nx_rwlock { SRWLOCK l; } nx_rwlock;
typedef struct nx_thread { HANDLE h; } nx_thread;
#else
#  include <pthread.h>
typedef struct nx_mutex { pthread_mutex_t m; } nx_mutex;
typedef struct nx_cond { pthread_cond_t c; } nx_cond;
typedef struct nx_rwlock { pthread_rwlock_t l; } nx_rwlock;
typedef struct nx_thread { pthread_t t; } nx_thread;
#endif

typedef void (*nx_thread_fn)(void *arg);

NX_API void nx_mutex_init(nx_mutex *m);
NX_API void nx_mutex_destroy(nx_mutex *m);
NX_API void nx_mutex_lock(nx_mutex *m);
NX_API bool nx_mutex_trylock(nx_mutex *m);
NX_API void nx_mutex_unlock(nx_mutex *m);

NX_API void nx_cond_init(nx_cond *c);
NX_API void nx_cond_destroy(nx_cond *c);
NX_API void nx_cond_wait(nx_cond *c, nx_mutex *m);
/* returns false on timeout */
NX_API bool nx_cond_timedwait(nx_cond *c, nx_mutex *m, uint32_t timeout_ms);
NX_API void nx_cond_signal(nx_cond *c);
NX_API void nx_cond_broadcast(nx_cond *c);

NX_API void nx_rwlock_init(nx_rwlock *l);
NX_API void nx_rwlock_destroy(nx_rwlock *l);
NX_API void nx_rwlock_rdlock(nx_rwlock *l);
NX_API void nx_rwlock_rdunlock(nx_rwlock *l);
NX_API void nx_rwlock_wrlock(nx_rwlock *l);
NX_API void nx_rwlock_wrunlock(nx_rwlock *l);

NX_API bool nx_thread_start(nx_thread *t, nx_thread_fn fn, void *arg);   /* false on failure */
NX_API void nx_thread_join(nx_thread *t);
NX_API void nx_thread_yield(void);
NX_API void nx_sleep_ms(uint32_t ms);
NX_API int  nx_cpu_count(void);

/* Monotonic clock in nanoseconds (QueryPerformanceCounter / clock_gettime). */
NX_API uint64_t nx_now_ns(void);
/* Wall clock, seconds since the Unix epoch (UTC). */
NX_API int64_t  nx_unix_time(void);
NX_API int64_t  nx_unix_time_ms(void);

/* ---- thread pool ------------------------------------------------------- */
typedef struct nx_pool nx_pool;

/* A task group: submit N tasks, then wait for all N. Wait helps execute tasks. */
typedef struct nx_group {
    nx_pool *pool;
    atomic_int pending;
    atomic_int failed;      /* set by nx_group_fail(), readable after wait */
} nx_group;

/* nthreads <= 0 selects nx_cpu_count(). nthreads == 0 workers is not allowed;
 * pass 1 for a single background worker. Returns NULL on failure. */
NX_API nx_pool *nx_pool_create(int nthreads);
NX_API void     nx_pool_destroy(nx_pool *p);   /* drains queued tasks, joins workers */
NX_API int      nx_pool_size(const nx_pool *p);
NX_API uint64_t nx_pool_tasks_run(const nx_pool *p);

NX_API void nx_group_init(nx_group *g, nx_pool *p);
/* Enqueue fn(arg). If the queue allocation fails the task is run INLINE on the
 * calling thread, so submission never fails and never loses work. */
NX_API void nx_group_submit(nx_group *g, nx_thread_fn fn, void *arg);
NX_API void nx_group_wait(nx_group *g);        /* helps run queued tasks until g.pending == 0 */
NX_API void nx_group_fail(nx_group *g);        /* flag from inside a task */

/* Fire-and-forget task (no group). */
NX_API void nx_pool_post(nx_pool *p, nx_thread_fn fn, void *arg);

/* Convenience: run body(ctx, begin, end) over [0, n) split in chunks of
 * `chunk` items (0 = auto), blocking until done. pool may be NULL -> serial. */
typedef void (*nx_range_fn)(void *ctx, size_t begin, size_t end);
NX_API void nx_parallel_for(nx_pool *p, size_t n, size_t chunk, nx_range_fn body, void *ctx);

#endif /* NX_THREAD_H */
