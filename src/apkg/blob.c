/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "pkg_application.h"
#include "pkg_fs.h"
#include "pkg_target.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define AK_CHUNK 65536u
static apkg_status io_status(void) {
    return errno == ENOMEM ? APKG_NOMEM : APKG_IO;
}
static int regular_open(const char *path) {
    int flags = O_RDONLY, fd;
    struct stat st;
    if (pkg_fs_path_is_link(path) != 0) {
        errno = ELOOP;
        return -1;
    }
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(path, flags);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}
apkg_status ak_stream_hash(const char *path, uint64_t *size, char hex[65],
                           unsigned char head[64], size_t *head_size) {
    unsigned char buffer[AK_CHUNK], digest[32];
    struct pkg_sha256 hash;
    uint64_t total = 0;
    int fd = regular_open(path);
    struct stat before, after;
    if (fd < 0)
        return errno == ENOENT ? APKG_NOT_FOUND : io_status();
    if (fstat(fd, &before) || before.st_size < 0) {
        close(fd);
        return APKG_IO;
    }
    pkg_sha256_init(&hash);
    *head_size = 0;
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            close(fd);
            return APKG_IO;
        }
        if (!n)
            break;
        if ((uint64_t)n > UINT64_MAX - total) {
            close(fd);
            return APKG_LIMIT;
        }
        if (*head_size < 64) {
            size_t take = 64 - *head_size;
            if (take > (size_t)n)
                take = (size_t)n;
            memcpy(head + *head_size, buffer, take);
            *head_size += take;
        }
        total += (uint64_t)n;
        pkg_sha256_update(&hash, buffer, (size_t)n);
    }
    if (fstat(fd, &after) || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino || before.st_size != after.st_size ||
        total != (uint64_t)after.st_size) {
        close(fd);
        return APKG_STALE;
    }
    if (close(fd))
        return APKG_IO;
    pkg_sha256_final(&hash, digest);
    ak_tohex(digest, 32, hex);
    *size = total;
    return APKG_OK;
}
static apkg_status store_dir(apkg_ctx *c, char *out, int create) {
    struct stat st;
    if (!ak_physical(c->anchor, &c->binding))
        return APKG_STALE;
    if (snprintf(out, AK_PATH, "%s.apkg-objects", c->root) >= AK_PATH)
        return APKG_LIMIT;
    if (lstat(out, &st)) {
        if (errno != ENOENT)
            return APKG_IO;
        if (!create)
            return APKG_NOT_FOUND;
        if (c->mode != APKG_WRITE)
            return APKG_POLICY;
        if (mkdir(out, 0700) && errno != EEXIST)
            return APKG_IO;
        if (lstat(out, &st))
            return APKG_IO;
    }
    if (!S_ISDIR(st.st_mode) || pkg_fs_path_is_link(out) != 0)
        return APKG_UNSUPPORTED;
    if (create) {
        char parent[AK_PATH], *slash;
        strcpy(parent, c->root);
        slash = strrchr(parent, '/');
        if (slash) {
            if (slash == parent)
                slash[1] = 0;
            else
                *slash = 0;
        } else {
            slash = strchr(parent, ':');
            if (!slash)
                return APKG_INVALID;
            slash[1] = 0;
        }
        if (ak_sync_dir(parent))
            return APKG_IO;
    }
    return APKG_OK;
}
apkg_status ak_blob_path(apkg_ctx *c, const char *sha, char *out) {
    char dir[AK_PATH];
    unsigned char digest[32];
    apkg_status code;
    if (!c || !sha || ak_hex(sha, digest, 32))
        return APKG_INVALID;
    code = store_dir(c, dir, 0);
    if (code)
        return code;
    return ak_path(out, dir, sha) ? APKG_LIMIT : APKG_OK;
}
apkg_status ak_blob_check(apkg_ctx *c, const char *sha, uint64_t size,
                          const char *target) {
    char path[AK_PATH], actual[65];
    unsigned char head[64];
    size_t hn = 0;
    uint64_t n;
    apkg_status code = ak_blob_path(c, sha, path);
    if (code)
        return code;
    code = ak_stream_hash(path, &n, actual, head, &hn);
    if (code)
        return code;
    if (n != size || strcmp(actual, sha))
        return APKG_INTEGRITY;
    const char *cpu = pkg_application_binary_arch(head, hn);
    return target && *cpu && !pkg_target_cpu_matches(target, cpu) ? APKG_TARGET
                                                                  : APKG_OK;
}
static apkg_status temp_open(apkg_ctx *c, char *dir, char *path, int *out) {
    unsigned char random[16];
    char id[33], leaf[64];
    apkg_status code = store_dir(c, dir, 1);
    if (code)
        return code;
    if (c->mode != APKG_WRITE)
        return APKG_POLICY;
    if (pkg_fs_random(random, sizeof random))
        return APKG_IO;
    ak_tohex(random, 16, id);
    snprintf(leaf, sizeof leaf, ".incoming.%s", id);
    if (ak_path(path, dir, leaf))
        return APKG_LIMIT;
    *out = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    return *out < 0 ? APKG_IO : APKG_OK;
}
static int write_all(int fd, const unsigned char *p, size_t size) {
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += (size_t)n;
        size -= (size_t)n;
    }
    return 0;
}
/* The caller has bounded the source stream to exactly size bytes. A failed
 * import leaves only verified immutable objects; its private temp is removed.
 */
apkg_status ak_blob_ingest(apkg_ctx *c, const char *sha, uint64_t size,
                           int input, ak_import_progress *progress) {
    char dir[AK_PATH], temp[AK_PATH], final[AK_PATH], actual[65];
    int output = -1;
    unsigned char buffer[AK_CHUNK], digest[32];
    uint64_t left = size;
    struct pkg_sha256 hash;
    apkg_status code;
    int retain = 0;
    unsigned char validate[32];
    if (!c || !sha || ak_hex(sha, validate, 32))
        return APKG_INVALID;
    code = temp_open(c, dir, temp, &output);
    if (code)
        return code;
    if (ak_fault(1001)) {
        code = APKG_IO;
        goto done;
    }
    pkg_sha256_init(&hash);
    while (left) {
        size_t take = left > sizeof buffer ? sizeof buffer : (size_t)left;
        ssize_t n = read(input, buffer, take);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            code = n < 0 ? APKG_IO : APKG_INTEGRITY;
            goto done;
        }
        pkg_sha256_update(&hash, buffer, (size_t)n);
        if (write_all(output, buffer, (size_t)n)) {
            code = APKG_IO;
            goto done;
        }
        left -= (uint64_t)n;
        code = ak_import_tick(progress, (uint64_t)n);
        if (code)
            goto done;
    }
    pkg_sha256_final(&hash, digest);
    ak_tohex(digest, 32, actual);
    if (strcmp(actual, sha)) {
        code = APKG_INTEGRITY;
        goto done;
    }
    if (fsync(output)) {
        code = APKG_IO;
        goto done;
    }
    if (ak_fault(1002)) {
        code = APKG_IO;
        goto done;
    }
    if (close(output)) {
        output = -1;
        code = APKG_IO;
        goto done;
    }
    output = -1;
    if (ak_path(final, dir, sha)) {
        code = APKG_LIMIT;
        goto done;
    }
    if (!ak_physical(c->anchor, &c->binding)) {
        code = APKG_STALE;
        goto done;
    }
    if (ak_publish(temp, final)) {
        /* Another context may have won; identical bytes are reusable, corrupt
         * or linked objects are never silently replaced. */
        code = ak_blob_check(c, sha, size, NULL);
        if (!code && ak_sync_dir(dir))
            code = APKG_IO;
        goto done;
    }
    retain = 1;
    code = ak_fault(1003) || ak_sync_dir(dir) ? APKG_IO : APKG_OK;
done:
    if (output >= 0)
        close(output);
    if (!retain)
        unlink(temp);
    return code;
}
apkg_status ak_blob_inline(apkg_ctx *c, const ak_json *j, int entry) {
    char sha[65];
    uint64_t size;
    int data = ak_member(j, entry, "data");
    apkg_status code;
    if (data < 0)
        return APKG_OK;
    if (ak_string(j, ak_member(j, entry, "sha256"), sha, sizeof sha) ||
        ak_u64(j, ak_member(j, entry, "size"), &size))
        return APKG_INVALID;
    code = ak_blob_check(c, sha, size, NULL);
    if (code == APKG_OK)
        return code;
    if (code != APKG_NOT_FOUND)
        return code;
    char dir[AK_PATH], temp[AK_PATH];
    int fd = -1;
    code = temp_open(c, dir, temp, &fd);
    if (code)
        return code;
    unsigned char buffer[AK_CHUNK];
    uint64_t offset = 0;
    while (offset < size) {
        size_t n = size - offset > sizeof buffer ? sizeof buffer
                                                 : (size_t)(size - offset);
        for (size_t x = 0; x < n; x++) {
            char hex[3] = {
                j->text[j->tokens[data].start + 1 + (size_t)(offset + x) * 2],
                j->text[j->tokens[data].start + 2 + (size_t)(offset + x) * 2],
                0};
            if (ak_hex(hex, &buffer[x], 1)) {
                code = APKG_INVALID;
                goto done;
            }
        }
        if (write_all(fd, buffer, n)) {
            code = APKG_IO;
            goto done;
        }
        offset += n;
    }
    if (close(fd)) {
        fd = -1;
        code = APKG_IO;
        goto done;
    }
    fd = -1;
    int input = regular_open(temp);
    if (input < 0) {
        code = APKG_IO;
        goto done;
    }
    code = ak_blob_ingest(c, sha, size, input, NULL);
    close(input);
done:
    if (fd >= 0)
        close(fd);
    unlink(temp);
    return code;
}
apkg_status ak_blob_copy(apkg_ctx *c, const char *sha, uint64_t size,
                         const char *fallback, const char *destination) {
    apkg_status code = ak_blob_check(c, sha, size, NULL);
    char path[AK_PATH];
    if (code == APKG_NOT_FOUND && fallback) {
        int input = regular_open(fallback);
        if (input < 0)
            return APKG_NOT_FOUND;
        code = ak_blob_ingest(c, sha, size, input, NULL);
        close(input);
    }
    if (code)
        return code;
    code = ak_blob_path(c, sha, path);
    if (code)
        return code;
    int input = regular_open(path), output = -1;
    unsigned char buffer[AK_CHUNK], digest[32];
    struct pkg_sha256 hash;
    char actual[65];
    uint64_t copied = 0;
    if (input < 0)
        return APKG_IO;
    output = open(destination, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (output < 0) {
        close(input);
        return APKG_IO;
    }
    pkg_sha256_init(&hash);
    for (;;) {
        ssize_t n = read(input, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            code = APKG_IO;
            break;
        }
        if (!n)
            break;
        if ((uint64_t)n > size - copied) {
            code = APKG_INTEGRITY;
            break;
        }
        if (write_all(output, buffer, (size_t)n)) {
            code = APKG_IO;
            break;
        }
        pkg_sha256_update(&hash, buffer, (size_t)n);
        copied += (uint64_t)n;
    }
    if (!code) {
        pkg_sha256_final(&hash, digest);
        ak_tohex(digest, 32, actual);
        if (copied != size || strcmp(actual, sha))
            code = APKG_INTEGRITY;
    }
    if (!code && fsync(output))
        code = APKG_IO;
    if (close(input) && !code)
        code = APKG_IO;
    if (close(output) && !code)
        code = APKG_IO;
    if (code)
        unlink(destination);
    return code;
}
apkg_status ak_package_payload_check(apkg_ctx *c, const apkg_package *p) {
    int arr = ak_member(&p->json, ak_member(&p->json, 0, "body"), "files");
    size_t i;
#ifdef __AROS__
    for (i = (size_t)arr + 1; i < p->json.tokens[arr].next;
         i = p->json.tokens[i].next)
        if (ak_member(&p->json, (int)i, "amiga") >= 0)
            return APKG_UNSUPPORTED;
#endif
    for (i = (size_t)arr + 1; i < p->json.tokens[arr].next;
         i = p->json.tokens[i].next)
        if (ak_member(&p->json, (int)i, "blob") >= 0) {
            char sha[65];
            uint64_t size;
            apkg_status code;
            if (ak_string(&p->json, ak_member(&p->json, (int)i, "blob"), sha,
                          sizeof sha) ||
                ak_u64(&p->json, ak_member(&p->json, (int)i, "size"), &size))
                return APKG_INVALID;
            code = ak_blob_check(c, sha, size, p->target);
            if (code)
                return code;
        }
    return APKG_OK;
}
apkg_status ak_blob_retain(apkg_ctx *c, const char *sha, uint64_t size,
                           const char *source) {
    apkg_status code = ak_blob_check(c, sha, size, NULL);
    if (code != APKG_NOT_FOUND)
        return code;
    int fd = regular_open(source);
    if (fd < 0)
        return APKG_IO;
    code = ak_blob_ingest(c, sha, size, fd, NULL);
    close(fd);
    return code;
}
apkg_status ak_import_tick(ak_import_progress *p, uint64_t added) {
    if (!p || !p->observer)
        return APKG_OK;
    if (ak_cancelled(p->observer))
        return APKG_CANCELLED;
    if (added > p->total - p->completed)
        return APKG_LIMIT;
    p->completed += added;
    long long now = pkg_fs_now_ms();
    if (p->sequence && p->completed != p->total &&
        p->completed - p->last_emitted < 4 * 1024 * 1024 &&
        now - p->last_ms < 100)
        return APKG_OK;
    apkg_event event = {0};
    event.operation_id = p->operation_id;
    event.sequence = ++p->sequence;
    event.phase = APKG_PHASE_FETCH;
    event.completed = p->completed;
    event.total.state = APKG_KNOWN;
    event.total.value = p->total;
    event.cancellable = 1;
    event.subject.data = p->subject;
    event.subject.size = strlen(p->subject);
    event.unit.data = "bytes";
    event.unit.size = 5;
    p->last_ms = now;
    p->last_emitted = p->completed;
    if (p->observer->event &&
        p->observer->event(p->observer->user, &event) == APKG_REQUEST_CANCEL) {
        if (p->observer->cancel)
            apkg_cancel_request(p->observer->cancel);
        return APKG_CANCELLED;
    }
    return ak_cancelled(p->observer) ? APKG_CANCELLED : APKG_OK;
}
