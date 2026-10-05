#include "nx_arena.h"
#include "nx_mem.h"

struct nx_arena_chunk {
    nx_arena_chunk *next;   /* older chunk */
    size_t cap;             /* payload capacity */
    size_t used;
    /* payload follows, 16-byte aligned (header is 24 bytes -> pad to 32) */
    uint8_t pad_[8];
};
#define CHUNK_HDR ((size_t)sizeof(nx_arena_chunk))
NX_STATIC_ASSERT(sizeof(nx_arena_chunk) % 16 == 0, "chunk header must keep payload 16-byte aligned");

static uint8_t *chunk_data(nx_arena_chunk *c) { return (uint8_t *)c + CHUNK_HDR; }

static nx_arena_chunk *chunk_new(size_t cap) {
    size_t bytes;
    if (nx_add_overflow(CHUNK_HDR, cap, &bytes)) return NULL;
    nx_arena_chunk *c = (nx_arena_chunk *)nx_malloc(bytes);
    if (!c) return NULL;
    c->next = NULL; c->cap = cap; c->used = 0;
    return c;
}

void nx_arena_init(nx_arena *a, size_t chunk_size) {
    a->head = NULL;
    a->chunk_size = chunk_size ? chunk_size : 32 * 1024;
    a->total = 0;
    a->oom = false;
}

void nx_arena_free(nx_arena *a) {
    nx_arena_chunk *c = a->head;
    while (c) { nx_arena_chunk *n = c->next; nx_free(c); c = n; }
    a->head = NULL; a->total = 0; a->oom = false;
}

void nx_arena_reset(nx_arena *a) {
    nx_arena_chunk *c = a->head;
    a->total = 0;
    a->oom = false;
    if (!c) return;
    while (c->next) { nx_arena_chunk *n = c->next; nx_free(c); c = n; }
    c->used = 0;
    a->head = c;
    a->total = 0;
    a->oom = false;
}

void *nx_arena_alloc(nx_arena *a, size_t n, size_t align) {
    size_t total;
    if (nx_add_overflow(a->total, n, &total)) { a->oom = true; return NULL; }
    if (align < 1) align = 1;
    if (!nx_is_pow2(align) || align > 4096) { a->oom = true; return NULL; }
    nx_arena_chunk *c = a->head;
    if (c) {
        size_t base = (size_t)(uintptr_t)chunk_data(c);
        size_t off = nx_align_up(base + c->used, align) - base;
        if (off <= c->cap && n <= c->cap - off) {
            c->used = off + n;
            a->total = total;
            return chunk_data(c) + off;
        }
    }
    /* need a new chunk: oversize requests get a dedicated chunk but stay behind
     * the current head so the head's free space keeps being used */
    size_t need;
    if (nx_add_overflow(n, align, &need)) { a->oom = true; return NULL; }
    size_t cap = need > a->chunk_size ? need : a->chunk_size;
    nx_arena_chunk *nc = chunk_new(cap);
    if (!nc) { a->oom = true; return NULL; }
    size_t base = (size_t)(uintptr_t)chunk_data(nc);
    size_t off = nx_align_up(base, align) - base;
    nc->used = off + n;
    a->total = total;
    if (c && need > a->chunk_size) {
        /* dedicated chunk: insert behind head */
        nc->next = c->next;
        c->next = nc;
    } else {
        nc->next = c;
        a->head = nc;
    }
    return chunk_data(nc) + off;
}

void *nx_arena_zalloc(nx_arena *a, size_t n, size_t align) {
    void *p = nx_arena_alloc(a, n, align);
    if (p) memset(p, 0, n);
    return p;
}

char *nx_arena_strndup(nx_arena *a, const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *r = (char *)nx_arena_alloc(a, len + 1, 1);
    if (!r) return NULL;
    memcpy(r, s, len);
    r[len] = 0;
    return r;
}
char *nx_arena_strdup(nx_arena *a, const char *s) { return s ? nx_arena_strndup(a, s, strlen(s)) : NULL; }

nx_arena_mark nx_arena_save(const nx_arena *a) {
    nx_arena_mark m;
    m.chunk = a->head;
    m.next = a->head ? a->head->next : NULL;
    m.used = a->head ? a->head->used : 0;
    m.total = a->total;
    return m;
}

void nx_arena_restore(nx_arena *a, nx_arena_mark m) {
    /* New regular chunks precede the mark; dedicated oversize chunks can
     * also have been inserted immediately behind it. Restore both chains. */
    nx_arena_chunk *c = a->head;
    while (c && c != m.chunk) { nx_arena_chunk *n = c->next; nx_free(c); c = n; }
    a->head = c;
    if (c) {
        while (c->next != m.next) {
            nx_arena_chunk *old = c->next;
            c->next = old->next;
            nx_free(old);
        }
        c->used = m.used;
    }
    a->total = m.total;
}
