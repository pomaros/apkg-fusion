/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "pkg_fs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static apkg_status cache_dir(apkg_ctx *ctx, char out[AK_PATH]) {
    struct stat st;
    char parent[AK_PATH];
    if (!ak_physical(ctx->anchor, &ctx->binding))
        return APKG_STALE;
    if (snprintf(out, AK_PATH, "%s.apkg-archives", ctx->root) >= AK_PATH)
        return APKG_LIMIT;
    if (lstat(out, &st)) {
        if (errno != ENOENT)
            return APKG_IO;
        if (mkdir(out, 0700) && errno != EEXIST)
            return APKG_IO;
        if (lstat(out, &st))
            return APKG_IO;
        if (ak_parent(out, parent))
            return APKG_INVALID;
        if (ak_sync_dir(parent))
            return APKG_IO;
    }
    if (!S_ISDIR(st.st_mode) || st.st_mode & 0077)
        return APKG_UNSUPPORTED;
#ifndef __AROS__
    if (st.st_uid != geteuid())
        return APKG_UNSUPPORTED;
#endif
    return APKG_OK;
}
/* Verify what was read/copied in fixed chunks. A cached object is never
 * trusted by its filename and a failed read does not replace it. */
static apkg_status file_bytes_with(unsigned char *buffer, const char *path,
                                   int output, uint64_t expected,
                                   const unsigned char wanted[32],
                                   ak_import_progress *progress,
                                   const apkg_observer *observer) {
    int flags = O_RDONLY, fd = -1;
    struct stat before, after;
    unsigned char digest[32];
    struct pkg_sha256 hash;
    uint64_t total = 0;
    apkg_status code = APKG_OK;
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    if (pkg_fs_path_is_link(path) != 0)
        return APKG_INVALID;
    fd = open(path, flags);
    if (fd < 0)
        return errno == ENOENT ? APKG_NOT_FOUND : APKG_IO;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0) {
        code = APKG_IO;
        goto done;
    }
    if ((uint64_t)before.st_size != expected) {
        code = APKG_INTEGRITY;
        goto done;
    }
    pkg_sha256_init(&hash);
    for (;;) {
        if (ak_cancelled(observer)) {
            code = APKG_CANCELLED;
            goto done;
        }
        ssize_t count = read(fd, buffer, (size_t)65536);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            code = APKG_IO;
            goto done;
        }
        if (!count)
            break;
        if ((uint64_t)count > expected - total) {
            code = APKG_INTEGRITY;
            goto done;
        }
        total += (uint64_t)count;
        pkg_sha256_update(&hash, buffer, (size_t)count);
        if (output >= 0) {
            size_t offset = 0;
            while (offset < (size_t)count) {
                ssize_t n =
                    write(output, buffer + offset, (size_t)count - offset);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0) {
                    code = APKG_IO;
                    goto done;
                }
                offset += (size_t)n;
            }
        }
        if (progress) {
            code = ak_import_tick(progress, (uint64_t)count);
            if (code)
                goto done;
        }
    }
    if (fstat(fd, &after) || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino || before.st_size != after.st_size ||
        total != expected) {
        code = APKG_STALE;
        goto done;
    }
    pkg_sha256_final(&hash, digest);
    if (memcmp(digest, wanted, 32))
        code = APKG_INTEGRITY;
done:
    if (close(fd) && !code)
        code = APKG_IO;
    return code;
}
/* Heap buffer: AROS task stacks have no guard page. */
static apkg_status file_bytes(const char *path, int output, uint64_t expected,
                              const unsigned char wanted[32],
                              ak_import_progress *progress,
                              const apkg_observer *observer) {
    unsigned char *buffer = malloc(65536);
    apkg_status code;
    if (!buffer)
        return APKG_NOMEM;
    code = file_bytes_with(buffer, path, output, expected, wanted, progress,
                           observer);
    free(buffer);
    return code;
}
apkg_status apkg_channel_fetch(apkg_ctx *ctx, const apkg_channel *channel,
                               uint64_t index, const apkg_observer *observer,
                               apkg_package **out, apkg_error **error) {
    const apkg_channel_entry_view *row = NULL;
    const char *endpoint = NULL;
    int remote = 0, fd = -1, published = 0;
    char directory[AK_PATH], archive[AK_PATH], temp[AK_PATH] = "", leaf[128],
                                               source[AK_PATH], sha[65];
    apkg_package *selected = NULL, *actual = NULL, *rechecked = NULL;
    apkg_status code;
    ak_import_progress progress = {0};
    apkg_observer silent = {0};
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!ctx || !channel)
        return APKG_INVALID;
    if (ctx->mode != APKG_WRITE)
        return APKG_POLICY;
    if (ak_cancelled(observer))
        return APKG_CANCELLED;
    if (observer)
        silent.cancel = observer->cancel;
    code = apkg_channel_entry_at(channel, index, &row, error);
    if (code)
        goto done;
    code = ak_channel_source(channel, &endpoint, &remote);
    if (code)
        goto done;
    progress.observer = observer;
    progress.total = row->archive_bytes;
    progress.subject = endpoint;
    progress.has_phase = 1;
    progress.phase = APKG_PHASE_CHECK;
    if (observer && pkg_fs_random(progress.operation_id.bytes, 16)) {
        code = APKG_IO;
        goto done;
    }
    code = ak_import_tick(&progress, 0);
    if (code)
        goto done;
    code = ak_channel_select(ctx, channel, index, &silent, &selected, error);
    if (code)
        goto done;
    code = cache_dir(ctx, directory);
    if (code)
        goto done;
    ak_tohex(row->archive_digest.bytes, 32, sha);
    snprintf(leaf, sizeof leaf, "%s.pkg", sha);
    if (ak_path(archive, directory, leaf)) {
        code = APKG_LIMIT;
        goto done;
    }
    progress.phase = APKG_PHASE_FETCH;
    code = ak_import_tick(&progress, 0);
    if (code)
        goto done;
    int exists = ak_exists(archive);
    if (exists < 0) {
        code = APKG_IO;
        goto done;
    }
    if (exists) {
        code = file_bytes(archive, -1, row->archive_bytes,
                          row->archive_digest.bytes, &progress, observer);
        if (code)
            goto done;
    } else {
        unsigned char random[16];
        char id[33];
        if (pkg_fs_random(random, sizeof random)) {
            code = APKG_IO;
            goto done;
        }
        ak_tohex(random, 16, id);
        snprintf(leaf, sizeof leaf, ".incoming.%s", id);
        if (ak_path(temp, directory, leaf)) {
            code = APKG_LIMIT;
            goto done;
        }
        fd = open(temp, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd < 0) {
            code = APKG_IO;
            goto done;
        }
        snprintf(leaf, sizeof leaf, "archives/%s.pkg", sha);
        if (remote) {
            code = ak_http_path(endpoint, leaf, source);
            if (!code)
                code = ak_http_get(source, row->archive_bytes, fd, NULL, NULL,
                                   &progress);
        } else {
            if (ak_path(source, endpoint, leaf))
                code = APKG_LIMIT;
            else
                code =
                    file_bytes(source, fd, row->archive_bytes,
                               row->archive_digest.bytes, &progress, observer);
        }
        if (code)
            goto done;
        if (ak_fault(1101) || fsync(fd)) {
            code = APKG_IO;
            goto done;
        }
        if (close(fd)) {
            fd = -1;
            code = APKG_IO;
            goto done;
        }
        fd = -1;
        code = file_bytes(temp, -1, row->archive_bytes,
                          row->archive_digest.bytes, NULL, observer);
        if (code)
            goto done;
    }
    progress.has_phase = 1;
    progress.phase = APKG_PHASE_CHECK;
    code = ak_import_tick(&progress, 0);
    if (code)
        goto done;
    const char *input = exists ? archive : temp;
    apkg_text path = {input, strlen(input)};
    code = apkg_package_import(ctx, path, &silent, &actual, error);
    if (code)
        goto done;
    if (memcmp(actual->digest.bytes, row->descriptor_digest.bytes, 32) ||
        strcmp(actual->raw, selected->raw) ||
        strcmp(actual->signature, selected->signature)) {
        code = APKG_INTEGRITY;
        goto done;
    }
    code = file_bytes(input, -1, row->archive_bytes, row->archive_digest.bytes,
                      NULL, observer);
    if (code)
        goto done;
    code = ak_channel_select(ctx, channel, index, &silent, &rechecked, error);
    if (code)
        goto done;
    if (ak_cancelled(observer)) {
        code = APKG_CANCELLED;
        goto done;
    }
    if (selected->origin) {
        actual->origin = ak_copy(selected->origin, strlen(selected->origin));
        if (!actual->origin) {
            code = APKG_NOMEM;
            goto done;
        }
    }
    if (!exists) {
        if (ak_fault(1102)) {
            code = APKG_IO;
            goto done;
        }
        if (ak_publish(temp, archive)) {
            if (errno != EEXIST) {
                code = APKG_IO;
                goto done;
            }
            code = file_bytes(archive, -1, row->archive_bytes,
                              row->archive_digest.bytes, NULL, observer);
            if (code)
                goto done;
        } else
            published = 1;
    }
    if (ak_fault(1103) || ak_sync_dir(directory)) {
        code = APKG_IO;
        goto done;
    }
    *out = actual;
    actual = NULL;
    code = APKG_OK;
done:
    if (fd >= 0)
        close(fd);
    if (*temp && !published)
        unlink(temp);
    apkg_package_free(selected);
    apkg_package_free(actual);
    apkg_package_free(rechecked);
    if (code && error && !*error)
        ak_fail(error, code, "selected channel archive refused", "");
    return code;
}
