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
/* The current tree representation cannot round-trip links, special mode bits,
 * foreign ownership, ACLs, AROS comments/protection or extended attributes.
 * Refuse such a participant before writes instead of stripping metadata. */
static int metadata_supported(const char *path, const struct stat *st) {
    unsigned long long protection = 0;
    char comment[256];
    if (st->st_mode & 07000)
        return 0;
#ifndef __AROS__
    if (st->st_uid != geteuid() || st->st_gid != getegid())
        return 0;
#endif
    int amiga = pkg_fs_amiga_get(path, &protection, comment, sizeof comment);
    if (amiga < 0 || (amiga > 0 && (protection || *comment)))
        return 0;
#ifdef __APPLE__
    char attributes[256];
    ssize_t attr_size =
        listxattr(path, attributes, sizeof attributes, XATTR_NOFOLLOW);
    /* macOS attaches its own process provenance to newly created files. It is
     * OS bookkeeping, excluded from managed package metadata on this host. */
    if (attr_size != 0 &&
        !(attr_size == 21 && !memcmp(attributes, "com.apple.provenance", 21)))
        return 0;
    acl_t acl = acl_get_file(path, ACL_TYPE_EXTENDED);
    if (acl) {
        acl_entry_t entry;
        int present = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
        acl_free(acl);
        if (present == 0)
            return 0;
    }
#elif defined(__linux__)
    ssize_t attributes = llistxattr(path, NULL, 0);
    if (attributes > 0 || (attributes < 0 && errno != ENOTSUP))
        return 0;
#endif
    return 1;
}
static int names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static int scan(const char *base, const char *rel, ak_buf *out,
                unsigned depth) {
    char path[AK_PATH];
    DIR *d;
    struct dirent *e;
    char **list = NULL;
    size_t n = 0, i;
    int bad = 0;
    if (depth > 32 ||
        (*rel ? ak_path(path, base, rel) : (strcpy(path, base), 0)))
        return APKG_LIMIT;
    d = opendir(path);
    if (!d)
        return APKG_IO;
    errno = 0;
    while ((e = readdir(d))) {
        char **next;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (n >= AK_TOKENS || n >= out->max / sizeof(ak_tree_row)) {
            bad = APKG_LIMIT;
            break;
        }
        next = malloc((n + 1) * sizeof *next);
        if (!next) {
            bad = APKG_NOMEM;
            break;
        }
        if (n)
            memcpy(next, list, n * sizeof *next);
        free(list);
        list = next;
        list[n] = ak_copy(e->d_name, strlen(e->d_name));
        if (!list[n]) {
            bad = APKG_NOMEM;
            break;
        }
        n++;
        errno = 0;
    }
    if (errno && !bad)
        bad = APKG_IO;
    closedir(d);
    if (n > 1)
        qsort(list, n, sizeof *list, names);
    for (i = 0; i < n && !bad; i++) {
        char sub[AK_PATH], full[AK_PATH];
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
        if (S_ISDIR(st.st_mode)) {
            ak_buf_add(
                out,
                "%s{\"mode\":\"%u\",\"path\":\"%s\",\"type\":\"directory\"}",
                out->size ? "," : "", (unsigned)(st.st_mode & 0777), sub);
            bad = scan(base, sub, out, depth + 1);
        } else if (S_ISREG(st.st_mode) && st.st_nlink == 1) {
            uint64_t len = 0;
            unsigned char head[64];
            size_t hn = 0;
            char sha[65];
            bad = ak_stream_hash(full, &len, sha, head, &hn);
            if (bad)
                break;
            ak_buf_add(out,
                       "%s{\"blob\":\"%s\",\"mode\":\"%u\",\"path\":\"%s\","
                       "\"sha256\":\"%s\",\"size\":\"%" PRIu64
                       "\",\"type\":\"file\"}",
                       out->size ? "," : "", sha, (unsigned)(st.st_mode & 0777),
                       sub, sha, len);

        } else
            bad = APKG_UNSUPPORTED;
        if (out->bad)
            bad = out->bad;
    }
    for (i = 0; i < n; i++)
        free(list[i]);
    free(list);
    return bad;
}
/* Entries are globally sorted after the walk: DFS order differs for a.b/a/x. */
static int records(const void *a, const void *b) {
    return strcmp(((const ak_tree_row *)a)->path,
                  ((const ak_tree_row *)b)->path);
}
apkg_status ak_tree_read(const char *base, size_t max, char **out,
                         struct stat *identity) {
    ak_buf files = {0}, result = {0};
    ak_json j = {0};
    ak_tree_row *rows = NULL;
    size_t count = 0, i, k = 0;
    int code;
    *out = NULL;
    files.max = result.max = max;
    if (lstat(base, identity)) {
        if (errno == ENOENT) {
            *out = ak_copy("null", 4);
            return *out ? APKG_OK : APKG_NOMEM;
        }
        return APKG_IO;
    }
    if (!S_ISDIR(identity->st_mode) || !metadata_supported(base, identity))
        return APKG_UNSUPPORTED;
    code = scan(base, "", &files, 0);
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
        if (ak_string(&j, ak_member(&j, (int)i, "path"), rows[k].path,
                      AK_PATH)) {
            code = APKG_INVALID;
            goto done;
        }
        rows[k++].token = (int)i;
    }
    if (count > 1)
        qsort(rows, count, sizeof *rows, records);
    free(files.data);
    memset(&files, 0, sizeof files);
    files.max = max;
    ak_buf_add(&files, "{\"files\":[");
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
    free(rows);
    free(files.data);
    free(result.data);
    ak_json_free(&j);
    if (code == APKG_NOMEM)
        errno = ENOMEM;
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
    if (!ak_fields(&j, 0, (const char *const[]){"files", "mode"}, 2) ||
        arr < 0 || j.tokens[arr].type != '[' ||
        ak_u64(&j, ak_member(&j, 0, "mode"), &permissions) ||
        permissions > 0777 ||
        ak_string(&j, ak_member(&j, 0, "mode"), mode, sizeof mode)) {
        code = APKG_INVALID;
        goto done;
    }
    ak_buf_add(&b, "{\"files\":[");
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
        if (!ak_fields(&j, (int)i,
                       directory   ? dir_fields
                       : data >= 0 ? data_fields
                                   : blob_fields,
                       directory ? 3 : 6) ||
            (!directory &&
             !ak_equal(&j, ak_member(&j, (int)i, "type"), "file"))) {
            code = APKG_INVALID;
            goto done;
        }
        if (ak_equal(&j, ak_member(&j, (int)i, "type"), "directory"))
            ak_buf_add(
                &b,
                "%s{\"mode\":\"%s\",\"path\":\"%s\",\"type\":\"directory\"}",
                i == (size_t)arr + 1 ? "" : ",", fm, path);
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
                unsigned char chunk[65536];
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
                       "%s{\"blob\":\"%s\",\"mode\":\"%s\",\"path\":\"%s\","
                       "\"sha256\":\"%s\",\"size\":\"%s\",\"type\":\"file\"}",
                       i == (size_t)arr + 1 ? "" : ",", sha, fm, path, sha,
                       size);
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
