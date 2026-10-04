/* nx_store.c - Multi-Segment Persistent Store implementation.
 *
 * See nx_store.h for design notes.
 */
#include "core/nx_config.h"
#include "core/nx_mem.h"
#include "core/nx_buf.h"
#include "core/nx_status.h"
#include "core/nx_json.h"
#include "core/nx_arena.h"
#include "core/nx_file.h"
#include "store/nx_store.h"
#include "store/nx_wal.h"
#include "seg/nx_seg_builder.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef NX_WINDOWS
#  include <windows.h>
#  include <direct.h>      /* _mkdir */
#  define nx_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define nx_mkdir(p) mkdir((p), 0755)
#endif

/* ---- path helpers ------------------------------------------------------- */
static void path_join(char *dst, size_t dstcap,
                      const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    if (dl + nl + 2 < dstcap) {
        memcpy(dst, dir, dl);
        dst[dl] = '/';
        memcpy(dst + dl + 1, name, nl);
        dst[dl + 1 + nl] = '\0';
    } else {
        /* Truncate gracefully: copy as much as fits */
        size_t copy_d = dstcap > 1 ? dstcap - 1 : 0;
        if (dl < copy_d) {
            memcpy(dst, dir, dl);
            dst[dl] = '/';
            size_t rem = copy_d - dl - 1;
            size_t nc = nl < rem ? nl : rem;
            if (nc > 0) memcpy(dst + dl + 1, name, nc);
            dst[dl + 1 + nc] = '\0';
        } else {
            if (copy_d > 0) memcpy(dst, dir, copy_d);
            dst[copy_d] = '\0';
        }
    }
}

/* ---- ensure directory exists -------------------------------------------- */
static nx_status ensure_dir(const char *path) {
    if (nx_mkdir(path) != 0) {
#ifdef NX_WINDOWS
        if (GetLastError() != ERROR_ALREADY_EXISTS) return NX_ERR_IO;
#else
        if (errno != EEXIST) return NX_ERR_IO;
#endif
    }
    return NX_OK;
}

/* ---- manifest serialisation -------------------------------------------- */

static nx_status write_manifest(const nx_store *store, uint64_t gen, nx_error *error) {
    nx_buf b;
    nx_buf_init(&b);

    /* Build JSON manually (no arena required for a simple flat object) */
    nx_buf_printf(&b, "{\"gen\":%llu,\"next_seg_id\":%llu,\"segments\":[",
                  (unsigned long long)gen,
                  (unsigned long long)store->next_seg_id);

    for (uint32_t i = 0; i < store->seg_count; i++) {
        const nx_store_seg_entry *s = &store->segs[i];
        if (i > 0) nx_buf_printf(&b, ",");
        /* Extract basename from file path */
        const char *base = strrchr(s->file, '/');
        base = base ? base + 1 : s->file;
        nx_buf_printf(&b, "{\"id\":%llu,\"file\":\"%s\",\"docs\":%u}",
                      (unsigned long long)s->seg_id, base,
                      (unsigned)s->doc_count);
    }
    nx_buf_printf(&b, "]}");

    if (!b.data) { nx_buf_free(&b); return NX_ERR_NOMEM; }

    /* Write to manifest-<gen>.json atomically */
    char mpath[NX_STORE_MAX_PATH];
    char mname[64];
    snprintf(mname, sizeof mname, "manifest-%llu.json", (unsigned long long)gen);
    path_join(mpath, sizeof mpath, store->dir, mname);

    nx_status st = nx_file_write_atomic(mpath, b.data, b.len);
    nx_buf_free(&b);
    if (st != NX_OK && error) {
        nx_error_set(error, st, -1, 0, "failed to write manifest %s", mpath);
    }
    return st;
}

/* Parse a manifest JSON file. Returns seg entries via out_segs, count via out_count.
 * Also fills out_gen, out_next_id. */
static nx_status read_manifest(const char *path, nx_error *error,
                                nx_store_seg_entry *out_segs,
                                uint32_t max_segs, uint32_t *out_count,
                                uint64_t *out_gen, uint64_t *out_next_id,
                                const char *store_dir)
{
    uint8_t *data = NULL;
    size_t   size = 0;
    nx_status st = nx_file_read(path, 4 * 1024 * 1024, &data, &size);
    if (st != NX_OK) return st;

    nx_arena arena;
    nx_arena_init(&arena, 64 * 1024);
    nx_json *root = NULL;
    nx_slice input = nx_slice_make(data, size);
    st = nx_json_parse(&arena, input, NULL, &root, error);
    nx_free(data);
    if (st != NX_OK) { nx_arena_free(&arena); return st; }

    *out_gen      = 0;
    *out_next_id  = 1;
    *out_count    = 0;

    const nx_json *jgen = nx_json_get_cstr(root, "gen");
    if (jgen) nx_json_get_i64(jgen, (int64_t *)out_gen);

    const nx_json *jnext = nx_json_get_cstr(root, "next_seg_id");
    if (jnext) nx_json_get_i64(jnext, (int64_t *)out_next_id);

    const nx_json *jsegs = nx_json_get_cstr(root, "segments");
    if (jsegs && jsegs->kind == NX_JSON_ARRAY) {
        const nx_json *item = jsegs->child;
        while (item && *out_count < max_segs) {
            uint32_t idx = *out_count;
            memset(&out_segs[idx], 0, sizeof out_segs[idx]);

            const nx_json *jid = nx_json_get_cstr(item, "id");
            if (jid) nx_json_get_i64(jid, (int64_t *)&out_segs[idx].seg_id);

            const nx_json *jfile = nx_json_get_cstr(item, "file");
            if (jfile && jfile->kind == NX_JSON_STRING) {
                /* Build absolute path by joining store_dir + basename */
                path_join(out_segs[idx].file, sizeof out_segs[idx].file,
                          store_dir, (const char *)jfile->as.string.p);
            }

            const nx_json *jdocs = nx_json_get_cstr(item, "docs");
            if (jdocs) {
                int64_t dc = 0;
                nx_json_get_i64(jdocs, &dc);
                out_segs[idx].doc_count = (uint32_t)dc;
            }

            (*out_count)++;
            item = item->next;
        }
    }

    nx_arena_free(&arena);
    return NX_OK;
}

/* Find the highest valid manifest generation in the store directory.
 * Returns NX_OK with gen=0 if no manifest found (fresh store). */
static nx_status find_latest_manifest(const char *dir, uint64_t *out_gen) {
    *out_gen = 0;
    bool found = false;

#ifdef NX_WINDOWS
    char pattern[NX_STORE_MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s/manifest-*.json", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NX_OK;
    do {
        uint64_t gen = 0;
        if (sscanf(fd.cFileName, "manifest-%llu.json", (unsigned long long *)&gen) == 1) {
            if (!found || gen > *out_gen) { *out_gen = gen; found = true; }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    /* POSIX: scandir */
    #include <dirent.h>
    DIR *d = opendir(dir);
    if (!d) return NX_OK;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        uint64_t gen = 0;
        if (sscanf(ent->d_name, "manifest-%llu.json", (unsigned long long *)&gen) == 1) {
            if (!found || gen > *out_gen) { *out_gen = gen; found = true; }
        }
    }
    closedir(d);
#endif
    (void)found;
    return NX_OK;
}

/* ---- WAL replay callback ------------------------------------------------ */
typedef struct replay_ctx {
    nx_buf *mutable_buf;
    uint32_t *mutable_docs;
} replay_ctx;

static nx_status replay_fn(nx_wal_op op, nx_slice payload, void *user_data) {
    replay_ctx *ctx = (replay_ctx *)user_data;
    if (op == NX_WAL_OP_UPSERT) {
        /* Append JSON line to mutable buffer */
        nx_buf_put(ctx->mutable_buf, payload.p, payload.n);
        /* Ensure newline separator */
        if (payload.n > 0 && ((const char *)payload.p)[payload.n - 1] != '\n') {
            nx_buf_put(ctx->mutable_buf, (const uint8_t *)"\n", 1);
        }
        if (ctx->mutable_buf->data) (*ctx->mutable_docs)++;
    }
    /* DELETE and CHECKPOINT: handle tombstones in future; for now skip */
    return NX_OK;
}

/* ---- nx_store_open ------------------------------------------------------ */
nx_status nx_store_open(const char *dir, nx_store *out, nx_error *error) {
    memset(out, 0, sizeof *out);
    nx_buf_init(&out->mutable_buf);

    /* Copy dir path */
    size_t dl = strlen(dir);
    if (dl >= NX_STORE_MAX_PATH) {
        if (error) nx_error_set(error, NX_ERR_INVALID, -1, 0, "store path too long");
        return NX_ERR_INVALID;
    }
    memcpy(out->dir, dir, dl + 1);

    /* Ensure directory exists */
    nx_status st = ensure_dir(dir);
    if (st != NX_OK) {
        if (error) nx_error_set(error, st, -1, 0, "cannot create store dir: %s", dir);
        return st;
    }

    /* Find + load manifest */
    uint64_t latest_gen = 0;
    find_latest_manifest(dir, &latest_gen);

    if (latest_gen > 0) {
        /* Load manifest */
        char mpath[NX_STORE_MAX_PATH];
        char mname[64];
        snprintf(mname, sizeof mname, "manifest-%llu.json",
                 (unsigned long long)latest_gen);
        path_join(mpath, sizeof mpath, dir, mname);

        uint32_t count = 0;
        st = read_manifest(mpath, error,
                           out->segs, NX_STORE_MAX_SEGMENTS, &count,
                           &out->manifest_gen, &out->next_seg_id, dir);
        if (st != NX_OK) return st;
        out->seg_count = count;
        if (out->next_seg_id == 0) out->next_seg_id = 1;

        /* Open each segment file */
        for (uint32_t i = 0; i < out->seg_count; i++) {
            uint8_t *seg_data = NULL;
            size_t   seg_size = 0;
            /* Allow up to 1 GiB per segment */
            nx_status load_st = nx_file_read(out->segs[i].file,
                                              1024 * 1024 * 1024,
                                              &seg_data, &seg_size);
            if (load_st != NX_OK) {
                /* Segment file missing/corrupt — skip and continue */
                continue;
            }
            out->seg_mem[i]      = seg_data;
            out->seg_mem_size[i] = seg_size;

            nx_slice seg_bytes = nx_slice_make(seg_data, seg_size);
            nx_error seg_err;
            memset(&seg_err, 0, sizeof seg_err);
            load_st = nx_segment_open(seg_bytes, &out->segs[i].seg, &seg_err);
            if (load_st == NX_OK) {
                out->segs[i].open = true;
            } else {
                /* Corrupt segment: log and skip */
                nx_free(seg_data);
                out->seg_mem[i]      = NULL;
                out->seg_mem_size[i] = 0;
            }
        }
    } else {
        /* Fresh store */
        out->manifest_gen = 0;
        out->next_seg_id  = 1;
    }

    /* Open / create WAL */
    char wal_path[NX_STORE_MAX_PATH];
    path_join(wal_path, sizeof wal_path, dir, "wal.log");
    st = nx_wal_open(wal_path, &out->wal);
    if (st != NX_OK) {
        if (error) nx_error_set(error, st, -1, 0, "cannot open WAL: %s", wal_path);
        nx_store_close(out);
        return st;
    }
    out->wal_open = true;

    /* Replay WAL into mutable buffer */
    replay_ctx ctx = { &out->mutable_buf, &out->mutable_docs };
    size_t replayed = 0;
    nx_wal_replay(wal_path, replay_fn, &ctx, &replayed);

    return NX_OK;
}

/* ---- nx_store_close ----------------------------------------------------- */
void nx_store_close(nx_store *store) {
    if (store->wal_open) {
        nx_wal_sync(&store->wal);
        nx_wal_close(&store->wal);
        store->wal_open = false;
    }
    nx_buf_free(&store->mutable_buf);
    for (uint32_t i = 0; i < NX_STORE_MAX_SEGMENTS; i++) {
        if (store->seg_mem[i]) {
            nx_free(store->seg_mem[i]);
            store->seg_mem[i]      = NULL;
            store->seg_mem_size[i] = 0;
        }
    }
    store->seg_count   = 0;
    store->mutable_docs = 0;
}

/* ---- nx_store_upsert ---------------------------------------------------- */
nx_status nx_store_upsert(nx_store *store, const char *doc_json, nx_error *error) {
    if (!store || !doc_json) return NX_ERR_INVALID;

    nx_slice payload = nx_slice_cstr(doc_json);

    /* Write to WAL first (durable) */
    nx_status st = nx_wal_append_upsert(&store->wal, payload);
    if (st != NX_OK) {
        if (error) nx_error_set(error, st, -1, 0, "WAL upsert failed");
        return st;
    }

    /* Append to mutable buffer */
    nx_buf_put(&store->mutable_buf, payload.p, payload.n);
    if (payload.n > 0 && ((const char *)payload.p)[payload.n - 1] != '\n') {
        nx_buf_put(&store->mutable_buf, (const uint8_t *)"\n", 1);
    }

    if (!store->mutable_buf.data) return NX_ERR_NOMEM;
    store->mutable_docs++;
    return NX_OK;
}

/* ---- nx_store_delete ---------------------------------------------------- */
nx_status nx_store_delete(nx_store *store, const char *id, nx_error *error) {
    if (!store || !id) return NX_ERR_INVALID;

    nx_slice payload = nx_slice_cstr(id);
    nx_status st = nx_wal_append_delete(&store->wal, payload);
    if (st != NX_OK) {
        if (error) nx_error_set(error, st, -1, 0, "WAL delete failed");
        return st;
    }
    /* Future: update per-segment tombstone bitmap. */
    return NX_OK;
}

/* ---- nx_store_flush ----------------------------------------------------- */
nx_status nx_store_flush(nx_store *store, nx_error *error) {
    if (!store) return NX_ERR_INVALID;
    if (store->mutable_docs == 0) return NX_OK;  /* nothing to flush */

    if (store->seg_count >= NX_STORE_MAX_SEGMENTS) {
        if (error) nx_error_set(error, NX_ERR_LIMIT, -1, 0, "segment count limit reached");
        return NX_ERR_LIMIT;
    }

    /* Build new segment from mutable buffer */
    uint64_t new_seg_id = store->next_seg_id;

    nx_seg_build_config bcfg = nx_seg_build_default_config();
    bcfg.seg_id = new_seg_id;

    nx_buf seg_buf;
    nx_buf_init(&seg_buf);

    nx_slice jsonl = nx_buf_slice(&store->mutable_buf);
    nx_status st = nx_seg_build(jsonl, &bcfg, &seg_buf, error);
    if (st != NX_OK) { nx_buf_free(&seg_buf); return st; }

    /* Write segment file atomically */
    char seg_name[64];
    snprintf(seg_name, sizeof seg_name, "seg-%llu.nxs", (unsigned long long)new_seg_id);
    char seg_path[NX_STORE_MAX_PATH];
    path_join(seg_path, sizeof seg_path, store->dir, seg_name);

    st = nx_file_write_atomic(seg_path, seg_buf.data, seg_buf.len);
    if (st != NX_OK) {
        nx_buf_free(&seg_buf);
        if (error) nx_error_set(error, st, -1, 0, "failed to write segment %s", seg_path);
        return st;
    }

    /* Register segment into store slot */
    uint32_t slot = store->seg_count;
    nx_store_seg_entry *entry = &store->segs[slot];
    memset(entry, 0, sizeof *entry);
    entry->seg_id    = new_seg_id;
    entry->doc_count = store->mutable_docs;
    {
        size_t plen = strlen(seg_path);
        size_t cap  = sizeof(entry->file) - 1;
        size_t cpn  = plen < cap ? plen : cap;
        memcpy(entry->file, seg_path, cpn);
        entry->file[cpn] = '\0';
    }

    /* Keep segment memory for zero-copy open */
    store->seg_mem[slot]      = (uint8_t *)seg_buf.data;
    store->seg_mem_size[slot] = seg_buf.len;
    seg_buf.data = NULL;  /* transfer ownership */
    seg_buf.len  = 0;
    seg_buf.cap  = 0;
    nx_buf_free(&seg_buf);

    nx_slice seg_bytes = nx_slice_make(store->seg_mem[slot], store->seg_mem_size[slot]);
    nx_error seg_err;
    memset(&seg_err, 0, sizeof seg_err);
    st = nx_segment_open(seg_bytes, &entry->seg, &seg_err);
    if (st != NX_OK) {
        /* Segment write succeeded but open failed — unlikely; continue */
        nx_free(store->seg_mem[slot]);
        store->seg_mem[slot] = NULL;
    } else {
        entry->open = true;
    }

    store->seg_count++;
    store->next_seg_id++;

    /* Checkpoint WAL (marks all prior mutations as durable in segment) */
    nx_wal_append_checkpoint(&store->wal, new_seg_id);
    nx_wal_sync(&store->wal);

    /* Write new manifest */
    uint64_t new_gen = store->manifest_gen + 1;
    st = write_manifest(store, new_gen, error);
    if (st != NX_OK) return st;
    store->manifest_gen = new_gen;

    /* Truncate WAL (mutations are now in the segment) */
    nx_wal_truncate(&store->wal);

    /* Reset mutable buffer */
    nx_buf_clear(&store->mutable_buf);
    store->mutable_docs = 0;

    return NX_OK;
}

/* ---- nx_store_doc_count ------------------------------------------------- */
uint64_t nx_store_doc_count(const nx_store *store) {
    uint64_t total = (uint64_t)store->mutable_docs;
    for (uint32_t i = 0; i < store->seg_count; i++) {
        total += (uint64_t)store->segs[i].doc_count;
    }
    return total;
}

/* ---- nx_store_seg_count ------------------------------------------------- */
uint32_t nx_store_seg_count(const nx_store *store) {
    return store->seg_count;
}

/* ---- nx_store_seg ------------------------------------------------------- */
const nx_segment *nx_store_seg(const nx_store *store, uint32_t slot) {
    if (slot >= store->seg_count) return NULL;
    if (!store->segs[slot].open) return NULL;
    return &store->segs[slot].seg;
}
