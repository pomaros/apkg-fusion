/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include "pkg_application.h"
#include "pkg_target.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
int ak_valid_name(const char *s) {
    size_t i, n = strlen(s);
    if (!n || n > 255 || !strcmp(s, "db") || !isalnum((unsigned char)*s))
        return 0;
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') ||
              strchr("._-", s[i])))
            return 0;
    return 1;
}
int ak_valid_path(const char *s) {
    size_t i, components = 1, n = strlen(s);
    if (!n || n >= AK_PATH || s[0] == '/' || s[n - 1] == '/')
        return 0;
    for (i = 0; i < n; i++) {
        if (s[i] == '/' && ++components > 32)
            return 0;
    }
    for (i = 0; i < n; i++)
        if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126 ||
            s[i] == '\\' || s[i] == ':' || s[i] == '"')
            return 0;
    for (i = 0; i < n;) {
        size_t a = i;
        while (i < n && s[i] != '/')
            i++;
        if (i == a || (i - a == 1 && s[a] == '.') ||
            (i - a == 2 && s[a] == '.' && s[a + 1] == '.'))
            return 0;
        if (i < n)
            i++;
    }
    return 1;
}
int ak_policy_valid(const ak_json *j) {
    int b, arr;
    size_t i;
    char last[33] = "", id[33];
    static const char *const bf[] = {"authorities"},
                             *const af[] = {"id", "keys"},
                             *const kf[] = {"algorithm", "public_key"};
    if (!ak_object(j, "policy"))
        return 0;
    b = ak_member(j, 0, "body");
    if (!ak_fields(j, b, bf, 1))
        return 0;
    arr = ak_member(j, b, "authorities");
    if (arr < 0 || j->tokens[arr].type != '[')
        return 0;
    for (i = (size_t)arr + 1; i < j->tokens[arr].next; i = j->tokens[i].next) {
        int keys;
        size_t k;
        char prev[65] = "", key[65];
        unsigned char bytes[32];
        if (!ak_fields(j, (int)i, af, 2) ||
            ak_string(j, ak_member(j, (int)i, "id"), id, sizeof id) ||
            ak_hex(id, bytes, 16) || (*last && strcmp(last, id) >= 0))
            return 0;
        strcpy(last, id);
        keys = ak_member(j, (int)i, "keys");
        if (keys < 0 || j->tokens[keys].type != '[' || ak_empty(j, keys))
            return 0;
        for (k = (size_t)keys + 1; k < j->tokens[keys].next;
             k = j->tokens[k].next) {
            if (!ak_fields(j, (int)k, kf, 2) ||
                !ak_equal(j, ak_member(j, (int)k, "algorithm"), "ed25519") ||
                ak_string(j, ak_member(j, (int)k, "public_key"), key,
                          sizeof key) ||
                ak_hex(key, bytes, 32) || (*prev && strcmp(prev, key) >= 0))
                return 0;
            strcpy(prev, key);
        }
    }
    return 1;
}
int ak_policy(const ak_json *j, int unused, const char *authority,
              const char *key) {
    int arr;
    size_t i;
    (void)unused;
    if (!ak_policy_valid(j))
        return 0;
    arr = ak_member(j, ak_member(j, 0, "body"), "authorities");
    for (i = (size_t)arr + 1; i < j->tokens[arr].next; i = j->tokens[i].next)
        if (ak_equal(j, ak_member(j, (int)i, "id"), authority)) {
            int ks = ak_member(j, (int)i, "keys");
            size_t k;
            for (k = (size_t)ks + 1; k < j->tokens[ks].next;
                 k = j->tokens[k].next)
                if (ak_equal(j, ak_member(j, (int)k, "public_key"), key))
                    return 1;
        }
    return 0;
}
static int compare_paths(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static int path_set_valid(const ak_json *j, int arr) {
    size_t n = 0, i, k = 0;
    char **original = NULL, **folded = NULL;
    unsigned char *directory = NULL;
    int valid = 0, oom = 0;
    for (i = (size_t)arr + 1; i < j->tokens[arr].next; i = j->tokens[i].next)
        n++;
    if (!n)
        return 1;
    original = calloc(n, sizeof *original);
    folded = calloc(n, sizeof *folded);
    directory = calloc(n, 1);
    if (!original || !folded || !directory) {
        oom = 1;
        goto done;
    }
    for (i = (size_t)arr + 1; i < j->tokens[arr].next; i = j->tokens[i].next) {
        char path[AK_PATH];
        size_t x;
        if (ak_string(j, ak_member(j, (int)i, "path"), path, sizeof path))
            goto done;
        original[k] = ak_copy(path, strlen(path));
        folded[k] = ak_copy(path, strlen(path));
        if (!original[k] || !folded[k]) {
            oom = 1;
            goto done;
        }
        for (x = 0; folded[k][x]; x++)
            folded[k][x] = (char)tolower((unsigned char)folded[k][x]);
        directory[k] = (unsigned char)ak_equal(j, ak_member(j, (int)i, "type"),
                                               "directory");
        k++;
    }
    for (i = 0; i < n; i++) {
        char prefix[AK_PATH];
        size_t x;
        strcpy(prefix, original[i]);
        for (x = 0; prefix[x]; x++)
            if (prefix[x] == '/') {
                char *key = prefix;
                char **found;
                prefix[x] = 0;
                found =
                    bsearch(&key, original, n, sizeof *original, compare_paths);
                if (!found || !directory[(size_t)(found - original)])
                    goto done;
                prefix[x] = '/';
            }
    }
    qsort(folded, n, sizeof *folded, compare_paths);
    for (i = 1; i < n; i++)
        if (!strcmp(folded[i - 1], folded[i]))
            goto done;
    valid = 1;
done:
    for (i = 0; i < n; i++) {
        if (original)
            free(original[i]);
        if (folded)
            free(folded[i]);
    }
    free(original);
    free(folded);
    free(directory);
    if (oom)
        errno = ENOMEM;
    return valid;
}
int ak_descriptor(const ak_json *j) {
    static const char *const bodyf[] = {"authority", "files",  "kind",   "name",
                                        "revision",  "target", "version"};
    static const char *const filef[] = {"data",   "mode", "path",
                                        "sha256", "size", "type"},
                             *const dirf[] = {"path", "type"};
    int b, arr;
    size_t i;
    char s[4096], previous[4096] = "";
    unsigned char id[32];
    uint64_t v;
    if (!ak_object(j, "descriptor"))
        return 0;
    b = ak_member(j, 0, "body");
    if ((!ak_fields(j, b, bodyf, 7) &&
         !ak_fields(j, b,
                    (const char *const[]){"authority", "files", "kind", "name",
                                          "relations", "revision", "target",
                                          "version"},
                    8)) ||
        !ak_relations_valid(j, ak_member(j, b, "relations")))
        return 0;
    if (ak_string(j, ak_member(j, b, "authority"), s, sizeof s) ||
        ak_hex(s, id, 16) || ak_string(j, ak_member(j, b, "name"), s, 256) ||
        !ak_valid_name(s) || ak_string(j, ak_member(j, b, "version"), s, 256) ||
        !*s)
        return 0;
    for (i = 0; s[i]; i++)
        if (!(isalnum((unsigned char)s[i]) || strchr("._-+", s[i])))
            return 0;
    if (ak_u64(j, ak_member(j, b, "revision"), &v) || !v ||
        ak_string(j, ak_member(j, b, "target"), s, sizeof s) ||
        pkg_target_parse(s, NULL, NULL, 0))
        return 0;
    if (!ak_equal(j, ak_member(j, b, "kind"), "data") &&
        !ak_equal(j, ak_member(j, b, "kind"), "application") &&
        !ak_equal(j, ak_member(j, b, "kind"), "tool"))
        return 0;
    arr = ak_member(j, b, "files");
    if (arr < 0 || j->tokens[arr].type != '[')
        return 0;
    for (i = (size_t)arr + 1; i < j->tokens[arr].next; i = j->tokens[i].next) {
        int data;
        size_t n;
        unsigned char *bytes;
        apkg_digest hash;
        char hex[65];
        uint64_t mode;
        if (ak_string(j, ak_member(j, (int)i, "path"), s, sizeof s) ||
            !ak_valid_path(s) || (*previous && strcmp(previous, s) >= 0))
            return 0;
        strcpy(previous, s);
        if (ak_equal(j, ak_member(j, (int)i, "type"), "directory")) {
            if (!ak_fields(j, (int)i, dirf, 2))
                return 0;
            continue;
        }
        if ((!ak_fields(j, (int)i, filef, 6) &&
             !(ak_member(j, (int)i, "config") >= 0 &&
               ak_equal(j, ak_member(j, (int)i, "config"), "preserve") &&
               ak_fields(j, (int)i,
                         (const char *const[]){"config", "data", "mode", "path",
                                               "sha256", "size", "type"},
                         7))) ||
            !ak_equal(j, ak_member(j, (int)i, "type"), "file") ||
            ak_u64(j, ak_member(j, (int)i, "size"), &v) || v > AK_MAX ||
            ak_u64(j, ak_member(j, (int)i, "mode"), &mode) ||
            (mode != 420 && mode != 493))
            return 0;
        data = ak_member(j, (int)i, "data");
        if (data < 0 || j->tokens[data].type != '"')
            return 0;
        n = j->tokens[data].end - j->tokens[data].start - 2;
        if (n != v * 2)
            return 0;
        {
            char *h = ak_copy(j->text + j->tokens[data].start + 1, n);
            bytes = malloc((size_t)v + 1);
            if (!h || !bytes) {
                free(h);
                free(bytes);
                errno = ENOMEM;
                return 0;
            }
            if (ak_hex(h, bytes, (size_t)v)) {
                free(h);
                free(bytes);
                return 0;
            }
            free(h);
        }
        ak_hash(bytes, (size_t)v, &hash);
        ak_tohex(hash.bytes, 32, hex);
        if (!ak_equal(j, ak_member(j, (int)i, "sha256"), hex)) {
            errno = EIO;
            free(bytes);
            return 0;
        }
        {
            const char *cpu = pkg_application_binary_arch(bytes, (size_t)v);
            char target[32];
            ak_string(j, ak_member(j, b, "target"), target, sizeof target);
            if (*cpu && !pkg_target_cpu_matches(target, cpu)) {
                errno = ENOEXEC;
                free(bytes);
                return 0;
            }
        }
        free(bytes);
    }
    return path_set_valid(j, arr);
}
