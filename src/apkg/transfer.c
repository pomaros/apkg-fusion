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
    apkg_status claim = ak_store_claim(ctx);
    if (claim)
        return claim;
    if (ak_store_path(ctx, "archives", out))
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

/* ---- restoring objects a root already approved ------------------------ */

static int digest_order(const void *a, const void *b) { return strcmp(a, b); }
static apkg_status channel_document(apkg_ctx *ctx, const char *endpoint,
                                    const apkg_observer *observer,
                                    unsigned char **document, size_t *size) {
    char path[AK_PATH], member[128];
    unsigned char *reference = NULL;
    size_t rn = 0;
    ak_import_progress progress = {0};
    apkg_status code;
    progress.observer = observer;
    progress.unknown_total = 1;
    progress.total = (uint64_t)ctx->object_max + 130;
    if (observer && pkg_fs_random(progress.operation_id.bytes, 16))
        return APKG_IO;
    if (ak_http_url(endpoint)) {
        code = ak_http_path(endpoint, "current", path);
        if (!code)
            code = ak_http_get(path, 65, -1, &reference, &rn, &progress);
    } else
        code = ak_path(path, endpoint, "current")
                   ? APKG_LIMIT
                   : ak_read(path, &reference, &rn, 65) ? APKG_NOT_FOUND
                                                         : APKG_OK;
    if (!code && (rn != 65 || reference[64] != '\n'))
        code = APKG_INVALID;
    if (!code) {
        unsigned char digest[32];
        reference[64] = 0;
        if (ak_hex((char *)reference, digest, 32))
            code = APKG_INVALID;
    }
    if (!code) {
        snprintf(member, sizeof member, "snapshots/%s/channel.json",
                 (char *)reference);
        if (ak_http_url(endpoint)) {
            code = ak_http_path(endpoint, member, path);
            if (!code)
                code = ak_http_get(path, ctx->object_max, -1, document, size,
                                   &progress);
        } else if (ak_path(path, endpoint, member))
            code = APKG_LIMIT;
        else if (ak_read(path, document, size, ctx->object_max))
            code = errno == ENOMEM ? APKG_NOMEM : APKG_NOT_FOUND;
    }
    free(reference);
    return code;
}
/* Make every object of one approved descriptor present and whole again: a
 * corrupt object is dropped, inline bytes are written back from the
 * descriptor itself, and the rest comes from the archive the channel at
 * `endpoint` offers for exactly this descriptor. The channel is only a place
 * to find bytes: what is admitted is checked against the digests the root
 * approved, so this works on a pending root, where nothing new may be
 * planned. Without an endpoint only the first two happen. */
apkg_status ak_objects_restore(apkg_ctx *ctx, const ak_json *j, int descriptor,
                               const char *endpoint,
                               const apkg_observer *observer, apkg_error **e) {
    int files = ak_member(j, ak_member(j, descriptor, "body"), "files");
    char (*wanted)[65] = NULL, path[AK_PATH], archive[AK_PATH], temp[AK_PATH] = "";
    size_t count = 0, capacity = 0, restored = 0, dn = 0;
    unsigned char *document = NULL;
    ak_json channel = {0};
    apkg_status code = APKG_OK;
    if (ctx->mode != APKG_WRITE)
        return APKG_POLICY;
    if (files < 0)
        return APKG_INVALID;
    for (size_t f = (size_t)files + 1; f < j->tokens[files].next && !code;
         f = j->tokens[f].next) {
        char sha[65];
        uint64_t size;
        apkg_status held;
        if (!ak_equal(j, ak_member(j, (int)f, "type"), "file"))
            continue;
        if (ak_cancelled(observer)) {
            code = APKG_CANCELLED;
            break;
        }
        if (ak_string(j, ak_member(j, (int)f, "sha256"), sha, sizeof sha) ||
            ak_u64(j, ak_member(j, (int)f, "size"), &size)) {
            code = APKG_INVALID;
            break;
        }
        held = ak_blob_check(ctx, sha, size, NULL);
        if (held == APKG_OK)
            continue;
        if (held == APKG_INTEGRITY) {
            /* Ours, by its name in our store, and not what it claims. */
            if (ak_blob_path(ctx, sha, path) || unlink(path)) {
                code = APKG_IO;
                break;
            }
        } else if (held != APKG_NOT_FOUND) {
            code = held;
            break;
        }
        if (ak_member(j, (int)f, "data") >= 0) {
            code = ak_blob_inline(ctx, j, (int)f);
            continue;
        }
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2 : 64;
            char (*next)[65] = calloc(grown, 65);
            if (!next) {
                code = APKG_NOMEM;
                break;
            }
            if (count)
                memcpy(next, wanted, count * 65);
            free(wanted);
            wanted = next;
            capacity = grown;
        }
        strcpy(wanted[count++], sha);
    }
    if (code || !count)
        goto done;
    if (!endpoint) {
        code = ak_fail(e, APKG_NOT_FOUND,
                       "the store lacks objects of this package and no "
                       "channel was given to fetch them from",
                       wanted[0]);
        goto done;
    }
    qsort(wanted, count, 65, digest_order);
    code = channel_document(ctx, endpoint, observer, &document, &dn);
    if (!code && ak_json_parse(document, dn, &channel))
        code = APKG_INVALID;
    if (!code) {
        int entries = ak_member(&channel, ak_member(&channel, 0, "body"),
                                "entries"),
            found = -1;
        size_t length = j->tokens[descriptor].end - j->tokens[descriptor].start;
        if (entries >= 0 && channel.tokens[entries].type == '[')
            for (size_t i = (size_t)entries + 1;
                 i < channel.tokens[entries].next; i = channel.tokens[i].next) {
                int d = ak_member(&channel, (int)i, "descriptor");
                if (d >= 0 &&
                    channel.tokens[d].end - channel.tokens[d].start == length &&
                    !memcmp(channel.text + channel.tokens[d].start,
                            j->text + j->tokens[descriptor].start, length))
                    found = ak_member(&channel, (int)i, "archive");
            }
        char sha[65];
        uint64_t size = 0;
        unsigned char digest[32];
        if (found < 0)
            code = ak_fail(e, APKG_NOT_FOUND,
                           "the channel no longer offers the build whose "
                           "objects are missing",
                           endpoint);
        else if (ak_string(&channel, ak_member(&channel, found, "sha256"), sha,
                           sizeof sha) ||
                 ak_hex(sha, digest, 32) ||
                 ak_u64(&channel, ak_member(&channel, found, "size"), &size))
            code = APKG_INVALID;
        if (!code) {
            char leaf[128];
            snprintf(leaf, sizeof leaf, "archives/%s.pkg", sha);
            if (!ak_http_url(endpoint)) {
                if (ak_path(archive, endpoint, leaf))
                    code = APKG_LIMIT;
            } else {
                /* Fetched into the archive cache under a transfer name and
                 * removed again: only its objects are wanted. */
                unsigned char random[16];
                char id[33], directory[AK_PATH], url[AK_PATH];
                ak_import_progress progress = {0};
                int fd = -1;
                progress.observer = observer;
                progress.total = size;
                progress.subject = endpoint;
                progress.has_phase = 1;
                progress.phase = APKG_PHASE_FETCH;
                code = cache_dir(ctx, directory);
                if (!code && (pkg_fs_random(random, 16) ||
                              (observer &&
                               pkg_fs_random(progress.operation_id.bytes, 16))))
                    code = APKG_IO;
                if (!code) {
                    char name[64];
                    ak_tohex(random, 16, id);
                    snprintf(name, sizeof name, ".incoming.%s", id);
                    if (ak_path(temp, directory, name))
                        code = APKG_LIMIT;
                }
                if (!code)
                    code = ak_http_path(endpoint, leaf, url);
                if (!code &&
                    (fd = open(temp, O_CREAT | O_EXCL | O_WRONLY, 0600)) < 0)
                    code = APKG_IO;
                if (!code)
                    code = ak_http_get(url, size, fd, NULL, NULL, &progress);
                if (fd >= 0 && (fsync(fd) | close(fd)) && !code)
                    code = APKG_IO;
                strcpy(archive, temp);
            }
        }
        if (!code)
            code = ak_archive_restore(ctx, archive, (const char (*)[65])wanted,
                                      count, &restored);
        if (!code && restored != count)
            code = ak_fail(e, APKG_INTEGRITY,
                           "the channel's archive does not carry every "
                           "missing object",
                           endpoint);
    }
done:
    if (*temp)
        unlink(temp);
    free(document);
    free(wanted);
    ak_json_free(&channel);
    if (code && e && !*e)
        ak_fail(e, code, "the package's objects could not be restored", "");
    return code;
}
/* The objects the pending transaction of this root still has to place. */
apkg_status ak_pending_restore(apkg_ctx *ctx, const char *endpoint,
                               const apkg_observer *observer, apkg_error **e) {
    char txn[AK_PATH], path[AK_PATH], name[256];
    unsigned char *raw = NULL;
    size_t n = 0;
    ak_json plan = {0};
    int body, system, installed;
    apkg_status code = APKG_OK;
    if (snprintf(txn, sizeof txn, "%s.apkg-transaction", ctx->root) >=
            (int)sizeof txn ||
        ak_path(path, txn, "plan"))
        return APKG_LIMIT;
    if (ak_read(path, &raw, &n, ctx->object_max))
        return errno == ENOENT ? APKG_NOT_FOUND : APKG_IO;
    if (ak_json_parse(raw, n, &plan) || !ak_object(&plan, "plan")) {
        free(raw);
        ak_json_free(&plan);
        return APKG_INVALID;
    }
    body = ak_member(&plan, 0, "body");
    system = ak_member(&plan, body, "system");
    installed = ak_member(
        &plan, ak_member(&plan, ak_member(&plan, body, "new_state"), "body"),
        "installed");
    if (system >= 0 && installed >= 0)
        for (size_t r = (size_t)system + 1;
             r < plan.tokens[system].next && !code; r = plan.tokens[r].next) {
            if (ak_string(&plan, ak_member(&plan, (int)r, "name"), name,
                          sizeof name)) {
                code = APKG_INVALID;
                break;
            }
            for (size_t i = (size_t)installed + 1;
                 i < plan.tokens[installed].next && !code;
                 i = plan.tokens[i].next) {
                int d = ak_member(&plan, (int)i, "descriptor");
                if (ak_equal(&plan,
                             ak_member(&plan, ak_member(&plan, d, "body"),
                                       "name"),
                             name))
                    code = ak_objects_restore(ctx, &plan, d, endpoint, observer,
                                              e);
            }
        }
    free(raw);
    ak_json_free(&plan);
    return code;
}

