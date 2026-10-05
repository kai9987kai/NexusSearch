#include "core/nx_config.h"
#include "nx_test.h"
#include "index/nx_trigram.h"
#include <string.h>

static void test_trigram_extract(void) {
    uint32_t trigs[16];
    size_t count = 0;

    /* Short strings */
    NX_CHECK_OK(nx_trigram_extract(nx_slice_cstr(""), trigs, 16, &count));
    NX_CHECK_EQ_U(count, 0);
    NX_CHECK_OK(nx_trigram_extract(nx_slice_cstr("ab"), trigs, 16, &count));
    NX_CHECK_EQ_U(count, 0);

    /* Exact 3 bytes */
    NX_CHECK_OK(nx_trigram_extract(nx_slice_cstr("abc"), trigs, 16, &count));
    NX_CHECK_EQ_U(count, 1);
    NX_CHECK_EQ_U(trigs[0], nx_trigram_pack('a', 'b', 'c'));

    /* Case-folding */
    uint32_t upper_trig[1];
    NX_CHECK_OK(nx_trigram_extract(nx_slice_cstr("ABC"), upper_trig, 1, &count));
    NX_CHECK_EQ_U(count, 1);
    NX_CHECK_EQ_U(upper_trig[0], nx_trigram_pack('a', 'b', 'c'));

    /* Deduplication: "aaaa" has 2 trigrams "aaa", "aaa" -> 1 unique */
    NX_CHECK_OK(nx_trigram_extract(nx_slice_cstr("aaaa"), trigs, 16, &count));
    NX_CHECK_EQ_U(count, 1);
    NX_CHECK_EQ_U(trigs[0], nx_trigram_pack('a', 'a', 'a'));

    /* 6 bytes: "abcdef" -> "abc", "bcd", "cde", "def" (4 trigrams) */
    NX_CHECK_OK(nx_trigram_extract(nx_slice_cstr("abcdef"), trigs, 16, &count));
    NX_CHECK_EQ_U(count, 4);
}

static void test_trigram_build_and_query(void) {
    nx_slice docs[4];
    docs[0] = nx_slice_cstr("hello world search engine");
    docs[1] = nx_slice_cstr("vector search database retrieval");
    docs[2] = nx_slice_cstr("safe immutable database storage");
    docs[3] = nx_slice_cstr("hello world safe storage");

    nx_buf index_buf;
    nx_buf_init(&index_buf);
    NX_REQUIRE(nx_trigram_build(docs, 4, &index_buf) == NX_OK);
    NX_CHECK(index_buf.len > 32);

    nx_trigram_index idx;
    NX_REQUIRE(nx_trigram_open(nx_buf_slice(&index_buf), &idx) == NX_OK);
    NX_CHECK_EQ_U(idx.doc_count, 4);
    NX_CHECK(idx.trigram_count > 0);

    /* 1. Point lookup for single trigram: "hel" -> docs 0 and 3 */
    nx_bitmap bm_hel;
    NX_CHECK_OK(nx_trigram_lookup(&idx, nx_trigram_pack('h', 'e', 'l'), &bm_hel));
    NX_CHECK_EQ_U(bm_hel.cardinality, 2);
    NX_CHECK(nx_bitmap_contains(&bm_hel, 0));
    NX_CHECK(nx_bitmap_contains(&bm_hel, 3));
    NX_CHECK(!nx_bitmap_contains(&bm_hel, 1));
    NX_CHECK(!nx_bitmap_contains(&bm_hel, 2));

    /* 2. Substring query: "search" -> docs 0 and 1 */
    nx_buf cand_buf;
    nx_buf_init(&cand_buf);
    bool has_cands = false;
    NX_CHECK_OK(nx_trigram_query_substring(&idx, nx_slice_cstr("search"), &cand_buf, &has_cands));
    NX_CHECK(has_cands);

    nx_bitmap bm_search;
    NX_CHECK_OK(nx_bitmap_open(nx_buf_slice(&cand_buf), &bm_search));
    NX_CHECK_EQ_U(bm_search.cardinality, 2);
    NX_CHECK(nx_bitmap_contains(&bm_search, 0));
    NX_CHECK(nx_bitmap_contains(&bm_search, 1));
    NX_CHECK(!nx_bitmap_contains(&bm_search, 2));
    NX_CHECK(!nx_bitmap_contains(&bm_search, 3));
    nx_buf_free(&cand_buf);

    /* 3. Substring query: "database" -> docs 1 and 2 */
    nx_buf_init(&cand_buf);
    NX_CHECK_OK(nx_trigram_query_substring(&idx, nx_slice_cstr("database"), &cand_buf, &has_cands));
    NX_CHECK(has_cands);
    nx_bitmap bm_db;
    NX_CHECK_OK(nx_bitmap_open(nx_buf_slice(&cand_buf), &bm_db));
    NX_CHECK_EQ_U(bm_db.cardinality, 2);
    NX_CHECK(nx_bitmap_contains(&bm_db, 1));
    NX_CHECK(nx_bitmap_contains(&bm_db, 2));
    nx_buf_free(&cand_buf);

    /* 4. Substring query: "hello world" -> docs 0 and 3 */
    nx_buf_init(&cand_buf);
    NX_CHECK_OK(nx_trigram_query_substring(&idx, nx_slice_cstr("hello world"), &cand_buf, &has_cands));
    NX_CHECK(has_cands);
    nx_bitmap bm_hw;
    NX_CHECK_OK(nx_bitmap_open(nx_buf_slice(&cand_buf), &bm_hw));
    NX_CHECK_EQ_U(bm_hw.cardinality, 2);
    NX_CHECK(nx_bitmap_contains(&bm_hw, 0));
    NX_CHECK(nx_bitmap_contains(&bm_hw, 3));
    nx_buf_free(&cand_buf);

    /* 5. Non-existent substring -> 0 candidates */
    nx_buf_init(&cand_buf);
    NX_CHECK_OK(nx_trigram_query_substring(&idx, nx_slice_cstr("quantum computing"), &cand_buf, &has_cands));
    NX_CHECK(has_cands);
    nx_bitmap bm_empty;
    NX_CHECK_OK(nx_bitmap_open(nx_buf_slice(&cand_buf), &bm_empty));
    NX_CHECK_EQ_U(bm_empty.cardinality, 0);
    nx_buf_free(&cand_buf);

    /* 6. Needle too short (< 3 bytes) -> fallback to scan (has_candidates == false) */
    nx_buf_init(&cand_buf);
    NX_CHECK_OK(nx_trigram_query_substring(&idx, nx_slice_cstr("hi"), &cand_buf, &has_cands));
    NX_CHECK(!has_cands);
    nx_buf_free(&cand_buf);

    nx_buf_free(&index_buf);
}

static void test_trigram_corruptions(void) {
    nx_slice docs[2];
    docs[0] = nx_slice_cstr("alpha beta gamma");
    docs[1] = nx_slice_cstr("delta epsilon zeta");

    nx_buf index_buf;
    nx_buf_init(&index_buf);
    NX_REQUIRE(nx_trigram_build(docs, 2, &index_buf) == NX_OK);

    /* 1. Truncated */
    nx_trigram_index idx;
    NX_CHECK_EQ_I(nx_trigram_open(nx_slice_make(index_buf.data, 20), &idx), NX_ERR_CORRUPT);

    /* 2. Corrupted CRC */
    uint8_t copy[1024];
    NX_REQUIRE(index_buf.len <= sizeof(copy));
    memcpy(copy, index_buf.data, index_buf.len);
    copy[index_buf.len - 1] ^= 0x55;
    NX_CHECK_EQ_I(nx_trigram_open(nx_slice_make(copy, index_buf.len), &idx), NX_ERR_CORRUPT);

    /* 3. Bad magic */
    memcpy(copy, index_buf.data, index_buf.len);
    copy[0] = 'X';
    NX_CHECK_EQ_I(nx_trigram_open(nx_slice_make(copy, index_buf.len), &idx), NX_ERR_CORRUPT);

    nx_buf_free(&index_buf);
}

int main(void) {
    NX_RUN(test_trigram_extract);
    NX_RUN(test_trigram_build_and_query);
    NX_RUN(test_trigram_corruptions);
    return nx_test_summary();
}
