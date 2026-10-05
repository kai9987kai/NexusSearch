#include "nx_test.h"
#include "core/nx_arena.h"

static void test_rollback_reclaims_oversize(void) {
    nx_arena a;
    nx_arena_init(&a, 128);
    char *keep = nx_arena_strdup(&a, "keep");
    NX_REQUIRE(keep);
    nx_arena_mark mark = nx_arena_save(&a);
    nx_mem_stats before = nx_mem_get_stats();
    for (size_t i = 0; i < 20; i++) {
        NX_CHECK(nx_arena_alloc(&a, 8192, 64) != NULL);
        nx_arena_mark inner = nx_arena_save(&a);
        NX_CHECK(nx_arena_alloc(&a, 16384, 64) != NULL);
        nx_arena_restore(&a, inner);
        nx_arena_restore(&a, mark);
        NX_CHECK_EQ_U(nx_mem_get_stats().live_bytes, before.live_bytes);
        NX_CHECK_EQ_S(keep, "keep");
    }
    nx_arena_free(&a);
}

static void test_arena_huge_chunk_rejected(void) {
    nx_arena a;
    nx_arena_init(&a, SIZE_MAX);
    /* Reject header + payload overflow before attempting an allocation. */
    nx_mem_fail_after(0);
    NX_CHECK(nx_arena_alloc(&a, 1, 1) == NULL);
    nx_mem_fail_after(-1);
    NX_CHECK(a.oom);
    nx_arena_reset(&a);
    NX_CHECK(!a.oom);
    nx_arena_free(&a);
}

int main(void) {
    NX_RUN(test_rollback_reclaims_oversize);
    NX_RUN(test_arena_huge_chunk_rejected);
    return nx_test_summary();
}
