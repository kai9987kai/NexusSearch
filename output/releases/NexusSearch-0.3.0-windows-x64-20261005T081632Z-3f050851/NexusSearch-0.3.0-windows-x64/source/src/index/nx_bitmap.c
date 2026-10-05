#include "nx_bitmap.h"
#include "core/nx_mem.h"

#define ROARING_COOKIE 12346u
#define ARRAY_LIMIT 4096u
#define BITSET_WORDS 1024u

static uint16_t key_at(const nx_bitmap *b, uint32_t i) { return nx_ld16(b->bytes.p + 8 + 4 * (size_t)i); }
static uint32_t count_at(const nx_bitmap *b, uint32_t i) {
    return (uint32_t)nx_ld16(b->bytes.p + 10 + 4 * (size_t)i) + 1;
}
static const uint8_t *data_at(const nx_bitmap *b, uint32_t i) {
    return b->bytes.p + nx_ld32(b->bytes.p + 8 + 4 * (size_t)b->containers + 4 * (size_t)i);
}

nx_status nx_bitmap_open(nx_slice bytes, nx_bitmap *out) {
    if (!out) return NX_ERR_INVALID;
    memset(out, 0, sizeof *out);
    if (!bytes.p || bytes.n < 8) return NX_ERR_CORRUPT;
    nx_cursor cur = nx_cursor_make(bytes);
    uint32_t cookie = nx_rd_u32(&cur), n = nx_rd_u32(&cur);
    if ((cookie & 65535u) == 12347u) return NX_ERR_UNSUPPORTED;
    if (cookie != ROARING_COOKIE || n > 65536u || (size_t)n > nx_cursor_left(&cur) / 8) return NX_ERR_CORRUPT;
    nx_bitmap view = {bytes, n, 0};
    size_t off = 8 + 8 * (size_t)n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t card = count_at(&view, i);
        if (i && key_at(&view, i - 1) >= key_at(&view, i)) return NX_ERR_CORRUPT;
        uint32_t stored = nx_ld32(bytes.p + 8 + 4 * (size_t)n + 4 * (size_t)i);
        size_t len = card <= ARRAY_LIMIT ? 2 * (size_t)card : BITSET_WORDS * 8;
        if (stored != off || off > bytes.n || len > bytes.n - off) return NX_ERR_CORRUPT;
        if (card <= ARRAY_LIMIT) {
            for (uint32_t j = 1; j < card; j++)
                if (nx_ld16(bytes.p + off + 2 * (size_t)(j - 1)) >= nx_ld16(bytes.p + off + 2 * (size_t)j))
                    return NX_ERR_CORRUPT;
        } else {
            uint32_t actual = 0;
            for (size_t j = 0; j < BITSET_WORDS; j++) actual += (uint32_t)nx_popcnt64(nx_ld64(bytes.p + off + 8 * j));
            if (actual != card) return NX_ERR_CORRUPT;
        }
        view.cardinality += card;
        off += len;
    }
    if (off != bytes.n) return NX_ERR_CORRUPT;
    *out = view;
    return NX_OK;
}

static void put_container(nx_buf *desc, nx_buf *offsets, nx_buf *data, uint16_t key, const uint64_t *words) {
    uint32_t count = 0;
    for (size_t i = 0; i < BITSET_WORDS; i++) count += (uint32_t)nx_popcnt64(words[i]);
    if (!count) return;
    nx_buf_put_u16(desc, key);
    nx_buf_put_u16(desc, (uint16_t)(count - 1));
    nx_buf_put_u32(offsets, (uint32_t)data->len);
    if (count > ARRAY_LIMIT) {
        for (size_t i = 0; i < BITSET_WORDS; i++) nx_buf_put_u64(data, words[i]);
    } else {
        for (uint32_t i = 0; i < BITSET_WORDS; i++) {
            uint64_t w = words[i];
            while (w) {
                nx_buf_put_u16(data, (uint16_t)(i * 64 + (uint32_t)nx_ctz64(w)));
                w &= w - 1;
            }
        }
    }
}

static nx_status finish(nx_buf *desc, nx_buf *offsets, nx_buf *data, nx_buf *out) {
    nx_status status = NX_OK;
    if (desc->oom || offsets->oom || data->oom || out->oom) status = NX_ERR_NOMEM;
    size_t n = desc->len / 4, header = 8 + 8 * n, size = header + data->len;
    if (status == NX_OK && !nx_buf_reserve(out, size)) status = NX_ERR_NOMEM;
    if (status == NX_OK) {
        nx_buf_put_u32(out, ROARING_COOKIE);
        nx_buf_put_u32(out, (uint32_t)n);
        nx_buf_put(out, desc->data, desc->len);
        for (size_t i = 0; i < n; i++) nx_buf_put_u32(out, (uint32_t)header + nx_ld32(offsets->data + 4 * i));
        nx_buf_put(out, data->data, data->len);
    }
    nx_buf_free(desc); nx_buf_free(offsets); nx_buf_free(data);
    return status;
}

static int compare_ids(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

nx_status nx_bitmap_build(const uint32_t *ids, size_t n, nx_buf *out) {
    if (!out || (n && !ids)) return NX_ERR_INVALID;
    if (n > (uint64_t)UINT32_MAX + 1 || n > SIZE_MAX / sizeof(uint32_t)) return NX_ERR_LIMIT;
    uint32_t *copy = n ? NX_NEW_ARRAY(uint32_t, n) : NULL;
    if (n && !copy) return NX_ERR_NOMEM;
    if (n) { memcpy(copy, ids, n * sizeof(uint32_t)); qsort(copy, n, sizeof(uint32_t), compare_ids); }
    nx_buf desc, offsets, data;
    nx_buf_init(&desc); nx_buf_init(&offsets); nx_buf_init(&data);
    size_t i = 0;
    while (i < n && !desc.oom && !offsets.oom && !data.oom) {
        uint16_t key = (uint16_t)(copy[i] >> 16);
        uint64_t words[BITSET_WORDS] = {0};
        do {
            uint32_t low = copy[i] & 65535u;
            words[low / 64] |= UINT64_C(1) << (low % 64);
            i++;
        } while (i < n && copy[i] >> 16 == key);
        put_container(&desc, &offsets, &data, key, words);
    }
    nx_free(copy);
    return finish(&desc, &offsets, &data, out);
}

bool nx_bitmap_contains(const nx_bitmap *b, uint32_t id) {
    uint32_t lo = 0, hi = b->containers, key = id >> 16;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (key_at(b, mid) < key) lo = mid + 1; else hi = mid;
    }
    if (lo == b->containers || key_at(b, lo) != key) return false;
    const uint8_t *p = data_at(b, lo);
    uint32_t count = count_at(b, lo), low = id & 65535u;
    if (count > ARRAY_LIMIT) return (nx_ld64(p + 8 * (size_t)(low / 64)) & (UINT64_C(1) << (low % 64))) != 0;
    lo = 0; hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (nx_ld16(p + 2 * (size_t)mid) < low) lo = mid + 1; else hi = mid;
    }
    return lo < count && nx_ld16(p + 2 * (size_t)lo) == low;
}

void nx_bitmap_iter_init(const nx_bitmap *b, nx_bitmap_iter *it) { it->bitmap = b; it->container = 0; it->low = 0; }
bool nx_bitmap_next(nx_bitmap_iter *it, uint32_t *id) {
    const nx_bitmap *b = it->bitmap;
    while (it->container < b->containers) {
        uint32_t c = it->container, count = count_at(b, c);
        const uint8_t *p = data_at(b, c);
        if (count <= ARRAY_LIMIT) {
            if (it->low < count) {
                *id = ((uint32_t)key_at(b, c) << 16) | nx_ld16(p + 2 * (size_t)it->low++);
                return true;
            }
        } else {
            while (it->low < 65536) {
                uint32_t word = it->low / 64;
                uint64_t bits = nx_ld64(p + 8 * (size_t)word) & (UINT64_MAX << (it->low % 64));
                if (bits) {
                    uint32_t low = word * 64 + (uint32_t)nx_ctz64(bits);
                    it->low = low + 1;
                    *id = ((uint32_t)key_at(b, c) << 16) | low;
                    return true;
                }
                it->low = (word + 1) * 64;
            }
        }
        it->container++; it->low = 0;
    }
    return false;
}

static void unpack(const nx_bitmap *b, uint32_t i, uint64_t *words) {
    uint32_t card = count_at(b, i);
    const uint8_t *p = data_at(b, i);
    if (card > ARRAY_LIMIT) {
        for (size_t j = 0; j < BITSET_WORDS; j++) words[j] = nx_ld64(p + 8 * j);
    } else {
        memset(words, 0, BITSET_WORDS * sizeof(uint64_t));
        for (uint32_t j = 0; j < card; j++) {
            uint16_t v = nx_ld16(p + 2 * (size_t)j);
            words[v / 64] |= UINT64_C(1) << (v % 64);
        }
    }
}

nx_status nx_bitmap_combine(const nx_bitmap *a, const nx_bitmap *b, nx_bitmap_op op, nx_buf *out) {
    if (!a || !b || !out || op < NX_BITMAP_AND || op > NX_BITMAP_ANDNOT) return NX_ERR_INVALID;
    nx_buf desc, offsets, data;
    nx_buf_init(&desc); nx_buf_init(&offsets); nx_buf_init(&data);
    uint32_t i = 0, j = 0;
    while ((i < a->containers || j < b->containers) && !desc.oom && !offsets.oom && !data.oom) {
        uint32_t ka = i < a->containers ? key_at(a, i) : 65536u;
        uint32_t kb = j < b->containers ? key_at(b, j) : 65536u;
        uint32_t key = ka < kb ? ka : kb;
        uint64_t x[BITSET_WORDS] = {0}, y[BITSET_WORDS] = {0};
        if (ka == key) unpack(a, i++, x);
        if (kb == key) unpack(b, j++, y);
        for (size_t w = 0; w < BITSET_WORDS; w++) {
            switch (op) {
                case NX_BITMAP_AND: x[w] &= y[w]; break;
                case NX_BITMAP_OR: x[w] |= y[w]; break;
                case NX_BITMAP_XOR: x[w] ^= y[w]; break;
                case NX_BITMAP_ANDNOT: x[w] &= ~y[w]; break;
            }
        }
        put_container(&desc, &offsets, &data, (uint16_t)key, x);
    }
    return finish(&desc, &offsets, &data, out);
}
