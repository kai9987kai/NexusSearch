#include "store/nx_update.h"
#include "core/nx_file.h"
#include "core/nx_mem.h"
#include "seg/nx_table.h"
#include "nx_test.h"

static char snapshot_path[256];

static nx_status read_table(nx_mmap *mapping, nx_table *table) {
    nx_status st = nx_mmap_open(snapshot_path, 1024u * 1024u, mapping);
    return st == NX_OK ? nx_table_open(nx_slice_make(mapping->data, mapping->size), table, NULL) : st;
}
static bool read_all(uint8_t **bytes, size_t *size) {
    return nx_file_read(snapshot_path, 1024u * 1024u, bytes, size) == NX_OK;
}
static void expect_same_file(const uint8_t *before, size_t before_size) {
    uint8_t *after = NULL; size_t after_size = 0;
    NX_REQUIRE(read_all(&after, &after_size));
    NX_CHECK_EQ_U(after_size, before_size);
    if (after && after_size == before_size) NX_CHECK(memcmp(after, before, before_size) == 0);
    nx_free(after);
}

static void test_update_commit_order_and_rollback(void) {
    const char *initial =
        "{\"_id\":\"a\",\"title\":\"old\",\"n\":1}\n"
        "{\"_id\":\"b\",\"title\":\"remove\",\"n\":2}\n";
    nx_buf snapshot; nx_buf_init(&snapshot);
    NX_REQUIRE(nx_table_build(nx_slice_cstr(initial), NULL, &snapshot, NULL) == NX_OK);
    NX_REQUIRE(nx_file_write_atomic(snapshot_path, snapshot.data, snapshot.len) == NX_OK);
    uint8_t *original = NULL; size_t original_size = 0;
    NX_REQUIRE(read_all(&original, &original_size));

    const char *batch =
        "{\"op\":\"upsert\",\"document\":{\"_id\":\"a\",\"title\":\"new\",\"n\":3}}\n"
        "{\"op\":\"delete\",\"id\":\"b\"}\n"
        "{\"op\":\"delete\",\"id\":\"missing\"}\n"
        "{\"op\":\"upsert\",\"document\":{\"_id\":\"c\",\"title\":\"first\",\"n\":4}}\n"
        "{\"op\":\"upsert\",\"document\":{\"_id\":\"c\",\"title\":\"last\",\"n\":5}}\n";
    NX_CHECK_OK(nx_update_snapshot(snapshot_path, nx_slice_cstr(batch), NULL));
    nx_mmap mapping = {0}; nx_table table;
    NX_REQUIRE(read_table(&mapping, &table) == NX_OK);
    NX_CHECK_EQ_U(table.rows, 2);
    NX_CHECK(nx_slice_eq(nx_table_id(&table, 0), nx_slice_cstr("a")));
    NX_CHECK(nx_slice_eq(nx_table_id(&table, 1), nx_slice_cstr("c")));
    nx_cell cell;
    uint32_t title_field = 0;
    NX_CHECK_OK(nx_table_find(&table, nx_slice_cstr("title"), &title_field));
    NX_CHECK_OK(nx_table_get(&table, 1, title_field, &cell));
    NX_CHECK(nx_slice_eq(cell.as.text, nx_slice_cstr("last")));
    nx_mmap_close(&mapping);

    uint8_t *committed = NULL; size_t committed_size = 0;
    NX_REQUIRE(read_all(&committed, &committed_size));
    const char *invalid =
        "{\"op\":\"upsert\",\"document\":{\"_id\":\"a\",\"title\":\"partial\",\"n\":6}}\n"
        "{\"op\":\"upsert\",\"document\":{\"_id\":\"c\",\"title\":\"bad schema\",\"n\":\"text\"}}\n";
    NX_CHECK(nx_update_snapshot(snapshot_path, nx_slice_cstr(invalid), NULL) != NX_OK);
    expect_same_file(committed, committed_size);
    NX_CHECK_EQ_I(nx_update_snapshot(snapshot_path, nx_slice_cstr("{broken\n"), NULL), NX_ERR_PARSE);
    expect_same_file(committed, committed_size);
    NX_CHECK_OK(nx_update_snapshot(snapshot_path, nx_slice_make(NULL, 0), NULL));
    expect_same_file(committed, committed_size);
    nx_free(committed); nx_free(original); nx_buf_free(&snapshot);
}

static nx_status update_oom(void *opaque) {
    const char *batch = (const char *)opaque;
    return nx_update_snapshot(snapshot_path, nx_slice_cstr(batch), NULL);
}
static void test_update_oom_and_path_errors(void) {
    const char *initial = "{\"_id\":\"a\",\"value\":1}\n";
    nx_buf snapshot; nx_buf_init(&snapshot);
    NX_REQUIRE(nx_table_build(nx_slice_cstr(initial), NULL, &snapshot, NULL) == NX_OK);
    NX_REQUIRE(nx_file_write_atomic(snapshot_path, snapshot.data, snapshot.len) == NX_OK);
    const char *batch = "{\"op\":\"upsert\",\"document\":{\"_id\":\"b\",\"value\":2}}\n";
    nx_test_oom_sweep(update_oom, (void *)batch, 256);
    NX_CHECK_EQ_I(nx_update_snapshot("", nx_slice_cstr(batch), NULL), NX_ERR_INVALID);
    NX_CHECK_EQ_I(nx_update_snapshot(snapshot_path, nx_slice_make(NULL, 1), NULL), NX_ERR_INVALID);
    nx_buf_free(&snapshot);
}

int main(void) {
    (void)snprintf(snapshot_path, sizeof(snapshot_path), "nx-update-%llu-\xc3\xa9.nxs", (unsigned long long)nx_now_ns());
    NX_RUN(test_update_commit_order_and_rollback);
    NX_RUN(test_update_oom_and_path_errors);
    (void)nx_file_remove(snapshot_path);
    char lock_path[280];
    (void)snprintf(lock_path, sizeof(lock_path), "%s.lock", snapshot_path);
    (void)nx_file_remove(lock_path);
    return nx_test_summary();
}
