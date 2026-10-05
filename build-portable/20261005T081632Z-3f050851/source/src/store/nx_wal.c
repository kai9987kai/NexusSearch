#include "store/nx_wal.h"
#include "core/nx_mem.h"
#include "core/nx_crc32c.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <io.h>
#define nx_fileno _fileno
#define nx_fsync  _commit
#else
#include <unistd.h>
#define nx_fileno fileno
#define nx_fsync  fsync
#endif

#define WAL_HEADER_SIZE 32u
#define FRAME_HEADER_SIZE 16u

static nx_status write_wal_header(FILE *fp, uint64_t seq) {
    uint8_t hdr[WAL_HEADER_SIZE];
    memset(hdr, 0, WAL_HEADER_SIZE);
    memcpy(hdr, NX_WAL_MAGIC, 8);
    uint32_t ver = 1;
    memcpy(hdr + 8, &ver, 4);
    uint32_t flags = 0;
    memcpy(hdr + 12, &flags, 4);
    memcpy(hdr + 16, &seq, 8);

    uint32_t crc = nx_crc32c(0, hdr, 28);
    memcpy(hdr + 28, &crc, 4);

    if (fwrite(hdr, 1, WAL_HEADER_SIZE, fp) != WAL_HEADER_SIZE) {
        return NX_ERR_IO;
    }
    fflush(fp);
    return NX_OK;
}

nx_status nx_wal_open(const char *path, nx_wal *out) {
    if (!path || !out) return NX_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    size_t path_len = strlen(path);
    if (path_len >= sizeof(out->path)) return NX_ERR_LIMIT;
    memcpy(out->path, path, path_len + 1);

    FILE *fp = fopen(path, "r+b");
    if (!fp) {
        /* Create new WAL file */
        fp = fopen(path, "w+b");
        if (!fp) return NX_ERR_IO;
        nx_status st = write_wal_header(fp, 0);
        if (st != NX_OK) {
            fclose(fp);
            return st;
        }
        out->fp = fp;
        out->seq = 0;
        out->file_size = WAL_HEADER_SIZE;
        return NX_OK;
    }

    /* Validate existing WAL header */
    uint8_t hdr[WAL_HEADER_SIZE];
    if (fread(hdr, 1, WAL_HEADER_SIZE, fp) != WAL_HEADER_SIZE) {
        fclose(fp);
        return NX_ERR_CORRUPT;
    }

    if (memcmp(hdr, NX_WAL_MAGIC, 8) != 0) {
        fclose(fp);
        return NX_ERR_CORRUPT;
    }

    uint32_t ver = 0, stored_crc = 0;
    uint64_t seq = 0;
    memcpy(&ver, hdr + 8, 4);
    memcpy(&seq, hdr + 16, 8);
    memcpy(&stored_crc, hdr + 28, 4);

    if (ver != 1) {
        fclose(fp);
        return NX_ERR_UNSUPPORTED;
    }

    uint32_t computed_crc = nx_crc32c(0, hdr, 28);
    if (computed_crc != stored_crc) {
        fclose(fp);
        return NX_ERR_CORRUPT;
    }

    /* Position at end of file for subsequent appends */
    fseek(fp, 0, SEEK_END);
    long end_pos = ftell(fp);
    if (end_pos < (long)WAL_HEADER_SIZE) {
        fclose(fp);
        return NX_ERR_CORRUPT;
    }

    out->fp = fp;
    out->seq = seq;
    out->file_size = (size_t)end_pos;
    return NX_OK;
}

void nx_wal_close(nx_wal *wal) {
    if (wal && wal->fp) {
        fflush(wal->fp);
        fclose(wal->fp);
        wal->fp = NULL;
    }
}

static nx_status append_frame(nx_wal *wal, nx_wal_op op, nx_slice payload) {
    if (!wal || !wal->fp) return NX_ERR_INVALID;
    if (payload.n > 64u * 1024u * 1024u) return NX_ERR_LIMIT;

    uint8_t frame[FRAME_HEADER_SIZE];
    uint32_t magic = NX_WAL_FRAME_MAGIC;
    memcpy(frame, &magic, 4);
    frame[4] = (uint8_t)op;
    frame[5] = 0; /* flags */
    frame[6] = 0; frame[7] = 0; /* reserved */
    uint32_t plen = (uint32_t)payload.n;
    memcpy(frame + 8, &plen, 4);

    uint32_t crc = nx_crc32c(0, frame, 12);
    if (payload.p && payload.n > 0) {
        crc = nx_crc32c(crc, payload.p, payload.n);
    }
    memcpy(frame + 12, &crc, 4);

    if (fwrite(frame, 1, FRAME_HEADER_SIZE, wal->fp) != FRAME_HEADER_SIZE) {
        return NX_ERR_IO;
    }
    if (payload.n > 0 && payload.p) {
        if (fwrite(payload.p, 1, payload.n, wal->fp) != payload.n) {
            return NX_ERR_IO;
        }
    }

    wal->seq++;
    wal->file_size += FRAME_HEADER_SIZE + payload.n;
    return NX_OK;
}

nx_status nx_wal_append_upsert(nx_wal *wal, nx_slice doc_json) {
    return append_frame(wal, NX_WAL_OP_UPSERT, doc_json);
}

nx_status nx_wal_append_delete(nx_wal *wal, nx_slice id) {
    return append_frame(wal, NX_WAL_OP_DELETE, id);
}

nx_status nx_wal_append_checkpoint(nx_wal *wal, uint64_t seq) {
    uint8_t seq_bytes[8];
    memcpy(seq_bytes, &seq, 8);
    return append_frame(wal, NX_WAL_OP_CHECKPOINT, nx_slice_make(seq_bytes, 8));
}

nx_status nx_wal_sync(nx_wal *wal) {
    if (!wal || !wal->fp) return NX_ERR_INVALID;
    if (fflush(wal->fp) != 0) return NX_ERR_IO;
    int fd = nx_fileno(wal->fp);
    if (fd < 0) return NX_ERR_IO;
    if (nx_fsync(fd) != 0) return NX_ERR_IO;
    return NX_OK;
}

nx_status nx_wal_replay(const char *path, nx_wal_replay_fn fn, void *user_data, size_t *out_records) {
    if (!path || !fn || !out_records) return NX_ERR_INVALID;
    *out_records = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) return NX_ERR_NOT_FOUND;

    /* Verify WAL header */
    uint8_t hdr[WAL_HEADER_SIZE];
    if (fread(hdr, 1, WAL_HEADER_SIZE, fp) != WAL_HEADER_SIZE) {
        fclose(fp);
        return NX_ERR_CORRUPT;
    }

    if (memcmp(hdr, NX_WAL_MAGIC, 8) != 0) {
        fclose(fp);
        return NX_ERR_CORRUPT;
    }

    uint32_t stored_crc = 0;
    memcpy(&stored_crc, hdr + 28, 4);
    if (nx_crc32c(0, hdr, 28) != stored_crc) {
        fclose(fp);
        return NX_ERR_CORRUPT;
    }

    /* Iterate through frames */
    uint8_t frame[FRAME_HEADER_SIZE];
    size_t count = 0;

    while (1) {
        size_t rd = fread(frame, 1, FRAME_HEADER_SIZE, fp);
        if (rd == 0) break; /* Clean EOF */
        if (rd < FRAME_HEADER_SIZE) {
            /* Partial frame at EOF (torn write) -> stop recovery gracefully */
            break;
        }

        uint32_t magic = 0;
        memcpy(&magic, frame, 4);
        if (magic != NX_WAL_FRAME_MAGIC) {
            /* Invalid frame magic -> stop recovery */
            break;
        }

        uint8_t op_byte = frame[4];
        if (op_byte < NX_WAL_OP_UPSERT || op_byte > NX_WAL_OP_CHECKPOINT) {
            break;
        }

        uint32_t plen = 0, f_crc = 0;
        memcpy(&plen, frame + 8, 4);
        memcpy(&f_crc, frame + 12, 4);

        if (plen > 64u * 1024u * 1024u) break;

        uint8_t *payload_buf = NULL;
        if (plen > 0) {
            payload_buf = (uint8_t *)nx_malloc(plen);
            if (!payload_buf) {
                fclose(fp);
                return NX_ERR_NOMEM;
            }
            if (fread(payload_buf, 1, plen, fp) != plen) {
                nx_free(payload_buf);
                /* Incomplete payload (torn write) -> stop gracefully */
                break;
            }
        }

        /* Check CRC32C */
        uint32_t comp_crc = nx_crc32c(0, frame, 12);
        if (plen > 0) comp_crc = nx_crc32c(comp_crc, payload_buf, plen);

        if (comp_crc != f_crc) {
            nx_free(payload_buf);
            /* Corrupted payload at EOF -> stop gracefully */
            break;
        }

        nx_slice s = nx_slice_make(payload_buf, plen);
        nx_status st = fn((nx_wal_op)op_byte, s, user_data);
        nx_free(payload_buf);

        if (st != NX_OK) {
            fclose(fp);
            return st;
        }
        count++;
    }

    fclose(fp);
    *out_records = count;
    return NX_OK;
}

nx_status nx_wal_truncate(nx_wal *wal) {
    if (!wal) return NX_ERR_INVALID;
    if (wal->fp) {
        fclose(wal->fp);
        wal->fp = NULL;
    }

    FILE *fp = fopen(wal->path, "wb+");
    if (!fp) return NX_ERR_IO;

    nx_status st = write_wal_header(fp, wal->seq);
    if (st != NX_OK) {
        fclose(fp);
        return st;
    }

    wal->fp = fp;
    wal->file_size = WAL_HEADER_SIZE;
    return NX_OK;
}
