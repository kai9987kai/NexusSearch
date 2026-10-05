#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#else
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000002
#endif
#endif
#include "core/nx_file.h"
#include "core/nx_mem.h"
#include "core/nx_thread.h"
#include <stdio.h>
#ifdef NX_WINDOWS
#include <windows.h>
#include <stdatomic.h>

static nx_status win_error(void) {
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return NX_ERR_NOT_FOUND;
    if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) return NX_ERR_EXISTS;
    return NX_ERR_IO;
}
static nx_status wide_path(const char *path, wchar_t **out) {
    *out = NULL;
    if (!path || !*path) return NX_ERR_INVALID;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (n <= 0) return NX_ERR_INVALID;
    wchar_t *wide = NX_NEW_ARRAY(wchar_t, (size_t)n + 80);
    if (!wide) return NX_ERR_NOMEM;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n)) { nx_free(wide); return NX_ERR_INVALID; }
    *out = wide;
    return NX_OK;
}
nx_status nx_mmap_open(const char *path, size_t max_bytes, nx_mmap *out) {
    if (!out) return NX_ERR_INVALID;
    memset(out, 0, sizeof *out);
    wchar_t *wide = NULL; nx_status st = wide_path(path, &wide);
    if (st != NX_OK) return st;
    HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    nx_free(wide);
    if (file == INVALID_HANDLE_VALUE) return win_error();
    LARGE_INTEGER size; BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        GetFileType(file) != FILE_TYPE_DISK || !GetFileSizeEx(file, &size) || size.QuadPart < 0) st = NX_ERR_IO;
    else if ((uint64_t)size.QuadPart > max_bytes) st = NX_ERR_LIMIT;
    else if (size.QuadPart == 0) { CloseHandle(file); return NX_OK; }
    if (st != NX_OK) { CloseHandle(file); return st; }
    HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) { CloseHandle(file); return NX_ERR_IO; }
    const uint8_t *data = (const uint8_t *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!data) { CloseHandle(mapping); CloseHandle(file); return NX_ERR_IO; }
    out->file_handle = file; out->map_handle = mapping; out->size = (size_t)size.QuadPart; out->data = data;
    return NX_OK;
}
void nx_mmap_close(nx_mmap *map) {
    if (!map) return;
    if (map->data) UnmapViewOfFile(map->data);
    if (map->map_handle) CloseHandle(map->map_handle);
    if (map->file_handle) CloseHandle(map->file_handle);
    memset(map, 0, sizeof *map);
}
/* Only used to avoid temporary-name collisions; exclusivity is enforced by
 * CREATE_NEW, so the relaxed counter is not part of the correctness protocol. */
static atomic_uint_fast64_t temp_sequence;
static nx_status replace_open_file(HANDLE file, const wchar_t *path) {
    DWORD n = GetFullPathNameW(path, 0, NULL, NULL);
    if (!n || n > 32768) return NX_ERR_IO;
    size_t bytes = offsetof(FILE_RENAME_INFO, FileName) + (size_t)n * sizeof(wchar_t);
    FILE_RENAME_INFO *info = (FILE_RENAME_INFO *)nx_calloc(1, bytes);
    if (!info) return NX_ERR_NOMEM;
    DWORD len = GetFullPathNameW(path, n, info->FileName, NULL);
    nx_status st = NX_ERR_IO;
    if (len && len < n) {
        info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        info->FileNameLength = len * (DWORD)sizeof(wchar_t);
        if (SetFileInformationByHandle(file, FileRenameInfoEx, info, (DWORD)bytes)) st = NX_OK;
    }
    nx_free(info); return st;
}
nx_status nx_file_write_atomic(const char *path, const void *data, size_t size) {
    if (!data && size) return NX_ERR_INVALID;
    wchar_t *wide = NULL, *temp = NULL;
    nx_status st = wide_path(path, &wide);
    if (st != NX_OK) return st;
    st = wide_path(path, &temp);
    if (st != NX_OK) { nx_free(wide); return st; }
    size_t n = wcslen(temp);
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        uint64_t serial = atomic_fetch_add_explicit(&temp_sequence, 1, memory_order_relaxed);
        int written = swprintf(temp + n, 80, L".nx-%u-%llu-%llu.tmp", (unsigned)GetCurrentProcessId(),
                               (unsigned long long)nx_now_ns(), (unsigned long long)serial);
        if (written < 0) { st = NX_ERR_IO; break; }
        file = CreateFileW(temp, GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        st = win_error();
        if (st != NX_ERR_EXISTS) break;
    }
    if (file != INVALID_HANDLE_VALUE) {
        st = NX_OK;
        size_t pos = 0;
        while (pos < size) {
            DWORD amount = (DWORD)(size - pos > 1024u * 1024u ? 1024u * 1024u : size - pos), wrote = 0;
            if (!WriteFile(file, (const uint8_t *)data + pos, amount, &wrote, NULL) || wrote == 0) { st = NX_ERR_IO; break; }
            pos += wrote;
        }
        if (st == NX_OK && !FlushFileBuffers(file)) st = NX_ERR_IO;
        if (st == NX_OK) st = replace_open_file(file, wide);
        if (!CloseHandle(file)) st = NX_ERR_IO;
        if (st != NX_OK) DeleteFileW(temp);
    }
    nx_free(temp); nx_free(wide);
    return st;
}
nx_status nx_file_remove(const char *path) {
    wchar_t *wide = NULL; nx_status st = wide_path(path, &wide);
    if (st != NX_OK) return st;
    if (!DeleteFileW(wide)) st = win_error();
    nx_free(wide); return st;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
static nx_status posix_error(void) { return errno == ENOENT ? NX_ERR_NOT_FOUND : NX_ERR_IO; }
nx_status nx_mmap_open(const char *path, size_t max_bytes, nx_mmap *out) {
    if (!out) return NX_ERR_INVALID;
    memset(out, 0, sizeof *out);
    if (!path || !*path) return NX_ERR_INVALID;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return posix_error();
    struct stat info; nx_status st = NX_OK;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0) st = NX_ERR_IO;
    else if ((uint64_t)info.st_size > max_bytes) st = NX_ERR_LIMIT;
    else if (info.st_size) {
        void *p = mmap(NULL, (size_t)info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) st = NX_ERR_IO;
        else { out->data = (const uint8_t *)p; out->size = (size_t)info.st_size; }
    }
    close(fd); return st;
}
void nx_mmap_close(nx_mmap *map) {
    if (!map) return;
    if (map->data) munmap((void *)map->data, map->size);
    memset(map, 0, sizeof *map);
}
nx_status nx_file_write_atomic(const char *path, const void *data, size_t size) {
    if (!path || !*path || (!data && size)) return NX_ERR_INVALID;
    size_t len;
    if (nx_add_overflow(strlen(path), 16, &len)) return NX_ERR_LIMIT;
    char *temp = (char *)nx_malloc(len), *parent = nx_strdup(path);
    if (!temp || !parent) { nx_free(temp); nx_free(parent); return NX_ERR_NOMEM; }
    (void)snprintf(temp, len, "%s.nx-XXXXXX", path);
    char *slash = strrchr(parent, '/');
    if (slash) { if (slash == parent) slash[1] = 0; else *slash = 0; } else strcpy(parent, ".");
    int dir = open(parent, O_RDONLY); nx_free(parent);
    if (dir < 0) { nx_free(temp); return posix_error(); }
    int fd = mkstemp(temp);
    if (fd < 0) { close(dir); nx_free(temp); return posix_error(); }
    nx_status st = NX_OK; size_t pos = 0;
    while (pos < size) {
        size_t amount = size - pos > 1024u * 1024u ? 1024u * 1024u : size - pos;
        ssize_t wrote = write(fd, (const uint8_t *)data + pos, amount);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) { st = NX_ERR_IO; break; }
        pos += (size_t)wrote;
    }
    if (st == NX_OK && fsync(fd)) st = NX_ERR_IO;
    if (close(fd)) st = NX_ERR_IO;
    if (st == NX_OK && rename(temp, path)) st = NX_ERR_IO;
    if (st == NX_OK && fsync(dir)) st = NX_ERR_IO;
    if (st != NX_OK) unlink(temp);
    close(dir); nx_free(temp); return st;
}
nx_status nx_file_remove(const char *path) {
    if (!path || !*path) return NX_ERR_INVALID;
    return unlink(path) == 0 ? NX_OK : posix_error();
}
#endif

nx_status nx_file_read(const char *path, size_t max_bytes, uint8_t **data, size_t *size) {
    if (data) *data = NULL;
    if (size) *size = 0;
    if (!data || !size) return NX_ERR_INVALID;
    nx_mmap map; nx_status st = nx_mmap_open(path, max_bytes, &map);
    if (st != NX_OK) return st;
    if (map.size) {
        *data = (uint8_t *)nx_malloc(map.size);
        if (!*data) st = NX_ERR_NOMEM;
        else { memcpy(*data, map.data, map.size); *size = map.size; }
    }
    nx_mmap_close(&map); return st;
}
