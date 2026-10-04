#include "core/nx_config.h"
#include "nx_test.h"
#include "store/nx_wal.h"
#include "core/nx_file.h"
#include <string.h>

typedef struct replay_tracker {
    size_t upsert_count;
    size_t delete_count;
    size_t checkpoint_count;
    char last_id[64];
} replay_tracker;

static nx_status on_replay_record(nx_wal_op op, nx_slice payload, void *user_data) {
    replay_tracker *tr = (replay_tracker *)user_data;
    if (op == NX_WAL_OP_UPSERT) {
        tr->upsert_count++;
    } else if (op == NX_WAL_OP_DELETE) {
        tr->delete_count++;
        size_t n = payload.n < sizeof(tr->last_id) - 1 ? payload.n : sizeof(tr->last_id) - 1;
        memcpy(tr->last_id, payload.p, n);
        tr->last_id[n] = '\0';
    } else if (op == NX_WAL_OP_CHECKPOINT) {
        tr->checkpoint_count++;
    }
    return NX_OK;
}

static void test_wal_lifecycle(void) {
    const char *path = "test_store.wal";
    (void)nx_file_remove(path);

    /* 1. Create WAL */
    nx_wal wal;
    NX_REQUIRE(nx_wal_open(path, &wal) == NX_OK);
    NX_CHECK_EQ_U(wal.file_size, 32);

    /* 2. Append mutations */
    const char doc1[] = "{\"_id\":\"doc-1\",\"title\":\"alpha\"}";
    const char doc2[] = "{\"_id\":\"doc-2\",\"title\":\"beta\"}";
    const char doc3[] = "{\"_id\":\"doc-3\",\"title\":\"gamma\"}";

    NX_CHECK_OK(nx_wal_append_upsert(&wal, nx_slice_cstr(doc1)));
    NX_CHECK_OK(nx_wal_append_upsert(&wal, nx_slice_cstr(doc2)));
    NX_CHECK_OK(nx_wal_append_upsert(&wal, nx_slice_cstr(doc3)));
    NX_CHECK_OK(nx_wal_append_delete(&wal, nx_slice_cstr("doc-2")));
    NX_CHECK_OK(nx_wal_append_checkpoint(&wal, 100));

    NX_CHECK_OK(nx_wal_sync(&wal));
    nx_wal_close(&wal);

    /* 3. Replay WAL */
    replay_tracker tr = {0};
    size_t records = 0;
    NX_REQUIRE(nx_wal_replay(path, on_replay_record, &tr, &records) == NX_OK);
    NX_CHECK_EQ_U(records, 5);
    NX_CHECK_EQ_U(tr.upsert_count, 3);
    NX_CHECK_EQ_U(tr.delete_count, 1);
    NX_CHECK_EQ_U(tr.checkpoint_count, 1);
    NX_CHECK(!strcmp(tr.last_id, "doc-2"));

    /* 4. Truncate WAL */
    NX_REQUIRE(nx_wal_open(path, &wal) == NX_OK);
    NX_CHECK_OK(nx_wal_truncate(&wal));
    NX_CHECK_EQ_U(wal.file_size, 32);
    nx_wal_close(&wal);

    /* Verify replay after truncate sees 0 records */
    memset(&tr, 0, sizeof(tr));
    records = 0;
    NX_REQUIRE(nx_wal_replay(path, on_replay_record, &tr, &records) == NX_OK);
    NX_CHECK_EQ_U(records, 0);

    (void)nx_file_remove(path);
}

static void test_wal_torn_write_recovery(void) {
    const char *path = "test_torn.wal";
    (void)nx_file_remove(path);

    nx_wal wal;
    NX_REQUIRE(nx_wal_open(path, &wal) == NX_OK);
    NX_CHECK_OK(nx_wal_append_upsert(&wal, nx_slice_cstr("{\"_id\":\"doc-A\"}")));
    NX_CHECK_OK(nx_wal_append_upsert(&wal, nx_slice_cstr("{\"_id\":\"doc-B\"}")));
    NX_CHECK_OK(nx_wal_sync(&wal));
    nx_wal_close(&wal);

    /* Corrupt the end by appending partial frame bytes (simulating sudden power loss mid-write) */
    FILE *fp = fopen(path, "ab");
    NX_REQUIRE(fp != NULL);
    uint8_t garbage[7] = {0x46, 0x4C, 0x41, 0x57, 0x01, 0x00, 0x12};
    fwrite(garbage, 1, sizeof(garbage), fp);
    fclose(fp);

    /* Replay must gracefully recover intact records and stop at the torn write */
    replay_tracker tr = {0};
    size_t records = 0;
    NX_CHECK_OK(nx_wal_replay(path, on_replay_record, &tr, &records));
    NX_CHECK_EQ_U(records, 2);
    NX_CHECK_EQ_U(tr.upsert_count, 2);

    (void)nx_file_remove(path);
}

int main(void) {
    NX_RUN(test_wal_lifecycle);
    NX_RUN(test_wal_torn_write_recovery);
    return nx_test_summary();
}
