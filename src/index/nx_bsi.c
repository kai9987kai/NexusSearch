#include "nx_bsi.h"
#include "core/nx_crc32c.h"

#define BSI_MAGIC UINT32_C(0x4953424e)
#define BSI_VERSION 1u
#define BSI_HEADER 32u
#define BSI_PLANES 65u
#define BSI_SIGN (UINT64_C(1) << 63)
#define ROARING_COOKIE 12346u
#define CONTAINER_WORDS 1024u
#define CONTAINER_ROWS 65536u
#define ARRAY_LIMIT 4096u
#define MAX_CONTAINERS ((NX_BSI_MAX_ROWS + CONTAINER_ROWS - 1u) / CONTAINER_ROWS)

static uint32_t bsi_crc(nx_slice bytes) {
    uint32_t crc = nx_crc32c(0, bytes.p, BSI_HEADER - 4u);
    return nx_crc32c(crc, bytes.p + BSI_HEADER, bytes.n - BSI_HEADER);
}

nx_status nx_bsi_build(const int64_t *values, const uint8_t *present, size_t rows, nx_buf *out) {
    if (!out || (rows && !values)) return NX_ERR_INVALID;
    if (rows > NX_BSI_MAX_ROWS) return NX_ERR_LIMIT;
    if (out->oom) return NX_ERR_NOMEM;
    for (size_t row = 0; present && row < rows; row++) if (present[row] > 1) return NX_ERR_INVALID;
    size_t words = (rows + 63u) / 64u, plane_size, payload_size, size;
    if (nx_mul_overflow(words, 8u, &plane_size) || nx_mul_overflow(plane_size, BSI_PLANES, &payload_size) ||
        nx_add_overflow(payload_size, BSI_HEADER, &size)) return NX_ERR_LIMIT;
    nx_buf tmp; nx_buf_init(&tmp);
    nx_buf_put_zeros(&tmp, size);
    if (tmp.oom) { nx_buf_free(&tmp); return NX_ERR_NOMEM; }
    nx_st32(tmp.data, BSI_MAGIC); nx_st32(tmp.data + 4, BSI_VERSION);
    nx_st32(tmp.data + 8, (uint32_t)rows); nx_st32(tmp.data + 12, (uint32_t)words);
    nx_st32(tmp.data + 16, BSI_PLANES); nx_st32(tmp.data + 20, BSI_HEADER);
    nx_st32(tmp.data + 24, (uint32_t)payload_size);
    for (size_t row = 0; row < rows; row++) {
        if (present && !present[row]) continue;
        uint64_t mask = UINT64_C(1) << (row % 64u);
        size_t off = BSI_HEADER + row / 64u * 8u;
        nx_st64(tmp.data + off, nx_ld64(tmp.data + off) | mask);
        uint64_t key = (uint64_t)values[row] ^ BSI_SIGN;
        while (key) {
            size_t plane = (size_t)nx_ctz64(key) + 1u;
            uint8_t *word = tmp.data + off + plane * plane_size;
            nx_st64(word, nx_ld64(word) | mask);
            key &= key - 1u;
        }
    }
    nx_st32(tmp.data + 28, bsi_crc(nx_buf_slice(&tmp)));
    nx_status st = nx_buf_reserve(out, size) ? NX_OK : NX_ERR_NOMEM;
    if (st == NX_OK) nx_buf_put(out, tmp.data, size);
    nx_buf_free(&tmp);
    return st;
}

nx_status nx_bsi_open(nx_slice bytes, nx_bsi *out) {
    if (!out) return NX_ERR_INVALID;
    memset(out, 0, sizeof *out);
    if (!bytes.p || bytes.n < BSI_HEADER) return NX_ERR_CORRUPT;
    nx_cursor cur = nx_cursor_make(bytes);
    uint32_t magic = nx_rd_u32(&cur), version = nx_rd_u32(&cur);
    uint32_t rows = nx_rd_u32(&cur), words = nx_rd_u32(&cur), planes = nx_rd_u32(&cur);
    uint32_t header = nx_rd_u32(&cur), payload = nx_rd_u32(&cur), crc = nx_rd_u32(&cur);
    if (magic != BSI_MAGIC) return NX_ERR_CORRUPT;
    if (version != BSI_VERSION) return NX_ERR_VERSION;
    size_t plane_size, expected;
    if (rows > NX_BSI_MAX_ROWS || words != (rows + 63u) / 64u || planes != BSI_PLANES || header != BSI_HEADER ||
        nx_mul_overflow((size_t)words, 8u, &plane_size) || nx_mul_overflow(plane_size, planes, &expected) ||
        expected != payload || nx_cursor_left(&cur) != expected || crc != bsi_crc(bytes)) return NX_ERR_CORRUPT;
    nx_slice presence = nx_rd_bytes(&cur, plane_size);
    if (rows % 64u && (nx_ld64(presence.p + plane_size - 8u) >> (rows % 64u))) return NX_ERR_CORRUPT;
    for (uint32_t plane = 1; plane < BSI_PLANES; plane++) {
        nx_slice bits = nx_rd_bytes(&cur, plane_size);
        for (size_t w = 0; w < words; w++)
            if (nx_ld64(bits.p + w * 8u) & ~nx_ld64(presence.p + w * 8u)) return NX_ERR_CORRUPT;
    }
    if (cur.err) return NX_ERR_CORRUPT;
    nx_bsi view = {bytes, rows, words};
    *out = view;
    return NX_OK;
}

/* Evaluate one 64-row lane from most-significant plane to least. eq contains
 * the still-equal prefix; lt accumulates lanes that first differ below key. */
nx_status nx_bsi_get(const nx_bsi *index, uint32_t row, int64_t *value, bool *present) {
    if (value) *value = 0;
    if (present) *present = false;
    if (!index || !value || !present || row >= index->rows || !index->bytes.p ||
        index->rows > NX_BSI_MAX_ROWS || index->words != (index->rows + 63u) / 64u ||
        index->bytes.n != BSI_HEADER + (size_t)index->words * 8u * BSI_PLANES) return NX_ERR_INVALID;
    const uint8_t *base = index->bytes.p + BSI_HEADER + (size_t)(row / 64u) * 8u;
    uint64_t mask = UINT64_C(1) << (row % 64u);
    if (!(nx_ld64(base) & mask)) return NX_OK;
    uint64_t bits = 0;
    for (uint32_t plane = 0; plane < 64; plane++)
        if (nx_ld64(base + (size_t)(plane + 1u) * index->words * 8u) & mask) bits |= UINT64_C(1) << plane;
    bits ^= BSI_SIGN;
    /* Conversion above INT64_MAX is implementation-defined; memcpy preserves
     * the required two's-complement representation without that conversion. */
    memcpy(value, &bits, sizeof bits);
    *present = true;
    return NX_OK;
}

static uint64_t filter_word(const nx_bsi *index, uint32_t word, nx_compare op, uint64_t key) {
    size_t stride = (size_t)index->words * 8u;
    const uint8_t *base = index->bytes.p + BSI_HEADER + (size_t)word * 8u;
    uint64_t present = nx_ld64(base), eq = present, lt = 0;
    for (uint32_t plane = 64; plane && eq; plane--) {
        uint64_t bits = nx_ld64(base + (size_t)plane * stride);
        if ((key >> (plane - 1u)) & 1u) { lt |= eq & ~bits; eq &= bits; }
        else eq &= ~bits;
    }
    switch (op) {
        case NX_CMP_EQ: return eq;
        case NX_CMP_NE: return present & ~eq;
        case NX_CMP_LT: return lt;
        case NX_CMP_LE: return lt | eq;
        case NX_CMP_GT: return present & ~(lt | eq);
        case NX_CMP_GE: return present & ~lt;
    }
    return 0;
}

nx_status nx_bsi_filter(const nx_bsi *index, nx_compare op, int64_t value, nx_buf *out) {
    if (!index || !out || !index->bytes.p || index->bytes.n < BSI_HEADER ||
        index->rows > NX_BSI_MAX_ROWS || index->words != (index->rows + 63u) / 64u ||
        index->bytes.n != BSI_HEADER + (size_t)index->words * 8u * BSI_PLANES ||
        op < NX_CMP_EQ || op > NX_CMP_GE) return NX_ERR_INVALID;
    if (out->oom) return NX_ERR_NOMEM;
    struct descriptor { uint16_t key, count_minus_one; uint32_t offset; } desc[MAX_CONTAINERS];
    nx_buf data; nx_buf_init(&data);
    uint32_t count = 0;
    uint64_t key = (uint64_t)value ^ BSI_SIGN;
    for (uint32_t first = 0; first < index->words; first += CONTAINER_WORDS) {
        uint32_t n = NX_MIN(index->words - first, CONTAINER_WORDS), card = 0;
        uint64_t words[CONTAINER_WORDS] = {0};
        for (uint32_t w = 0; w < n; w++) {
            words[w] = filter_word(index, first + w, op, key);
            card += (uint32_t)nx_popcnt64(words[w]);
        }
        if (!card) continue;
        desc[count].key = (uint16_t)(first / CONTAINER_WORDS);
        desc[count].count_minus_one = (uint16_t)(card - 1u);
        desc[count++].offset = (uint32_t)data.len;
        if (card > ARRAY_LIMIT) {
            for (uint32_t w = 0; w < CONTAINER_WORDS; w++) nx_buf_put_u64(&data, words[w]);
        } else {
            for (uint32_t w = 0; w < n; w++) {
                uint64_t bits = words[w];
                while (bits) {
                    nx_buf_put_u16(&data, (uint16_t)(w * 64u + (uint32_t)nx_ctz64(bits)));
                    bits &= bits - 1u;
                }
            }
        }
        if (data.oom) { nx_buf_free(&data); return NX_ERR_NOMEM; }
    }
    size_t header = 8u + (size_t)count * 8u, size;
    nx_status st = NX_OK;
    if (nx_add_overflow(header, data.len, &size)) st = NX_ERR_LIMIT;
    else if (!nx_buf_reserve(out, size)) st = NX_ERR_NOMEM;
    if (st == NX_OK) {
        nx_buf_put_u32(out, ROARING_COOKIE); nx_buf_put_u32(out, count);
        for (uint32_t i = 0; i < count; i++) {
            nx_buf_put_u16(out, desc[i].key); nx_buf_put_u16(out, desc[i].count_minus_one);
        }
        for (uint32_t i = 0; i < count; i++) nx_buf_put_u32(out, (uint32_t)header + desc[i].offset);
        nx_buf_put(out, data.data, data.len);
    }
    nx_buf_free(&data);
    return st;
}
