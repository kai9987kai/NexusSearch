/* nx_merge.c - Tiered Segment Merge & Compaction Implementation.
 *
 * Implements segment consolidation with document deduplication (latest _id wins),
 * atomic manifest updates, and cleanup of superseded segment files.
 */
#include "store/nx_merge.h"
#include "store/nx_store.h"
#include "seg/nx_seg_builder.h"
#include "seg/nx_table.h"
#include "core/nx_mem.h"
#include "core/nx_buf.h"
#include "core/nx_file.h"
#include "core/nx_status.h"
#include <string.h>
#include <stdio.h>

#define SEEN_HASH_INIT_CAP 1024u

/* Hash entry for tracking seen _id slices */
typedef struct id_entry {
    nx_slice id;
    uint32_t hash;
    bool used;
} id_entry;

typedef struct id_set {
    id_entry *entries;
    size_t cap;
    size_t count;
} id_set;

/* 32-bit FNV-1a hash */
static inline uint32_t fnv1a_32(const uint8_t *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint32_t)data[i];
        h *= 16777619u;
    }
    return h;
}

static nx_status id_set_init(id_set *set, size_t initial_cap) {
    set->cap = initial_cap < SEEN_HASH_INIT_CAP ? SEEN_HASH_INIT_CAP : initial_cap;
    set->count = 0;
    set->entries = NX_NEW_ARRAY(id_entry, set->cap);
    return set->entries ? NX_OK : NX_ERR_NOMEM;
}

static void id_set_free(id_set *set) {
    nx_free(set->entries);
    set->entries = NULL;
    set->cap = 0;
    set->count = 0;
}

static nx_status id_set_grow(id_set *set);

/* Returns true if id was already seen; if not seen, inserts and returns false */
static bool id_set_test_and_add(id_set *set, nx_slice id) {
    if (set->count * 2 >= set->cap) {
        if (id_set_grow(set) != NX_OK) {
            /* Growth failed, continue probing current table */
        }
    }

    uint32_t h = fnv1a_32(id.p, id.n);
    size_t mask = set->cap - 1u;
    size_t idx = (size_t)h & mask;

    while (set->entries[idx].used) {
        if (set->entries[idx].hash == h &&
            set->entries[idx].id.n == id.n &&
            memcmp(set->entries[idx].id.p, id.p, id.n) == 0) {
            return true; /* Already seen */
        }
        idx = (idx + 1u) & mask;
    }

    /* Not seen: insert */
    set->entries[idx].used = true;
    set->entries[idx].hash = h;
    set->entries[idx].id = id;
    set->count++;
    return false;
}

static nx_status id_set_grow(id_set *set) {
    size_t new_cap = set->cap * 2u;
    id_entry *new_entries = NX_NEW_ARRAY(id_entry, new_cap);
    if (!new_entries) return NX_ERR_NOMEM;

    size_t mask = new_cap - 1u;
    for (size_t i = 0; i < set->cap; i++) {
        if (set->entries[i].used) {
            size_t idx = (size_t)set->entries[i].hash & mask;
            while (new_entries[idx].used) {
                idx = (idx + 1u) & mask;
            }
            new_entries[idx] = set->entries[i];
        }
    }

    nx_free(set->entries);
    set->entries = new_entries;
    set->cap = new_cap;
    return NX_OK;
}

/* Helper to write manifest file */
static nx_status write_new_manifest(const nx_store *store, uint64_t gen,
                                    const nx_store_seg_entry *segs, uint32_t seg_count,
                                    nx_error *error) {
    nx_buf b;
    nx_buf_init(&b);

    nx_buf_printf(&b, "{\"gen\":%llu,\"next_seg_id\":%llu,\"segments\":[",
                  (unsigned long long)gen,
                  (unsigned long long)store->next_seg_id);

    for (uint32_t i = 0; i < seg_count; i++) {
        const nx_store_seg_entry *s = &segs[i];
        if (i > 0) nx_buf_printf(&b, ",");
        const char *base = strrchr(s->file, '/');
        base = base ? base + 1 : s->file;
        nx_buf_printf(&b, "{\"id\":%llu,\"file\":\"%s\",\"docs\":%u}",
                      (unsigned long long)s->seg_id, base,
                      (unsigned)s->doc_count);
    }
    nx_buf_printf(&b, "]}");

    if (!b.data) { nx_buf_free(&b); return NX_ERR_NOMEM; }

    char mpath[NX_STORE_MAX_PATH];
    char mname[64];
    snprintf(mname, sizeof(mname), "manifest-%llu.json", (unsigned long long)gen);

    size_t dl = strlen(store->dir);
    size_t nl = strlen(mname);
    if (dl + nl + 2 < sizeof(mpath)) {
        memcpy(mpath, store->dir, dl);
        mpath[dl] = '/';
        memcpy(mpath + dl + 1, mname, nl);
        mpath[dl + 1 + nl] = '\0';
    } else {
        memcpy(mpath, store->dir, dl);
        mpath[dl] = '\0';
    }

    nx_status st = nx_file_write_atomic(mpath, b.data, b.len);
    nx_buf_free(&b);
    if (st != NX_OK && error) {
        nx_error_set(error, st, -1, 0, "failed to write merged manifest %s", mpath);
    }
    return st;
}

nx_status nx_store_compact(nx_store *store, const nx_merge_policy *policy,
                           nx_merge_result *out_res, nx_error *error) {
    if (!store) return NX_ERR_INVALID;
    if (out_res) memset(out_res, 0, sizeof(*out_res));

    nx_merge_policy pol = policy ? *policy : nx_merge_default_policy();
    if (pol.min_segments_to_merge < 2) pol.min_segments_to_merge = 2;
    if (pol.max_segments_per_merge < 2) pol.max_segments_per_merge = 2;

    if (store->seg_count < pol.min_segments_to_merge) {
        /* Not enough segments to merge */
        return NX_OK;
    }

    /* Select candidates to merge (from oldest segment up to max_segments_per_merge) */
    uint32_t num_to_merge = store->seg_count < pol.max_segments_per_merge
                                ? store->seg_count
                                : pol.max_segments_per_merge;

    /* Check doc limit */
    uint32_t total_docs = 0;
    uint32_t actual_merge_count = 0;
    for (uint32_t i = 0; i < num_to_merge; i++) {
        if (total_docs + store->segs[i].doc_count > pol.max_total_docs_per_merge && actual_merge_count >= 2) {
            break;
        }
        total_docs += store->segs[i].doc_count;
        actual_merge_count++;
    }

    if (actual_merge_count < pol.min_segments_to_merge) {
        return NX_OK;
    }

    /* Deduplicate documents across candidate segments:
     * Iterate from newest candidate (actual_merge_count - 1) down to oldest (0).
     * If an _id was already seen in a newer segment, it is superseded. */
    id_set seen;
    if (id_set_init(&seen, total_docs > 0 ? total_docs * 2 : 1024) != NX_OK) {
        return NX_ERR_NOMEM;
    }

    /* Collect surviving documents in chronological order */
    /* Store pointers to document slices */
    nx_buf doc_jsonl;
    nx_buf_init(&doc_jsonl);

    uint32_t surviving_docs = 0;

    /* Scan backwards to identify surviving documents per segment */
    /* Allocate boolean flags per document */
    uint8_t **keep_flags = NX_NEW_ARRAY(uint8_t *, actual_merge_count);
    if (!keep_flags) {
        id_set_free(&seen);
        nx_buf_free(&doc_jsonl);
        return NX_ERR_NOMEM;
    }

    for (uint32_t s = 0; s < actual_merge_count; s++) {
        uint32_t dc = store->segs[s].doc_count;
        keep_flags[s] = dc > 0 ? NX_NEW_ARRAY(uint8_t, dc) : NULL;
    }

    /* Mark surviving documents from newest to oldest */
    for (int32_t s = (int32_t)actual_merge_count - 1; s >= 0; s--) {
        const nx_segment *seg = &store->segs[s].seg;
        nx_table table;
        memset(&table, 0, sizeof(table));
        if (nx_table_open(seg->table_bytes, &table, error) != NX_OK) {
            continue;
        }

        for (int32_t r = (int32_t)table.rows - 1; r >= 0; r--) {
            nx_slice id = nx_table_id(&table, (uint32_t)r);
            if (!id_set_test_and_add(&seen, id)) {
                if (keep_flags[s]) keep_flags[s][r] = 1;
                surviving_docs++;
            }
        }
    }
    id_set_free(&seen);

    /* Emit surviving documents in forward order into doc_jsonl */
    for (uint32_t s = 0; s < actual_merge_count; s++) {
        const nx_segment *seg = &store->segs[s].seg;
        nx_table table;
        memset(&table, 0, sizeof(table));
        if (nx_table_open(seg->table_bytes, &table, error) != NX_OK) {
            continue;
        }

        for (uint32_t r = 0; r < table.rows; r++) {
            if (keep_flags[s] && keep_flags[s][r]) {
                nx_slice doc_slice = nx_table_document(&table, r);
                nx_buf_put(&doc_jsonl, doc_slice.p, doc_slice.n);
                nx_buf_put_u8(&doc_jsonl, '\n');
            }
        }
        nx_free(keep_flags[s]);
    }
    nx_free(keep_flags);

    if (doc_jsonl.oom) {
        nx_buf_free(&doc_jsonl);
        return NX_ERR_NOMEM;
    }

    /* Build consolidated segment */
    uint64_t new_seg_id = store->next_seg_id;
    nx_seg_build_config bcfg = nx_seg_build_default_config();
    bcfg.seg_id = new_seg_id;

    nx_buf seg_buf;
    nx_buf_init(&seg_buf);
    nx_status st = nx_seg_build(nx_buf_slice(&doc_jsonl), &bcfg, &seg_buf, error);
    nx_buf_free(&doc_jsonl);

    if (st != NX_OK) {
        nx_buf_free(&seg_buf);
        return st;
    }

    /* Write consolidated segment to disk */
    char seg_name[64];
    snprintf(seg_name, sizeof(seg_name), "seg-%llu.nxs", (unsigned long long)new_seg_id);
    char seg_path[NX_STORE_MAX_PATH];
    size_t dl = strlen(store->dir);
    size_t nl = strlen(seg_name);
    if (dl + nl + 2 < sizeof(seg_path)) {
        memcpy(seg_path, store->dir, dl);
        seg_path[dl] = '/';
        memcpy(seg_path + dl + 1, seg_name, nl);
        seg_path[dl + 1 + nl] = '\0';
    } else {
        memcpy(seg_path, store->dir, dl);
        seg_path[dl] = '\0';
    }

    st = nx_file_write_atomic(seg_path, seg_buf.data, seg_buf.len);
    if (st != NX_OK) {
        nx_buf_free(&seg_buf);
        if (error) nx_error_set(error, st, -1, 0, "failed to write merged segment file");
        return st;
    }

    /* Assemble new segment entries list:
     * [0] = new consolidated segment
     * [1 ..] = unmerged segments from [actual_merge_count .. seg_count - 1] */
    uint32_t remaining = store->seg_count - actual_merge_count;
    uint32_t new_seg_count = 1u + remaining;

    nx_store_seg_entry new_segs[NX_STORE_MAX_SEGMENTS];
    memset(new_segs, 0, sizeof(new_segs));

    /* Slot 0 is the new merged segment */
    new_segs[0].seg_id = new_seg_id;
    new_segs[0].doc_count = surviving_docs;
    {
        size_t plen = strlen(seg_path);
        size_t cap = sizeof(new_segs[0].file) - 1;
        size_t cpn = plen < cap ? plen : cap;
        memcpy(new_segs[0].file, seg_path, cpn);
        new_segs[0].file[cpn] = '\0';
    }

    /* Open new segment reader */
    uint8_t *new_seg_mem = (uint8_t *)seg_buf.data;
    size_t new_seg_mem_sz = seg_buf.len;
    seg_buf.data = NULL;
    seg_buf.len = 0;
    seg_buf.cap = 0;
    nx_buf_free(&seg_buf);

    st = nx_segment_open(nx_slice_make(new_seg_mem, new_seg_mem_sz), &new_segs[0].seg, error);
    if (st == NX_OK) {
        new_segs[0].open = true;
    }

    /* Copy unmerged segments */
    for (uint32_t i = 0; i < remaining; i++) {
        new_segs[1 + i] = store->segs[actual_merge_count + i];
    }

    /* Write new manifest atomically */
    uint64_t new_gen = store->manifest_gen + 1u;
    store->next_seg_id++;
    st = write_new_manifest(store, new_gen, new_segs, new_seg_count, error);
    if (st != NX_OK) {
        /* Rollback */
        nx_free(new_seg_mem);
        nx_file_remove(seg_path);
        return st;
    }
    store->manifest_gen = new_gen;

    /* Delete old segment files on disk */
    for (uint32_t i = 0; i < actual_merge_count; i++) {
        nx_file_remove(store->segs[i].file);
        if (store->seg_mem[i]) {
            nx_free(store->seg_mem[i]);
            store->seg_mem[i] = NULL;
            store->seg_mem_size[i] = 0;
        }
    }

    /* Shift unmerged segment memories */
    uint8_t *temp_mem[NX_STORE_MAX_SEGMENTS] = {0};
    size_t temp_mem_sz[NX_STORE_MAX_SEGMENTS] = {0};
    temp_mem[0] = new_seg_mem;
    temp_mem_sz[0] = new_seg_mem_sz;

    for (uint32_t i = 0; i < remaining; i++) {
        temp_mem[1 + i] = store->seg_mem[actual_merge_count + i];
        temp_mem_sz[1 + i] = store->seg_mem_size[actual_merge_count + i];
    }

    memcpy(store->segs, new_segs, sizeof(new_segs));
    memcpy(store->seg_mem, temp_mem, sizeof(temp_mem));
    memcpy(store->seg_mem_size, temp_mem_sz, sizeof(temp_mem_sz));
    store->seg_count = new_seg_count;

    if (out_res) {
        out_res->segments_merged = actual_merge_count;
        out_res->docs_input = total_docs;
        out_res->docs_output = surviving_docs;
        out_res->new_seg_id = new_seg_id;
    }

    return NX_OK;
}
