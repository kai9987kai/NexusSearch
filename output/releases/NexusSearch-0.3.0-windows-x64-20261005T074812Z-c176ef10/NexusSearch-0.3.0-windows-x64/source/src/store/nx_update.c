#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "store/nx_update.h"
#include "core/nx_file.h"
#include "core/nx_json.h"
#include "core/nx_hash.h"
#include "core/nx_mem.h"
#include "seg/nx_table.h"
#ifdef NX_WINDOWS
#include <windows.h>
typedef HANDLE update_lock;
#define NO_LOCK INVALID_HANDLE_VALUE
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
typedef int update_lock;
#define NO_LOCK (-1)
#endif

#define UPDATE_BYTES (64u * 1024u * 1024u)
#define UPDATE_ROWS 100000u
#define SNAPSHOT_BYTES (1024u * 1024u * 1024u)
#define UPDATE_WORK (512u * 1024u * 1024u)

typedef struct update_row { nx_slice id, document; bool deleted; } update_row;
typedef struct update_state {
    update_row *rows;
    size_t *slots;
    size_t count, capacity, slot_count, work;
    nx_arena ids, parser;
} update_state;

static nx_status lock_open(const char *path, update_lock *out) {
    size_t n;
    *out = NO_LOCK;
    if (nx_add_overflow(strlen(path), 6, &n)) return NX_ERR_LIMIT;
    char *name = (char *)nx_malloc(n);
    if (!name) return NX_ERR_NOMEM;
    memcpy(name, path, n - 6); memcpy(name + n - 6, ".lock", 6);
    nx_status st = NX_OK;
#ifdef NX_WINDOWS
    int wide_n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, NULL, 0);
    wchar_t *wide = wide_n > 0 ? NX_NEW_ARRAY(wchar_t, (size_t)wide_n) : NULL;
    if (wide_n <= 0) st = NX_ERR_INVALID;
    else if (!wide) st = NX_ERR_NOMEM;
    else if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, wide_n)) st = NX_ERR_INVALID;
    else {
        HANDLE file = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) st = NX_ERR_IO;
        else {
            OVERLAPPED offset; memset(&offset, 0, sizeof offset);
            if (LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                           0, 1, 0, &offset)) *out = file;
            else {
                st = GetLastError() == ERROR_LOCK_VIOLATION ? NX_ERR_BUSY : NX_ERR_IO;
                CloseHandle(file);
            }
        }
    }
    nx_free(wide);
#else
    int file = open(name, O_CREAT | O_RDWR, 0600);
    if (file < 0) st = NX_ERR_IO;
    else if (flock(file, LOCK_EX | LOCK_NB) == 0) *out = file;
    else { st = errno == EWOULDBLOCK || errno == EAGAIN ? NX_ERR_BUSY : NX_ERR_IO; close(file); }
#endif
    nx_free(name); return st;
}
static void lock_close(update_lock lock) {
    if (lock == NO_LOCK) return;
#ifdef NX_WINDOWS
    CloseHandle(lock);
#else
    close(lock);
#endif
}
static bool whitespace(uint8_t c) { return c == ' ' || c == '\t' || c == '\r'; }
static nx_slice next_line(nx_slice input, size_t *position) {
    size_t begin = *position, end = begin;
    while (end < input.n && input.p[end] != '\n') end++;
    *position = end < input.n ? end + 1 : end;
    while (begin < end && whitespace(input.p[begin])) begin++;
    while (end > begin && whitespace(input.p[end - 1])) end--;
    return nx_slice_sub(input, begin, end - begin);
}
static nx_status find_row(update_state *s, nx_slice id, size_t *out) {
    if (id.n > UPDATE_WORK - s->work) return NX_ERR_LIMIT;
    s->work += id.n;
    size_t slot = (size_t)nx_hash64(id.p, id.n, UINT64_C(0xB046DDC139A47)) & (s->slot_count - 1);
    for (;;) {
        if (s->work == UPDATE_WORK) return NX_ERR_LIMIT;
        s->work++;
        size_t row = s->slots[slot];
        if (!row) {
            if (s->count == s->capacity) return NX_ERR_LIMIT;
            uint8_t *copy = (uint8_t *)nx_arena_alloc(&s->ids, id.n, 1);
            if (!copy) return NX_ERR_NOMEM;
            memcpy(copy, id.p, id.n);
            *out = s->count++; s->slots[slot] = *out + 1;
            s->rows[*out].id = nx_slice_make(copy, id.n);
            s->rows[*out].deleted = true;
            return NX_OK;
        }
        if (s->rows[row - 1].id.n == id.n) {
            if (id.n > UPDATE_WORK - s->work) return NX_ERR_LIMIT;
            s->work += id.n;
            if (nx_slice_eq(s->rows[row - 1].id, id)) { *out = row - 1; return NX_OK; }
        }
        slot = (slot + 1) & (s->slot_count - 1);
    }
}
static nx_status apply_line(update_state *s, nx_slice line, nx_buf *validation, nx_error *error) {
    nx_arena_reset(&s->parser);
    nx_json_limits limits = nx_json_default_limits();
    limits.max_input_bytes = UPDATE_BYTES; limits.max_string_bytes = UPDATE_BYTES;
    limits.max_nodes = 4 + 128 * (4096 + 1); limits.max_depth = 4;
    limits.max_work = (line.n + 1) * 140;
    nx_json *root = NULL;
    nx_status st = nx_json_parse(&s->parser, line, &limits, &root, error);
    if (st != NX_OK) return st;
    if (root->kind != NX_JSON_OBJECT || root->count != 2) return NX_ERR_INVALID;
    const nx_json *op = nx_json_get_cstr(root, "op");
    if (!op || op->kind != NX_JSON_STRING) return NX_ERR_INVALID;
    bool upsert = nx_slice_eq(op->as.string, nx_slice_cstr("upsert"));
    bool deletion = nx_slice_eq(op->as.string, nx_slice_cstr("delete"));
    if (!upsert && !deletion) return NX_ERR_INVALID;
    const nx_json *doc = upsert ? nx_json_get_cstr(root, "document") : NULL;
    const nx_json *id = upsert ? nx_json_get_cstr(doc, "_id") : nx_json_get_cstr(root, "id");
    if (!id || id->kind != NX_JSON_STRING || !id->as.string.n ||
        id->as.string.n > 1024u * 1024u || memchr(id->as.string.p, 0, id->as.string.n)) return NX_ERR_INVALID;
    nx_slice document = nx_slice_make(NULL, 0);
    if (upsert) {
        if (!doc || doc->kind != NX_JSON_OBJECT) return NX_ERR_TYPE;
        document = nx_slice_sub(line, doc->offset, doc->length);
        /* Validate even an upsert later deleted/replaced in the same batch. */
        nx_buf_clear(validation);
        st = nx_table_build(document, NULL, validation, error);
        if (st != NX_OK) return st;
    }
    size_t row;
    st = find_row(s, id->as.string, &row);
    if (st == NX_OK) { s->rows[row].document = document; s->rows[row].deleted = deletion; }
    return st;
}

nx_status nx_update_snapshot(const char *path, nx_slice mutations, nx_error *error) {
    nx_error_clear(error);
    nx_status st = NX_OK;
    update_lock lock = NO_LOCK;
    nx_mmap map = {0}; nx_table old = {0}, verified = {0};
    update_state state; memset(&state, 0, sizeof state);
    nx_arena_init(&state.ids, 0); nx_arena_init(&state.parser, 0);
    nx_buf jsonl, snapshot, validation;
    nx_buf_init(&jsonl); nx_buf_init(&snapshot); nx_buf_init(&validation);
    if (!path || !*path || (!mutations.p && mutations.n)) { st = NX_ERR_INVALID; goto done; }
    if (mutations.n > UPDATE_BYTES) { st = NX_ERR_LIMIT; goto done; }
    size_t operations = 0, position = 0;
    while (position < mutations.n) {
        if (next_line(mutations, &position).n && ++operations > UPDATE_ROWS) { st = NX_ERR_LIMIT; goto done; }
    }
    st = lock_open(path, &lock);
    if (st != NX_OK) goto done;
    st = nx_mmap_open(path, SNAPSHOT_BYTES, &map);
    if (st != NX_OK) goto done;
    st = nx_table_open(nx_slice_make(map.data, map.size), &old, error);
    if (st != NX_OK) goto done;
    if (old.rows > UPDATE_ROWS) { st = NX_ERR_LIMIT; goto done; }
    if (!operations) goto done;
    state.capacity = (size_t)old.rows + operations;
    state.slot_count = 2;
    while (state.slot_count < state.capacity * 2) state.slot_count *= 2;
    state.rows = NX_NEW_ARRAY(update_row, state.capacity);
    state.slots = (size_t *)nx_calloc(state.slot_count, sizeof *state.slots);
    if (!state.rows || !state.slots) { st = NX_ERR_NOMEM; goto done; }
    for (uint32_t i = 0; i < old.rows; i++) {
        size_t row;
        st = find_row(&state, nx_table_id(&old, i), &row);
        if (st != NX_OK) goto done;
        state.rows[row].document = nx_table_document(&old, i); state.rows[row].deleted = false;
    }
    position = 0;
    while (position < mutations.n) {
        size_t begin = position;
        nx_slice line = next_line(mutations, &position);
        if (!line.n) continue;
        st = apply_line(&state, line, &validation, error);
        if (st != NX_OK) {
            if (error && error->code != NX_OK && error->pos >= 0)
                error->pos += (int64_t)(line.p - mutations.p);
            else nx_error_set(error, st, (int64_t)begin, (int64_t)(position - begin), "invalid update operation: %s", nx_status_str(st));
            goto done;
        }
    }
    size_t rows = 0;
    for (size_t i = 0; i < state.count; i++) {
        if (state.rows[i].deleted) continue;
        nx_slice document = state.rows[i].document;
        if (++rows > UPDATE_ROWS || document.n >= UPDATE_BYTES - jsonl.len) { st = NX_ERR_LIMIT; goto done; }
        nx_buf_put(&jsonl, document.p, document.n); nx_buf_put_u8(&jsonl, '\n');
        if (jsonl.oom) { st = NX_ERR_NOMEM; goto done; }
    }
    st = nx_table_build(nx_buf_slice(&jsonl), NULL, &snapshot, error);
    if (st != NX_OK) goto done;
    st = nx_table_open(nx_buf_slice(&snapshot), &verified, error);
    if (st != NX_OK) goto done;
    /* No allocation or validation after this call can turn a successful
     * publication into NOMEM. IO is the sole indeterminate commit outcome. */
    st = nx_file_write_atomic(path, snapshot.data, snapshot.len);
    if (st == NX_ERR_IO)
        nx_error_set(error, st, -1, 0, "snapshot publication IO error; commit may have occurred; reopen before retrying");
done:
    nx_free(state.rows); nx_free(state.slots);
    nx_arena_free(&state.ids); nx_arena_free(&state.parser);
    nx_buf_free(&jsonl); nx_buf_free(&snapshot); nx_buf_free(&validation);
    nx_mmap_close(&map); lock_close(lock);
    if (st != NX_OK && (!error || error->code == NX_OK))
        nx_error_set(error, st, -1, 0, "snapshot update failed: %s", nx_status_str(st));
    return st;
}
