/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * A verified local mirror of what one package's next update needs: the
 * channel snapshot exactly as served and the archive of the newest
 * acceptable build. A system updater applies at boot, before any network
 * exists; the running system mirrors beforehand and the updater reads the
 * mirror as an ordinary file channel. */
#define _POSIX_C_SOURCE 200809L
#include "apkg_client.h"
#include "internal.h"
#include "pkg_fs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHUNK 65536u

static int hex_name(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}
static int is_directory(const char *path) {
    struct stat st;
    return !lstat(path, &st) && S_ISDIR(st.st_mode);
}
static apkg_status directory(const char *path) {
    if (is_directory(path))
        return APKG_OK;
    if (mkdir(path, 0755) && !is_directory(path))
        return APKG_IO;
    return APKG_OK;
}
/* 0 when the file holds exactly these bytes. */
static int holds(const char *path, const unsigned char *bytes, size_t size) {
    unsigned char *have = NULL;
    size_t n = 0;
    int same;
    if (ak_read(path, &have, &n, size))
        return -1;
    same = n == size && !memcmp(have, bytes, size);
    free(have);
    return same ? 0 : -1;
}
/* Whole and flushed under a transfer name, then in place in one step. */
static apkg_status publish_bytes(const char *dir, const char *name,
                                 const unsigned char *bytes, size_t size) {
    char path[AK_PATH], temp[AK_PATH], leaf[64], id[33];
    unsigned char random[16];
    if (ak_path(path, dir, name))
        return APKG_LIMIT;
    if (!holds(path, bytes, size))
        return APKG_OK;
    if (pkg_fs_random(random, sizeof random))
        return APKG_IO;
    ak_tohex(random, 16, id);
    snprintf(leaf, sizeof leaf, ".incoming.%s", id);
    if (ak_path(temp, dir, leaf))
        return APKG_LIMIT;
    if (ak_write(temp, bytes, size, 1) || pkg_fs_replace(temp, path) ||
        ak_sync_dir(dir)) {
        unlink(temp);
        return APKG_IO;
    }
    return APKG_OK;
}
static apkg_status archive_holds(const char *path, const char *sha,
                                 uint64_t size) {
    char actual[65];
    unsigned char head[64];
    size_t head_size = 0;
    uint64_t n = 0;
    apkg_status code = ak_stream_hash(path, &n, actual, head, &head_size);
    if (code)
        return code;
    return n == size && !strcmp(actual, sha) ? APKG_OK : APKG_INTEGRITY;
}
static apkg_status copy_archive(const char *source, int output, uint64_t size,
                                ak_import_progress *progress) {
    unsigned char *buffer = malloc(CHUNK);
    uint64_t copied = 0;
    int input;
    apkg_status code = APKG_OK;
    if (!buffer)
        return APKG_NOMEM;
    if (pkg_fs_path_is_link(source) != 0 ||
        (input = open(source, O_RDONLY)) < 0) {
        free(buffer);
        return errno == ENOENT ? APKG_NOT_FOUND : APKG_IO;
    }
    while (!code) {
        ssize_t n = read(input, buffer, CHUNK);
        size_t done = 0;
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            code = APKG_IO;
        if (n <= 0)
            break;
        if ((uint64_t)n > size - copied) {
            code = APKG_INTEGRITY;
            break;
        }
        while (!code && done < (size_t)n) {
            ssize_t w = write(output, buffer + done, (size_t)n - done);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
                code = APKG_IO;
            else
                done += (size_t)w;
        }
        copied += (uint64_t)n;
        if (!code)
            code = ak_import_tick(progress, (uint64_t)n);
        if (!code && ak_fault(1301))
            code = APKG_IO;
    }
    close(input);
    free(buffer);
    return code;
}
/* Claim only an empty directory. The marker survives an interrupted first
 * transfer, so retry can clean up safely. Serialize claim, publish and sweep. */
static apkg_status claim_mirror(const char *dir) {
    char marker[AK_PATH];
    struct dirent *entry;
    if (ak_path(marker, dir, ".apkg-mirror-v1"))
        return APKG_LIMIT;
    if (is_directory(marker))
        return APKG_OK;
    DIR *d = opendir(dir);
    if (!d)
        return APKG_IO;
    apkg_status code = APKG_OK;
    errno = 0;
    while ((entry = readdir(d)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            code = APKG_CONFLICT;
            break;
        }
    if (!entry && errno)
        code = APKG_IO;
    closedir(d);
    if (!code && (mkdir(marker, 0700) || ak_sync_dir(dir)))
        code = APKG_IO;
    return code;
}
/* What an earlier mirror left and the current one does not reference. */
static void sweep(const char *dir, const char *keep, int snapshots) {
    char path[AK_PATH], (*names)[80] = NULL;
    size_t count = 0, capacity = 0;
    struct dirent *entry;
    DIR *d = opendir(dir);
    if (!d)
        return;
    while ((entry = readdir(d))) {
        size_t n = strlen(entry->d_name);
        int ours = snapshots
                       ? n == 64 && hex_name(entry->d_name, 64)
                       : (n == 68 && hex_name(entry->d_name, 64) &&
                          !strcmp(entry->d_name + 64, ".pkg")) ||
                             (n == 42 && !strncmp(entry->d_name, ".incoming.", 10) &&
                              hex_name(entry->d_name + 10, 32));
        if (!ours || !strcmp(entry->d_name, keep))
            continue;
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2 : 16;
            char (*next)[80] = calloc(grown, 80);
            if (!next)
                break;
            if (count)
                memcpy(next, names, count * 80);
            free(names);
            names = next;
            capacity = grown;
        }
        strcpy(names[count++], entry->d_name);
    }
    closedir(d);
    for (size_t i = 0; i < count; i++) {
        struct stat st;
        if (ak_path(path, dir, names[i]) || lstat(path, &st))
            continue;
        if (!snapshots) {
            if (S_ISREG(st.st_mode))
                unlink(path);
        } else if (S_ISDIR(st.st_mode)) {
            char file[AK_PATH];
            if (!ak_path(file, path, "channel.json"))
                unlink(file);
            if (!ak_path(file, path, "channel.signature.json"))
                unlink(file);
            rmdir(path); /* stays if anything else is in it */
        }
    }
    free(names);
    ak_sync_dir(dir);
}
apkg_status apkg_client_channel_mirror(const apkg_client_mirror *m,
                                       apkg_client_mirror_result *result,
                                       apkg_error **e) {
    apkg_open_options options = {0};
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    apkg_channel *channel = NULL;
    const apkg_channel_view *view;
    const apkg_channel_entry_view *best = NULL;
    apkg_candidate_view compared;
    apkg_bytes object, signature;
    apkg_id id;
    ak_import_progress progress = {0};
    char mirror[AK_PATH], snapshots[AK_PATH], snapshot[AK_PATH],
        archives[AK_PATH], archive[AK_PATH], temp[AK_PATH] = "", path[AK_PATH],
        digest[65], sha[65], leaf[80], reference[66];
    uint64_t best_index = 0;
    int fd = -1;
    void *lock = NULL;
    apkg_status code;
    if (e)
        *e = NULL;
    if (!result)
        return APKG_INVALID;
    memset(result, 0, sizeof *result);
    if (!m || m->contract_version != APKG_CONTRACT_VERSION ||
        m->struct_size != sizeof *m)
        return ak_fail(e, APKG_UNSUPPORTED, "API version/size mismatch", "");
    if (!m->root || !m->endpoint || !m->identity || !m->name ||
        !m->directory || !*m->directory || !ak_valid_name(m->name) ||
        strlen(m->identity) != 32 || ak_hex(m->identity, id.bytes, 16) ||
        strlen(m->directory) >= AK_PATH - 160)
        return APKG_INVALID;
    if (ak_cancelled(m->observer))
        return APKG_CANCELLED;
    /* The root is only read: its policy, target and replay floor decide what
     * is acceptable, and nothing in it or beside it changes. */
    options.contract_version = APKG_CONTRACT_VERSION;
    options.struct_size = sizeof options;
    options.mode = APKG_READ;
    options.root = (apkg_text){m->root, strlen(m->root)};
    ak_client_store(&options, m->store);
    code = apkg_open(&options, &c, e);
    if (!code)
        code = apkg_snapshot_read(c, &s, e);
    if (!code && s->view.state != APKG_ROOT_READY)
        code = ak_fail(e,
                       s->view.state == APKG_ROOT_PENDING
                           ? APKG_RECOVERY_REQUIRED
                           : APKG_POLICY,
                       "a mirror is made for a ready root: its policy decides "
                       "what is acceptable",
                       m->root);
    if (!code) {
        apkg_text endpoint = {m->endpoint, strlen(m->endpoint)};
        code = ak_http_url(m->endpoint)
                   ? apkg_channel_open_http(c, endpoint, &id, 0, m->observer,
                                            &channel, e)
                   : apkg_channel_open_file(c, endpoint, &id, 0, &channel, e);
    }
    if (code)
        goto done;
    view = apkg_channel_get(channel);
    for (uint64_t i = 0; i < view->entry_count; i++) {
        const apkg_channel_entry_view *row;
        char x[256], y[256];
        int order;
        code = apkg_channel_entry_at(channel, i, &row, e);
        if (code)
            goto done;
        if (row->name.size != strlen(m->name) ||
            memcmp(row->name.data, m->name, row->name.size) ||
            row->author_trust != APKG_TRUST_ACCEPTED || !row->target_matches)
            continue;
        if (!best) {
            best = row;
            best_index = i;
            continue;
        }
        if (best->authority.size != row->authority.size ||
            memcmp(best->authority.data, row->authority.data,
                   row->authority.size)) {
            code = ak_fail(e, APKG_CONFLICT,
                           "the channel offers this name from two authors",
                           m->name);
            goto done;
        }
        if (row->version.size >= sizeof x || best->version.size >= sizeof y) {
            code = APKG_LIMIT;
            goto done;
        }
        memcpy(x, row->version.data, row->version.size);
        x[row->version.size] = 0;
        memcpy(y, best->version.data, best->version.size);
        y[best->version.size] = 0;
        order = ak_version_compare(x, y);
        if (order == 0)
            order = row->revision > best->revision   ? 1
                    : row->revision < best->revision ? -1
                                                     : 0;
        if (order == 2 || order == 0) {
            code = ak_fail(e, APKG_CONFLICT,
                           "the channel's builds of this name have no defined "
                           "order",
                           m->name);
            goto done;
        }
        if (order == 1) {
            best = row;
            best_index = i;
        }
    }
    if (!best) {
        code = ak_fail(e, APKG_NOT_FOUND,
                       "the channel offers no trusted build of this name for "
                       "the root's target",
                       m->name);
        goto done;
    }
    if (best->version.size >= sizeof result->version) {
        code = APKG_LIMIT;
        goto done;
    }
    code = apkg_channel_compare(s, channel, best_index, &compared, e);
    if (!code)
        code = apkg_channel_observation(channel, &object, &signature);
    if (code)
        goto done;
    result->snapshot = view->snapshot_digest;
    result->sequence = view->sequence;
    result->descriptor = best->descriptor_digest;
    result->archive = best->archive_digest;
    result->archive_bytes = best->archive_bytes;
    result->revision = best->revision;
    memcpy(result->version, best->version.data, best->version.size);
    result->newer = compared.state == APKG_CANDIDATE_NEWER ||
                    compared.state == APKG_CANDIDATE_NOT_INSTALLED;
    result->installed = compared.state == APKG_CANDIDATE_CURRENT;
    ak_tohex(view->snapshot_digest.bytes, 32, digest);
    ak_tohex(best->archive_digest.bytes, 32, sha);
    snprintf(leaf, sizeof leaf, "%s.pkg", sha);
    snprintf(reference, sizeof reference, "%s\n", digest);
    if (directory(m->directory) ||
        pkg_fs_canonical_dir(m->directory, mirror, sizeof mirror)) {
        code = ak_fail(e, APKG_IO, "the mirror directory cannot be made",
                       m->directory);
        goto done;
    }
    if (!ak_http_url(m->endpoint)) {
        ak_identity source_id, dest_id;
        if (ak_stat(m->endpoint, &source_id) || ak_stat(mirror, &dest_id)) {
            code = APKG_IO;
            goto done;
        }
        if (source_id.st_dev == dest_id.st_dev && source_id.st_ino == dest_id.st_ino) {
            code = ak_fail(e, APKG_CONFLICT, "the source is also the mirror", mirror);
            goto done;
        }
    }
#ifdef __AROS__
    int busy = 0;
    lock = pkg_fs_lock_root(mirror, &busy);
    if (!lock) {
        code = busy ? APKG_BUSY : APKG_IO;
        goto done;
    }
#else
    lock = pkg_fs_lock_dir(mirror);
    if (!lock) {
        code = APKG_IO;
        goto done;
    }
#endif
    code = claim_mirror(mirror);
    if (code) {
        ak_fail(e, code, "use an empty directory or an existing apkg mirror", mirror);
        goto done;
    }
    if (ak_path(snapshots, mirror, "snapshots") ||
        ak_path(snapshot, snapshots, digest) ||
        ak_path(archives, mirror, "archives") ||
        ak_path(archive, archives, leaf)) {
        code = APKG_LIMIT;
        goto done;
    }
    /* Already complete: the same snapshot and its archive, verified. */
    if (!ak_path(path, mirror, "current") &&
        !holds(path, (const unsigned char *)reference, 65) &&
        !ak_path(path, snapshot, "channel.json") &&
        !holds(path, object.data, object.size) &&
        !ak_path(path, snapshot, "channel.signature.json") &&
        !holds(path, signature.data, signature.size) &&
        archive_holds(archive, sha, best->archive_bytes) == APKG_OK) {
        goto swept;
    }
    if (directory(snapshots) || directory(snapshot) || directory(archives)) {
        code = APKG_IO;
        goto done;
    }
    /* The snapshot was verified when the channel opened; these are the
     * bytes that were verified. */
    code = publish_bytes(snapshot, "channel.json", object.data, object.size);
    if (!code)
        code = publish_bytes(snapshot, "channel.signature.json", signature.data,
                             signature.size);
    if (!code && ak_sync_dir(snapshots))
        code = APKG_IO;
    if (code)
        goto done;
    if (archive_holds(archive, sha, best->archive_bytes) != APKG_OK) {
        /* Neither transport resumes: an unfinished or damaged copy is
         * discarded and the archive is fetched whole. It takes its name only
         * with the size and digest the signed snapshot gives it. */
        unsigned char random[16];
        char id_text[33], name[64], member[128];
        progress.observer = m->observer;
        progress.total = best->archive_bytes;
        progress.subject = m->endpoint;
        progress.has_phase = 1;
        progress.phase = APKG_PHASE_FETCH;
        if (pkg_fs_random(random, sizeof random) ||
            (m->observer && pkg_fs_random(progress.operation_id.bytes, 16))) {
            code = APKG_IO;
            goto done;
        }
        ak_tohex(random, 16, id_text);
        snprintf(name, sizeof name, ".incoming.%s", id_text);
        snprintf(member, sizeof member, "archives/%s", leaf);
        if (ak_path(temp, archives, name)) {
            code = APKG_LIMIT;
            goto done;
        }
        fd = open(temp, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd < 0) {
            code = APKG_IO;
            goto done;
        }
        if (ak_http_url(m->endpoint)) {
            code = ak_http_path(m->endpoint, member, path);
            if (!code)
                code = ak_http_get(path, best->archive_bytes, fd, NULL, NULL,
                                   &progress);
        } else if (ak_path(path, m->endpoint, member))
            code = APKG_LIMIT;
        else {
            code = ak_import_tick(&progress, 0);
            if (!code)
                code = copy_archive(path, fd, best->archive_bytes, &progress);
        }
        if (!code && fsync(fd))
            code = APKG_IO;
        if (close(fd) && !code)
            code = APKG_IO;
        fd = -1;
        if (!code && ak_fault(1302))
            code = APKG_IO;
        if (!code)
            code = archive_holds(temp, sha, best->archive_bytes);
        if (!code && (pkg_fs_replace(temp, archive) || ak_sync_dir(archives)))
            code = APKG_IO;
        if (code) {
            if (e && !*e)
                ak_fail(e, code,
                        code == APKG_INTEGRITY
                            ? "the fetched archive is not the one the signed "
                              "snapshot names"
                            : "the archive could not be fetched",
                        m->endpoint);
            goto done;
        }
        *temp = 0;
    }
    if (ak_cancelled(m->observer)) {
        code = APKG_CANCELLED;
        goto done;
    }
    if (ak_fault(1303)) {
        code = APKG_IO;
        goto done;
    }
    /* Last, and in one step: until here a reader of the mirror sees the
     * previous complete mirror, or none. */
    code = publish_bytes(mirror, "current", (const unsigned char *)reference,
                         65);
    if (code)
        goto done;
    result->changed = 1;
swept:
    sweep(archives, leaf, 0);
    sweep(snapshots, digest, 1);
done:
    if (fd >= 0)
        close(fd);
    if (*temp)
        unlink(temp);
#ifdef __AROS__
    pkg_fs_unlock_root(lock);
#else
    pkg_fs_unlock_dir(lock);
#endif
    apkg_channel_free(channel);
    apkg_snapshot_free(s);
    apkg_close(c);
    if (code && e && !*e)
        ak_fail(e, code, "the channel could not be mirrored", m->name);
    return code;
}
