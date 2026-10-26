/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Streaming envelope over the existing AROS PKG/1 container wire format. */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "pkg_container.h"
#include "pkg_fs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define AK_ARCHIVE_MAX 2147483647u
static int exact_read(int fd, void *bytes, size_t n) {
    unsigned char *p = bytes;
    while (n) {
        ssize_t got = read(fd, p, n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return -1;
        p += (size_t)got;
        n -= (size_t)got;
    }
    return 0;
}
static apkg_status member(int fd, uint64_t *remaining, char name[129],
                          uint64_t *size) {
    unsigned char field[4];
    uint64_t path;
    if (*remaining < 4 || exact_read(fd, field, 4))
        return APKG_INTEGRITY;
    *remaining -= 4;
    path = pkg_be32_get(field);
    if (!path || path > 128 || path + 5 > *remaining)
        return APKG_INVALID;
    if (exact_read(fd, name, (size_t)path + 1) || name[path] ||
        memchr(name, 0, (size_t)path) || exact_read(fd, field, 4))
        return APKG_INVALID;
    *remaining -= path + 5;
    *size = pkg_be32_get(field);
    return *size > *remaining ? APKG_INTEGRITY : APKG_OK;
}
static apkg_status metadata(int fd, uint64_t *left, const char *expected,
                            size_t max, unsigned char **out, size_t *n) {
    char name[129];
    uint64_t size;
    apkg_status code = member(fd, left, name, &size);
    if (code)
        return code;
    if (strcmp(name, expected))
        return APKG_INVALID;
    if (size > max || size > SIZE_MAX - 1)
        return APKG_LIMIT;
    *out = malloc((size_t)size + 1);
    if (!*out)
        return APKG_NOMEM;
    if (exact_read(fd, *out, (size_t)size))
        return APKG_INTEGRITY;
    (*out)[size] = 0;
    *n = (size_t)size;
    *left -= size;
    return APKG_OK;
}
apkg_status apkg_package_import(apkg_ctx *c, apkg_text archive,
                                const apkg_observer *observer,
                                apkg_package **out, apkg_error **e) {
    char path[AK_PATH];
    unsigned char header[8], *descriptor = NULL, *signature = NULL;
    size_t dn = 0, sn = 0;
    uint64_t remaining, size;
    int fd = -1, arr;
    struct stat start, end;
    apkg_package *p = NULL;
    apkg_status code = APKG_INVALID;
    unsigned char *seen = NULL;
    size_t entries = 0, i;
    ak_import_progress progress = {0};
    progress.observer = observer;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c || !archive.data || !archive.size || archive.size >= sizeof path ||
        memchr(archive.data, 0, archive.size))
        return APKG_INVALID;
    if (c->mode != APKG_WRITE)
        return APKG_POLICY;
    if (ak_cancelled(observer))
        return APKG_CANCELLED;
    if (!ak_physical(c->anchor, &c->binding))
        return APKG_STALE;
    memcpy(path, archive.data, archive.size);
    path[archive.size] = 0;
    if (pkg_fs_path_is_link(path) != 0)
        return ak_fail(e, APKG_INVALID, "archive path is a link", path);
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(path, flags);
    if (fd < 0) {
        code = APKG_IO;
        goto done;
    }
    if (fstat(fd, &start) || !S_ISREG(start.st_mode) || start.st_size < 8 ||
        start.st_size > AK_ARCHIVE_MAX) {
        code = APKG_LIMIT;
        goto done;
    }
    if (exact_read(fd, header, 8)) {
        code = APKG_INTEGRITY;
        goto done;
    }
    if (memcmp(header, "PKG\1", 4)) {
        code = APKG_UNSUPPORTED;
        goto done;
    }
    size = pkg_be32_get(header + 4);
    if (size != (uint64_t)start.st_size) {
        code = APKG_INTEGRITY;
        goto done;
    }
    remaining = size - 8;
    progress.total = size;
    progress.subject = path;
    if (observer && pkg_fs_random(progress.operation_id.bytes, 16)) {
        code = APKG_IO;
        goto done;
    }
    size_t max =
        c->object_max < c->memory_max / 32 ? c->object_max : c->memory_max / 32;
    code = metadata(fd, &remaining, "descriptor.json", max, &descriptor, &dn);
    if (code)
        goto done;
    code = metadata(fd, &remaining, "signature.json", max, &signature, &sn);
    if (code)
        goto done;
    {
        apkg_bytes d = {descriptor, dn}, s = {signature, sn};
        code = apkg_package_read(c, d, s, &p, e);
        if (code)
            goto done;
    }
    progress.completed = size - remaining;
    code = ak_import_tick(&progress, 0);
    if (code)
        goto done;
    arr = ak_member(&p->json, ak_member(&p->json, 0, "body"), "files");
    seen = calloc(p->json.count, 1);
    if (!seen) {
        code = APKG_NOMEM;
        goto done;
    }
    while (remaining) {
        char name[129], sha[65];
        uint64_t n;
        int found = 0;
        code = member(fd, &remaining, name, &n);
        if (code)
            goto done;
        if (strncmp(name, "objects/", 8) || strlen(name) != 72) {
            code = APKG_INVALID;
            goto done;
        }
        memcpy(sha, name + 8, 65);
        unsigned char digest[32];
        if (ak_hex(sha, digest, 32)) {
            code = APKG_INVALID;
            goto done;
        }
        for (i = (size_t)arr + 1; i < p->json.tokens[arr].next;
             i = p->json.tokens[i].next)
            if (ak_equal(&p->json, ak_member(&p->json, (int)i, "blob"), sha)) {
                uint64_t expected;
                if (ak_u64(&p->json, ak_member(&p->json, (int)i, "size"),
                           &expected) ||
                    expected != n) {
                    code = APKG_INTEGRITY;
                    goto done;
                }
                if (seen[i]) {
                    code = APKG_INVALID;
                    goto done;
                }
                seen[i] = 1;
                found = 1;
            }
        if (!found || ++entries > AK_TOKENS) {
            code = APKG_INVALID;
            goto done;
        }
        progress.completed = size - remaining;
        code = ak_blob_ingest(c, sha, n, fd, &progress);
        if (code)
            goto done;
        remaining -= n;
    }
    /* A stale cache cannot make an incomplete transport look complete. */
    for (i = (size_t)arr + 1; i < p->json.tokens[arr].next;
         i = p->json.tokens[i].next)
        if (ak_member(&p->json, (int)i, "blob") >= 0 && !seen[i]) {
            code = APKG_INTEGRITY;
            goto done;
        }
    if (fstat(fd, &end) || start.st_dev != end.st_dev ||
        start.st_ino != end.st_ino || start.st_size != end.st_size) {
        code = APKG_STALE;
        goto done;
    }
    code = ak_package_payload_check(c, p);
    if (code)
        goto done;
    *out = p;
    p = NULL;
    code = APKG_OK;
done:
    if (fd >= 0)
        close(fd);
    free(descriptor);
    free(signature);
    free(seen);
    apkg_package_free(p);
    if (code && e && !*e)
        ak_fail(e, code, "signed package archive refused", path);
    return code;
}
apkg_status ak_package_source(apkg_ctx *c, const char *source,
                              const apkg_observer *observer, apkg_package **out,
                              apkg_error **e) {
    struct stat st;
    unsigned char *d = NULL, *s = NULL;
    size_t dn = 0, sn = 0;
    char path[AK_PATH];
    apkg_status code;
    if (!source || !out)
        return APKG_INVALID;
    *out = NULL;
    if (lstat(source, &st))
        return APKG_IO;
    if (S_ISREG(st.st_mode)) {
        apkg_text archive = {source, strlen(source)};
        return apkg_package_import(c, archive, observer, out, e);
    }
    if (!S_ISDIR(st.st_mode))
        return APKG_INVALID;
    if (ak_path(path, source, "descriptor.json") ||
        ak_read(path, &d, &dn, c->object_max) ||
        ak_path(path, source, "signature.json") ||
        ak_read(path, &s, &sn, c->object_max))
        code = errno == ENOMEM  ? APKG_NOMEM
               : errno == EFBIG ? APKG_LIMIT
                                : APKG_IO;
    else {
        apkg_bytes db = {d, dn}, sb = {s, sn};
        code = apkg_package_read(c, db, sb, out, e);
    }
    free(d);
    free(s);
    return code;
}
