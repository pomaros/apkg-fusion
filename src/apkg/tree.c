/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "pkg_fs.h"
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/xattr.h>
#endif
#ifdef __APPLE__
#include <sys/acl.h>
#endif
int ak_buf_add(ak_buf *b, const char *fmt, ...) {
    va_list a;
    int n;
    size_t cap;
    char *p;
    if (b->bad)
        return -1;
    va_start(a, fmt);
    n = vsnprintf(NULL, 0, fmt, a);
    va_end(a);
    if (n < 0 || (size_t)n > b->max - b->size) {
        b->bad = APKG_LIMIT;
        return -1;
    }
    if (b->size + (size_t)n + 1 > b->cap) {
        cap = b->size + (size_t)n + 1;
        if (cap < b->cap * 2 && b->cap * 2 <= b->max + 1)
            cap = b->cap * 2;
        p = malloc(cap);
        if (!p) {
            b->bad = APKG_NOMEM;
            return -1;
        }
        if (b->size)
            memcpy(p, b->data, b->size);
        free(b->data);
        b->data = p;
        b->cap = cap;
    }
    va_start(a, fmt);
    vsnprintf(b->data + b->size, b->cap - b->size, fmt, a);
    va_end(a);
    b->size += (size_t)n;
    return 0;
}
char *ak_slice(const ak_json *j, int t) {
    return t < 0 ? NULL
                 : ak_copy(j->text + j->tokens[t].start,
                           j->tokens[t].end - j->tokens[t].start);
}
/* Unknown host metadata cannot be round-tripped. The recognized Amiga xattr
 * is validated separately and becomes part of every observed inventory. */
static int metadata_supported(const char *path, const struct stat *st) {
    if (st->st_mode & 07000)
        return 0;
#ifndef __AROS__
    if (st->st_uid != geteuid() || st->st_gid != getegid())
        return 0;
#endif
#if defined(__APPLE__) || defined(__linux__)
    char names[16384];
    ssize_t n;
#ifdef __APPLE__
    n = listxattr(path, names, sizeof names, XATTR_NOFOLLOW);
#else
    n = llistxattr(path, names, sizeof names);
#endif
    if (n < 0 && errno != ENOTSUP)
        return 0;
    for (ssize_t pos = 0; pos < n;) {
        size_t len = strnlen(names + pos, (size_t)(n - pos));
        if (len == (size_t)(n - pos))
            return 0;
        if (strcmp(names + pos, "user.apkg.amiga")
#ifdef __APPLE__
            && strcmp(names + pos, "com.apple.provenance")
#endif
        )
            return 0;
        pos += (ssize_t)len + 1;
    }
#ifdef __APPLE__
    acl_t acl = acl_get_file(path, ACL_TYPE_EXTENDED);
    if (acl) {
        acl_entry_t entry;
        int present = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
        acl_free(acl);
        if (present == 0)
            return 0;
    }
#endif
#endif
    return 1;
}
static int names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static int scan(const char *, const char *, ak_buf *, unsigned, size_t *);
static int scan_in(const char *base, const char *rel, ak_buf *out,
                   unsigned depth, size_t *names_memory, char *path,
                   char *sub, char *full) {
    DIR *d;
    struct dirent *e;
    char **list = NULL;
    size_t n = 0, i, capacity = 0, held = 0;
    int bad = 0;
    if (depth > 32 ||
        (*rel ? ak_path(path, base, rel) : (strcpy(path, base), 0)))
        return APKG_LIMIT;
    if (out->bad)
        return out->bad;
    d = opendir(path);
    if (!d)
        return APKG_IO;
    errno = 0;
    while ((e = readdir(d))) {
        char **next;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        size_t cost = strlen(e->d_name) + 1 + 2 * sizeof(char *);
        if (n >= AK_TOKENS || cost > out->max - *names_memory) {
            bad = APKG_LIMIT;
            break;
        }
        if (n == capacity) {
            size_t cap = capacity ? capacity * 2 : 16;
            if (cap > AK_TOKENS)
                cap = AK_TOKENS;
            next = malloc(cap * sizeof *next);
            if (!next) {
                bad = APKG_NOMEM;
                break;
            }
            if (n)
                memcpy(next, list, n * sizeof *next);
            free(list);
            list = next;
            capacity = cap;
        }
        list[n] = ak_copy(e->d_name, strlen(e->d_name));
        if (!list[n]) {
            bad = APKG_NOMEM;
            break;
        }
        *names_memory += cost;
        held += cost;
        n++;
        errno = 0;
    }
    if (errno && !bad)
        bad = APKG_IO;
    closedir(d);
    if (n > 1)
        qsort(list, n, sizeof *list, names);
    for (i = 0; i < n && !bad; i++) {
        struct stat st;
        if ((*rel ? ak_path(sub, rel, list[i])
                  : (strlen(list[i]) >= AK_PATH ? -1
                                                : (strcpy(sub, list[i]), 0))) ||
            !ak_valid_path(sub) || ak_path(full, base, sub)) {
            bad = APKG_UNSUPPORTED;
            break;
        }
        if (lstat(full, &st)) {
            bad = APKG_IO;
            break;
        }
        if (!metadata_supported(full, &st)) {
            bad = APKG_UNSUPPORTED;
            break;
        }
        char *attributes = NULL;
        bad = ak_attributes_read(full, (uint64_t)(st.st_mode & 0777),
                                 &attributes);
        if (bad)
            break;
        ak_buf prefix = {0};
        prefix.max = 32768;
        if (attributes)
            ak_buf_add(&prefix, "\"amiga\":%s,", attributes);
        free(attributes);
        if (prefix.bad) {
            bad = prefix.bad;
            free(prefix.data);
            break;
        }
        const char *meta = prefix.data ? prefix.data : "";
        if (S_ISDIR(st.st_mode)) {
            ak_buf_add(
                out,
                "%s{%s\"mode\":\"%u\",\"path\":\"%s\",\"type\":\"directory\"}",
                out->size ? "," : "", meta, (unsigned)(st.st_mode & 0777), sub);
            bad = scan(base, sub, out, depth + 1, names_memory);
        } else if (S_ISREG(st.st_mode) && st.st_nlink == 1) {
            uint64_t len = 0;
            unsigned char head[64];
            size_t hn = 0;
            char sha[65];
            bad = ak_stream_hash(full, &len, sha, head, &hn);
            if (bad) {
                free(prefix.data);
                break;
            }
            ak_buf_add(out,
                       "%s{%s\"blob\":\"%s\",\"mode\":\"%u\",\"path\":\"%s\","
                       "\"sha256\":\"%s\",\"size\":\"%" PRIu64
                       "\",\"type\":\"file\"}",
                       out->size ? "," : "", meta, sha,
                       (unsigned)(st.st_mode & 0777), sub, sha, len);

        } else
            bad = APKG_UNSUPPORTED;
        free(prefix.data);
        if (out->bad)
            bad = out->bad;
    }
    for (i = 0; i < n; i++)
        free(list[i]);
    free(list);
    *names_memory -= held;
    return bad;
}
/* One level's three path buffers live on the heap: 32 levels of 12 KiB
 * would overrun a 256 KiB AROS worker stack, which has no guard page. */
static int scan(const char *base, const char *rel, ak_buf *out, unsigned depth,
                size_t *names_memory) {
    char *paths = malloc(3 * (size_t)AK_PATH);
    int r;
    if (!paths)
        return APKG_NOMEM;
    r = scan_in(base, rel, out, depth, names_memory, paths, paths + AK_PATH,
                paths + 2 * AK_PATH);
    free(paths);
    return r;
}
/* Entries are globally sorted after the walk: DFS order differs for a.b/a/x. */
static int records(const void *a, const void *b) {
    return strcmp(((const ak_tree_row *)a)->path,
                  ((const ak_tree_row *)b)->path);
}
static apkg_status tree_read(const char *base, size_t max, char **out,
                             struct stat *identity) {
    ak_buf files = {0}, result = {0};
    ak_json j = {0};
    ak_tree_row *rows = NULL;
    size_t count = 0, i, k = 0;
    int code;
    char *rootattrs = NULL;
    *out = NULL;
    files.max = result.max = max;
    if (ak_lstat(base, identity)) {
        if (errno == ENOENT) {
            *out = ak_copy("null", 4);
            return *out ? APKG_OK : APKG_NOMEM;
        }
        return APKG_IO;
    }
    if (!S_ISDIR(identity->st_mode) || !metadata_supported(base, identity))
        return APKG_UNSUPPORTED;
    code = ak_attributes_read(base, (uint64_t)(identity->st_mode & 0777),
                              &rootattrs);
    if (code) {
        free(rootattrs);
        return code;
    }
    size_t names_memory = 0;
    code = scan(base, "", &files, 0, &names_memory);
    if (code)
        goto done;
    /* Parse an array to sort complete records without changing their bytes. */
    ak_buf_add(&result, "[%s]", files.data ? files.data : "");
    if (result.bad) {
        code = result.bad;
        goto done;
    }
    code = ak_json_parse(result.data, result.size, &j);
    if (code) {
        code = code == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    for (i = 1; i < j.tokens[0].next; i = j.tokens[i].next)
        count++;
    if (count > max / sizeof *rows) {
        code = APKG_LIMIT;
        goto done;
    }
    rows = calloc(count ? count : 1, sizeof *rows);
    if (!rows) {
        code = APKG_NOMEM;
        goto done;
    }
    for (i = 1; i < j.tokens[0].next; i = j.tokens[i].next) {
        char path[AK_PATH];
        if (ak_string(&j, ak_member(&j, (int)i, "path"), path, sizeof path)) {
            code = APKG_INVALID;
            goto done;
        }
        rows[k].path = ak_copy(path, strlen(path));
        if (!rows[k].path) {
            code = APKG_NOMEM;
            goto done;
        }
        rows[k++].token = (int)i;
    }
    if (count > 1)
        qsort(rows, count, sizeof *rows, records);
    free(files.data);
    memset(&files, 0, sizeof files);
    files.max = max;
    ak_buf_add(&files, "{");
    if (rootattrs)
        ak_buf_add(&files, "\"amiga\":%s,", rootattrs);
    ak_buf_add(&files, "\"files\":[");
    for (i = 0; i < count; i++) {
        int t = rows[i].token;
        ak_buf_add(&files, "%s%.*s", i ? "," : "",
                   (int)(j.tokens[t].end - j.tokens[t].start),
                   j.text + j.tokens[t].start);
    }
    ak_buf_add(&files, "],\"mode\":\"%u\"}",
               (unsigned)(identity->st_mode & 0777));
    code = files.bad;
    if (!code) {
        *out = files.data;
        files.data = NULL;
    }
done:
    free(rootattrs);
    if (rows)
        for (i = 0; i < count; i++)
            free(rows[i].path);
    free(rows);
    free(files.data);
    free(result.data);
    ak_json_free(&j);
    if (code == APKG_NOMEM)
        errno = ENOMEM;
    return code;
}
apkg_status ak_tree_read(const char *base, size_t max, char **out,
                         struct stat *identity) {
    return tree_read(base, max, out, identity);
}
apkg_status ak_tree_source_read(const char *base, size_t max, char **out,
                                struct stat *identity) {
    return tree_read(base, max, out, identity);
}
/* Observe exactly one selected authoring entry, without enumerating siblings
 * or reading their content. The caller supplies a validated relative name. */
apkg_status ak_tree_entry(const char *path, const char *name, char **out) {
    struct stat st;
    ak_buf b = {0};
    char *attributes = NULL;
    apkg_status code;
    *out = NULL;
    b.max = 32768;
    if (lstat(path, &st))
        return errno == ENOENT ? APKG_NOT_FOUND : APKG_IO;
    if (!metadata_supported(path, &st) ||
        (!S_ISDIR(st.st_mode) && (!S_ISREG(st.st_mode) || st.st_nlink != 1)))
        return APKG_UNSUPPORTED;
    if (!(st.st_mode & 0400) || (S_ISDIR(st.st_mode) && !(st.st_mode & 0100)))
        return APKG_UNSUPPORTED;
    code = ak_attributes_read(path, (uint64_t)(st.st_mode & 0777), &attributes);
    if (code)
        return code;
    ak_buf_add(&b, "{");
    if (attributes)
        ak_buf_add(&b, "\"amiga\":%s,", attributes);
    free(attributes);
    if (S_ISDIR(st.st_mode))
        ak_buf_add(&b,
                   "\"mode\":\"%u\",\"path\":\"%s\",\"type\":\"directory\"}",
                   (unsigned)(st.st_mode & 0777), name);
    else {
        uint64_t size;
        char sha[65];
        unsigned char head[64];
        size_t count;
        code = ak_stream_hash(path, &size, sha, head, &count);
        if (code) {
            free(b.data);
            return code;
        }
        ak_buf_add(&b,
                   "\"blob\":\"%s\",\"mode\":\"%u\",\"path\":\"%s\",\"sha256\":"
                   "\"%s\",\"size\":\"%" PRIu64 "\",\"type\":\"file\"}",
                   sha, (unsigned)(st.st_mode & 0777), name, sha, size);
    }
    code = (apkg_status)b.bad;
    if (code)
        free(b.data);
    else
        *out = b.data;
    return code;
}
int ak_tree_find(const ak_json *j, const char *path) {
    int arr = ak_member(j, 0, "files");
    size_t i;
    if (arr < 0)
        return -1;
    for (i = (size_t)arr + 1; i < j->tokens[arr].next; i = j->tokens[i].next)
        if (ak_equal(j, ak_member(j, (int)i, "path"), path))
            return (int)i;
    return -1;
}
/* Normalize old inline journal inventories to metadata-only inventories for
 * comparison. This retains recovery compatibility without reading file bytes
 * into memory or changing already approved journal records. */
apkg_status ak_tree_normalize(const char *raw, char **out) {
    ak_json j = {0};
    ak_buf b = {0};
    int arr;
    size_t i;
    apkg_status code;
    char mode[24], previous[AK_PATH] = "";
    uint64_t permissions;
    *out = NULL;
    b.max = AK_MAX;
    int parsed = ak_json_parse(raw, strlen(raw), &j);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    if (!strcmp(raw, "null")) {
        *out = ak_copy(raw, 4);
        code = *out ? APKG_OK : APKG_NOMEM;
        goto done;
    }
    arr = ak_member(&j, 0, "files");
    errno = 0;
    if (!ak_fields_optional(&j, 0, (const char *const[]){"files", "mode"}, 2,
                            (const char *const[]){"amiga"}, 1) ||
        arr < 0 || j.tokens[arr].type != '[' ||
        ak_u64(&j, ak_member(&j, 0, "mode"), &permissions) ||
        permissions > 0777 ||
        !ak_attributes_valid(&j, ak_member(&j, 0, "amiga"), permissions) ||
        ak_string(&j, ak_member(&j, 0, "mode"), mode, sizeof mode)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    ak_buf_add(&b, "{");
    int root_attributes = ak_member(&j, 0, "amiga");
    if (root_attributes >= 0) {
        char *attributes = ak_slice(&j, root_attributes);
        if (!attributes) {
            code = APKG_NOMEM;
            goto done;
        }
        ak_buf_add(&b, "\"amiga\":%s,", attributes);
        free(attributes);
    }
    ak_buf_add(&b, "\"files\":[");
    for (i = (size_t)arr + 1; i < j.tokens[arr].next; i = j.tokens[i].next) {
        char path[AK_PATH], fm[24], sha[65], size[24];
        if (ak_string(&j, ak_member(&j, (int)i, "path"), path, sizeof path) ||
            !ak_valid_path(path) ||
            ak_string(&j, ak_member(&j, (int)i, "mode"), fm, sizeof fm)) {
            code = APKG_INVALID;
            goto done;
        }
        if ((*previous && strcmp(previous, path) >= 0) ||
            ak_u64(&j, ak_member(&j, (int)i, "mode"), &permissions) ||
            permissions > 0777) {
            code = APKG_INVALID;
            goto done;
        }
        strcpy(previous, path);
        int directory =
                ak_equal(&j, ak_member(&j, (int)i, "type"), "directory"),
            data = ak_member(&j, (int)i, "data");
        const char *const dir_fields[] = {"mode", "path", "type"};
        const char *const blob_fields[] = {"blob",   "mode", "path",
                                           "sha256", "size", "type"};
        const char *const data_fields[] = {"data",   "mode", "path",
                                           "sha256", "size", "type"};
        if (!ak_fields_optional(&j, (int)i,
                                directory   ? dir_fields
                                : data >= 0 ? data_fields
                                            : blob_fields,
                                directory ? 3 : 6,
                                (const char *const[]){"amiga"}, 1) ||
            !ak_attributes_valid(&j, ak_member(&j, (int)i, "amiga"),
                                 permissions) ||
            (!directory &&
             !ak_equal(&j, ak_member(&j, (int)i, "type"), "file"))) {
            code = APKG_INVALID;
            goto done;
        }
        int at = ak_member(&j, (int)i, "amiga");
        char *attributes = at < 0 ? NULL : ak_slice(&j, at);
        if (at >= 0 && !attributes) {
            code = APKG_NOMEM;
            goto done;
        }
        ak_buf_add(&b, "%s{", i == (size_t)arr + 1 ? "" : ",");
        if (attributes)
            ak_buf_add(&b, "\"amiga\":%s,", attributes);
        free(attributes);
        if (ak_equal(&j, ak_member(&j, (int)i, "type"), "directory"))
            ak_buf_add(
                &b, "\"mode\":\"%s\",\"path\":\"%s\",\"type\":\"directory\"}",
                fm, path);
        else {
            if (ak_string(&j, ak_member(&j, (int)i, "sha256"), sha,
                          sizeof sha) ||
                ak_string(&j, ak_member(&j, (int)i, "size"), size,
                          sizeof size)) {
                code = APKG_INVALID;
                goto done;
            }
            unsigned char digest[32];
            uint64_t n;
            if (ak_hex(sha, digest, 32) ||
                ak_u64(&j, ak_member(&j, (int)i, "size"), &n)) {
                code = APKG_INVALID;
                goto done;
            }
            if (data < 0 && !ak_equal(&j, ak_member(&j, (int)i, "blob"), sha)) {
                code = APKG_INVALID;
                goto done;
            }
            if (data >= 0) {
                if (n > AK_MAX || j.tokens[data].type != '"' ||
                    j.tokens[data].end - j.tokens[data].start - 2 != n * 2) {
                    code = APKG_INVALID;
                    goto done;
                }
                struct pkg_sha256 hash;
                unsigned char chunk[1024]; /* small: AROS stacks have no guard page */
                uint64_t pos = 0;
                char actual[65];
                pkg_sha256_init(&hash);
                while (pos < n) {
                    size_t take = n - pos > sizeof chunk ? sizeof chunk
                                                         : (size_t)(n - pos);
                    for (size_t x = 0; x < take; x++) {
                        size_t at =
                            j.tokens[data].start + 1 + (size_t)(pos + x) * 2;
                        char hex[3] = {j.text[at], j.text[at + 1], 0};
                        if (ak_hex(hex, &chunk[x], 1)) {
                            code = APKG_INVALID;
                            goto done;
                        }
                    }
                    pkg_sha256_update(&hash, chunk, take);
                    pos += take;
                }
                pkg_sha256_final(&hash, digest);
                ak_tohex(digest, 32, actual);
                if (strcmp(actual, sha)) {
                    code = APKG_INTEGRITY;
                    goto done;
                }
            }
            ak_buf_add(&b,
                       "\"blob\":\"%s\",\"mode\":\"%s\",\"path\":\"%s\","
                       "\"sha256\":\"%s\",\"size\":\"%s\",\"type\":\"file\"}",
                       sha, fm, path, sha, size);
        }
    }
    ak_buf_add(&b, "],\"mode\":\"%s\"}", mode);
    code = (apkg_status)b.bad;
    if (!code) {
        *out = b.data;
        b.data = NULL;
    }
done:
    free(b.data);
    ak_json_free(&j);
    if (code == APKG_NOMEM)
        errno = ENOMEM;
    return code;
}
