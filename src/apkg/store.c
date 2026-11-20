/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Pruning of a root's store: the verified objects and the fetched archives.
 * Nothing but pruning ever removes from the store, so on a small volume, or
 * with a store that holds a whole system, it is what keeps the store to the
 * bytes repair of the installed versions needs. */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "pkg_fs.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char (*names)[65];
    size_t count, capacity;
} digests;
static int digest_compare(const void *a, const void *b) {
    return strcmp(a, b);
}
/* A prune retains only this root's live objects. Bind an external store
 * before its first import so a second root cannot lose its repair data. */
apkg_status ak_store_claim(apkg_ctx *c) {
    char owner[AK_PATH], objects[AK_PATH], archives[AK_PATH], temp[AK_PATH];
    unsigned char *bytes = NULL, random[16];
    char leaf[64] = ".apkg-owner.";
    size_t size = 0;
    if (!*c->store)
        return APKG_OK;
    if (ak_path(owner, c->store, ".apkg-owner") ||
        ak_store_path(c, "objects", objects) || ak_store_path(c, "archives", archives))
        return APKG_LIMIT;
    if (!ak_exists(owner)) {
        if (ak_exists(objects) || ak_exists(archives)) {
            /* Another first writer may already have published its marker. */
            if (ak_exists(owner) != 1)
                return APKG_CONFLICT;
        } else {
            if (pkg_fs_random(random, sizeof random))
                return APKG_IO;
            ak_tohex(random, sizeof random, leaf + strlen(leaf));
            if (ak_path(temp, c->store, leaf))
                return APKG_LIMIT;
            if (ak_write(temp, c->root, strlen(c->root), 1)) {
                unlink(temp);
                return APKG_IO;
            }
            if (ak_publish(temp, owner)) {
                unlink(temp);
                if (ak_exists(owner) != 1)
                    return APKG_IO;
            }
            if (ak_sync_dir(c->store))
                return APKG_IO;
        }
    }
    if (ak_read(owner, &bytes, &size, AK_PATH))
        return APKG_IO;
    int matches = size == strlen(c->root) && !memcmp(bytes, c->root, size);
    free(bytes);
    return matches ? APKG_OK : APKG_CONFLICT;
}
static int hex_name(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}
/* 1 object, 2 archive, 3 unfinished transfer, 0 not a name the engine gives. */
static int store_name(const char *name) {
    size_t n = strlen(name);
    if (n == 64 && hex_name(name, 64))
        return 1;
    if (n == 68 && hex_name(name, 64) && !strcmp(name + 64, ".pkg"))
        return 2;
    if (n == 42 && !strncmp(name, ".incoming.", 10) && hex_name(name + 10, 32))
        return 3;
    return 0;
}
static apkg_status keep_add(digests *keep, const char *sha, size_t max) {
    if (keep->count == keep->capacity) {
        size_t capacity = keep->capacity ? keep->capacity * 2 : 256;
        char (*grown)[65];
        if (capacity > max / 65)
            return APKG_LIMIT;
        grown = calloc(capacity, 65);
        if (!grown)
            return APKG_NOMEM;
        if (keep->count)
            memcpy(grown, keep->names, keep->count * 65);
        free(keep->names);
        keep->names = grown;
        keep->capacity = capacity;
    }
    strcpy(keep->names[keep->count++], sha);
    return APKG_OK;
}
/* Every content digest an installed descriptor of the committed state lists. */
static apkg_status keep_build(apkg_ctx *c, const apkg_snapshot *s,
                              digests *keep) {
    ak_json j = {0};
    int parsed = ak_json_parse(s->raw, s->view.source_record.size, &j), rows;
    apkg_status code = APKG_OK;
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    rows = ak_member(&j, ak_member(&j, 0, "body"), "installed");
    if (rows < 0) {
        ak_json_free(&j);
        return APKG_INVALID;
    }
    for (size_t r = (size_t)rows + 1; r < j.tokens[rows].next && !code;
         r = j.tokens[r].next) {
        int files = ak_member(
            &j,
            ak_member(&j, ak_member(&j, (int)r, "descriptor"), "body"),
            "files");
        if (files < 0) {
            code = APKG_INVALID;
            break;
        }
        for (size_t f = (size_t)files + 1; f < j.tokens[files].next && !code;
             f = j.tokens[f].next) {
            char sha[65];
            if (!ak_equal(&j, ak_member(&j, (int)f, "type"), "file"))
                continue;
            if (ak_string(&j, ak_member(&j, (int)f, "sha256"), sha, sizeof sha))
                code = APKG_INVALID;
            else
                code = keep_add(keep, sha, c->memory_max);
        }
    }
    ak_json_free(&j);
    if (!code && keep->count)
        qsort(keep->names, keep->count, 65, digest_compare);
    return code;
}
/* Names are collected before anything is removed: a DOS directory scan does
 * not survive deletions made under it. */
static apkg_status prune_directory(apkg_ctx *c, const char *kind,
                                   const digests *keep, uint64_t *files,
                                   uint64_t *bytes) {
    char dir[AK_PATH], path[AK_PATH], (*names)[69] = NULL;
    size_t count = 0, capacity = 0;
    struct stat st;
    struct dirent *entry;
    DIR *d;
    apkg_status code = APKG_OK;
    if (ak_store_path(c, kind, dir))
        return APKG_LIMIT;
    if (lstat(dir, &st))
        return errno == ENOENT ? APKG_OK : APKG_IO;
    if (!S_ISDIR(st.st_mode) || pkg_fs_path_is_link(dir) != 0)
        return APKG_UNSUPPORTED;
    d = opendir(dir);
    if (!d)
        return APKG_IO;
    errno = 0;
    while ((entry = readdir(d))) {
        int what = store_name(entry->d_name);
        if (what == 1 && keep->count &&
            bsearch(entry->d_name, keep->names, keep->count, 65,
                    digest_compare))
            what = 0;
        if (what) {
            if (count == capacity) {
                size_t grown = capacity ? capacity * 2 : 256;
                char (*next)[69] = calloc(grown, 69);
                if (!next) {
                    code = APKG_NOMEM;
                    break;
                }
                if (count)
                    memcpy(next, names, count * 69);
                free(names);
                names = next;
                capacity = grown;
            }
            strcpy(names[count++], entry->d_name);
        }
        errno = 0;
    }
    if (!code && errno)
        code = APKG_IO;
    closedir(d);
    for (size_t i = 0; i < count && !code; i++) {
        if (ak_path(path, dir, names[i]))
            code = APKG_LIMIT;
        else if (lstat(path, &st))
            code = errno == ENOENT ? APKG_OK : APKG_IO;
        else if (!S_ISREG(st.st_mode))
            continue; /* not something the engine stored */
        else if (unlink(path))
            code = APKG_IO;
        else {
            if (files)
                ++*files;
            if (bytes)
                *bytes += (uint64_t)st.st_size;
        }
    }
    free(names);
    if (!code && count && ak_sync_dir(dir))
        code = APKG_IO;
    return code;
}
/* Caller holds the mutation lock. */
apkg_status ak_store_prune(apkg_ctx *c, uint64_t *files, uint64_t *bytes) {
    apkg_snapshot *s = NULL;
    digests keep = {0};
    apkg_status code = ak_snapshot(c, &s, NULL);
    if (files)
        *files = 0;
    if (bytes)
        *bytes = 0;
    if (code)
        return code;
    /* A pending transaction still needs what it was approved with. */
    if (s->view.state != APKG_ROOT_READY)
        code = s->view.state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED
                                                  : APKG_POLICY;
    if (!code)
        code = ak_store_claim(c);
    if (!code)
        code = keep_build(c, s, &keep);
    if (!code)
        code = prune_directory(c, "objects", &keep, files, bytes);
    if (!code)
        code = prune_directory(c, "archives", &keep, files, bytes);
    free(keep.names);
    apkg_snapshot_free(s);
    return code;
}
