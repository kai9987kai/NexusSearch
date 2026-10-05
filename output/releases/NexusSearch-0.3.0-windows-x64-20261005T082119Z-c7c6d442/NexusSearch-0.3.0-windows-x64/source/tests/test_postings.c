#include "core/nx_config.h"
#include "nx_test.h"
#include "index/nx_postings.h"
#include <string.h>

static void test_postings_build_and_lookup(void) {
    nx_slice doc0_toks[] = {
        nx_slice_cstr("local"),
        nx_slice_cstr("search"),
        nx_slice_cstr("engine"),
        nx_slice_cstr("search") /* tf = 2 */
    };
    nx_slice doc1_toks[] = {
        nx_slice_cstr("vector"),
        nx_slice_cstr("search"),
        nx_slice_cstr("foundations")
    };
    nx_slice doc2_toks[] = {
        nx_slice_cstr("safe"),
        nx_slice_cstr("immutable"),
        nx_slice_cstr("storage")
    };

    nx_postings_doc docs[3];
    docs[0].tokens = doc0_toks; docs[0].token_count = 4;
    docs[1].tokens = doc1_toks; docs[1].token_count = 3;
    docs[2].tokens = doc2_toks; docs[2].token_count = 3;

    nx_buf index_buf;
    nx_buf_init(&index_buf);
    NX_REQUIRE(nx_postings_build(docs, 3, &index_buf) == NX_OK);
    NX_CHECK(index_buf.len > 48);

    nx_postings_index idx;
    NX_REQUIRE(nx_postings_open(nx_buf_slice(&index_buf), &idx) == NX_OK);
    NX_CHECK_EQ_U(idx.doc_count, 3);
    NX_CHECK_EQ_U(idx.field_doc_count, 3);
    NX_CHECK_EQ_U(idx.total_doc_len, 10);
    NX_CHECK(idx.avgdl > 3.3f && idx.avgdl < 3.4f);

    /* 1. Lookup "search" -> df = 2, docs 0 and 1 */
    uint32_t df = 0;
    nx_bitmap bm_search;
    NX_CHECK_OK(nx_postings_lookup(&idx, nx_slice_cstr("search"), &df, &bm_search));
    NX_CHECK_EQ_U(df, 2);
    NX_CHECK_EQ_U(bm_search.cardinality, 2);
    NX_CHECK(nx_bitmap_contains(&bm_search, 0));
    NX_CHECK(nx_bitmap_contains(&bm_search, 1));
    NX_CHECK(!nx_bitmap_contains(&bm_search, 2));

    /* 2. Lookup "immutable" -> df = 1, doc 2 */
    nx_bitmap bm_imm;
    NX_CHECK_OK(nx_postings_lookup(&idx, nx_slice_cstr("immutable"), &df, &bm_imm));
    NX_CHECK_EQ_U(df, 1);
    NX_CHECK_EQ_U(bm_imm.cardinality, 1);
    NX_CHECK(nx_bitmap_contains(&bm_imm, 2));
    NX_CHECK(!nx_bitmap_contains(&bm_imm, 0));

    /* 3. Lookup non-existent term -> NOT_FOUND */
    nx_bitmap bm_none;
    NX_CHECK_EQ_I(nx_postings_lookup(&idx, nx_slice_cstr("quantum"), &df, &bm_none), NX_ERR_NOT_FOUND);

    /* 4. BM25 Scoring: doc 0 has tf=2, doc 1 has tf=1 */
    double score0 = nx_postings_bm25_score(&idx, nx_slice_cstr("search"), 0, 1.2, 0.75);
    double score1 = nx_postings_bm25_score(&idx, nx_slice_cstr("search"), 1, 1.2, 0.75);
    double score2 = nx_postings_bm25_score(&idx, nx_slice_cstr("search"), 2, 1.2, 0.75);

    NX_CHECK(score0 > 0.0);
    NX_CHECK(score1 > 0.0);
    NX_CHECK(score0 > score1); /* higher term frequency = higher score */
    NX_CHECK_EQ_I((int)score2, 0); /* absent = 0 */

    nx_buf_free(&index_buf);
}

static void test_postings_corruptions(void) {
    nx_slice tok = nx_slice_cstr("word");
    nx_postings_doc doc;
    doc.tokens = &tok; doc.token_count = 1;

    nx_buf index_buf;
    nx_buf_init(&index_buf);
    NX_REQUIRE(nx_postings_build(&doc, 1, &index_buf) == NX_OK);

    /* 1. Truncated */
    nx_postings_index idx;
    NX_CHECK_EQ_I(nx_postings_open(nx_slice_make(index_buf.data, 30), &idx), NX_ERR_CORRUPT);

    /* 2. Corrupted CRC */
    uint8_t copy[1024];
    NX_REQUIRE(index_buf.len <= sizeof(copy));
    memcpy(copy, index_buf.data, index_buf.len);
    copy[index_buf.len - 1] ^= 0x55;
    NX_CHECK_EQ_I(nx_postings_open(nx_slice_make(copy, index_buf.len), &idx), NX_ERR_CORRUPT);

    /* 3. Bad magic */
    memcpy(copy, index_buf.data, index_buf.len);
    copy[0] = 'Z';
    NX_CHECK_EQ_I(nx_postings_open(nx_slice_make(copy, index_buf.len), &idx), NX_ERR_CORRUPT);

    nx_buf_free(&index_buf);
}

int main(void) {
    NX_RUN(test_postings_build_and_lookup);
    NX_RUN(test_postings_corruptions);
    return nx_test_summary();
}
