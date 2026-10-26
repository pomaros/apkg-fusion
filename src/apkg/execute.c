/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "internal.h"
#include "pkg_fs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef __AROS__
#include <sys/file.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#else
#include <proto/dos.h>
#endif
#ifdef APKG_TESTING
static int test_point, test_action;
void apkg_test_fault(int point, int action) {
    test_point = point;
    test_action = action;
}
int ak_fault(int point) {
    if (point != test_point)
        return 0;
    if (test_action == 2)
        _exit(99);
    errno = EIO;
    return test_action == 1;
}
#else
int ak_fault(int point) {
    (void)point;
    return 0;
}
#endif
static int parent_path(const char *root, char *out) {
    char *p;
    strcpy(out, root);
    p = strrchr(out, '/');
    if (p) {
        if (p == out)
            p[1] = 0;
        else
            *p = 0;
        return 0;
    }
    p = strchr(out, ':');
    if (p) {
        p[1] = 0;
        return 0;
    }
    return -1;
}
int ak_publish(const char *from, const char *to) {
#ifdef __APPLE__
    return renamex_np(from, to, RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
    return (int)syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, 1u);
#elif defined(__AROS__)
    if (Rename((CONST_STRPTR)from, (CONST_STRPTR)to))
        return 0;
    errno = EIO;
    return -1;
#else
    (void)from;
    (void)to;
    errno = ENOTSUP;
    return -1;
#endif
}
typedef struct {
    int fd;
    void *aros;
    char parent[AK_PATH];
} mutation_lock;
static int lock_take(apkg_ctx *c, mutation_lock *l) {
    memset(l, 0, sizeof *l);
    l->fd = -1;
    if (parent_path(c->root, l->parent))
        return -1;
#ifdef __AROS__
    {
        int busy = 0;
        l->aros = pkg_fs_lock_root(l->parent, &busy);
        return l->aros ? 0 : busy ? 1 : -1;
    }
#else
    l->fd = open(l->parent, O_RDONLY);
    if (l->fd < 0)
        return -1;
    if (flock(l->fd, LOCK_EX | LOCK_NB)) {
        int busy = errno == EWOULDBLOCK;
        close(l->fd);
        l->fd = -1;
        return busy ? 1 : -1;
    }
    return 0;
#endif
}
static void lock_drop(mutation_lock *l) {
#ifdef __AROS__
    pkg_fs_unlock_root(l->aros);
#else
    if (l->fd >= 0) {
        flock(l->fd, LOCK_UN);
        close(l->fd);
    }
#endif
}
static int paths(apkg_ctx *c, char *txn, char *state) {
    char meta[AK_PATH];
    int n = snprintf(txn, AK_PATH, "%s.apkg-transaction", c->root);
    return n < 0 || n >= AK_PATH || ak_path(meta, c->root, ".apkg") ||
           ak_path(state, meta, "state");
}
static int write_in(const char *base, const char *name, const void *p,
                    size_t n) {
    char path[AK_PATH];
    return ak_path(path, base, name) || ak_write(path, p, n, 1);
}
static int read_in_limit(const char *base, const char *name, unsigned char **p,
                         size_t *n, size_t max) {
    char path[AK_PATH];
    return ak_path(path, base, name) || ak_read(path, p, n, max);
}
static int marker(const char *base, const char *name) {
    return write_in(base, name, "", 0) || ak_sync_dir(base);
}
static int has(const char *base, const char *name) {
    char path[AK_PATH];
    if (ak_path(path, base, name))
        return -1;
    return ak_exists(path);
}
static int mkdir_parents(const char *base, const char *rel) {
    char path[AK_PATH];
    size_t i;
    if (ak_path(path, base, rel))
        return -1;
    for (i = strlen(base) + 1; path[i]; i++)
        if (path[i] == '/') {
            struct stat s;
            path[i] = 0;
            if (lstat(path, &s)) {
                if (errno != ENOENT || mkdir(path, 0700))
                    return -1;
            } else if (!S_ISDIR(s.st_mode))
                return -1;
            path[i] = '/';
        }
    return 0;
}
static int stage_attributes(const char *path, const ak_json *j, int entry,
                            uint64_t mode) {
    int at = ak_member(j, entry, "amiga");
    if (at < 0)
        return 0;
    char *raw = ak_slice(j, at);
    if (!raw) {
        errno = ENOMEM;
        return -1;
    }
    apkg_status code = ak_attributes_write(path, raw, mode);
    free(raw);
    if (code) {
        errno = code == APKG_NOMEM         ? ENOMEM
                : code == APKG_UNSUPPORTED ? ENOTSUP
                                           : EIO;
        return -1;
    }
    return 0;
}
static int stage_package(apkg_ctx *c, const char *tree, const ak_json *j,
                         const char *fallback) {
    int body = ak_member(j, 0, "body");
    int arr = ak_member(j, body < 0 ? 0 : body, "files");
    size_t i;
    for (i = (size_t)arr + 1; i < j->tokens[arr].next; i = j->tokens[i].next) {
        char rel[AK_PATH], path[AK_PATH];
        uint64_t n, mode;
        int data;
        unsigned char *bytes;
        char *hex;
        if (ak_string(j, ak_member(j, (int)i, "path"), rel, sizeof rel) ||
            mkdir_parents(tree, rel) || ak_path(path, tree, rel))
            return -1;
        if (ak_equal(j, ak_member(j, (int)i, "type"), "directory")) {
            if (mkdir(path, 0700) && errno != EEXIST)
                return -1;
            continue;
        }
        if (ak_u64(j, ak_member(j, (int)i, "size"), &n) ||
            ak_u64(j, ak_member(j, (int)i, "mode"), &mode))
            return -1;
        int blob = ak_member(j, (int)i, "blob");
        if (blob >= 0) {
            char sha[65], source[AK_PATH];
            const char *origin = NULL;
            if (ak_string(j, blob, sha, sizeof sha))
                return -1;
            if (fallback) {
                if (ak_path(source, fallback, rel))
                    return -1;
                origin = source;
            }
            apkg_status code = ak_blob_copy(c, sha, n, origin, path);
            if (code) {
                errno = code == APKG_NOMEM       ? ENOMEM
                        : code == APKG_NOT_FOUND ? ENOENT
                        : code == APKG_INTEGRITY ? EILSEQ
                        : code == APKG_TARGET    ? ENOEXEC
                                                 : EIO;
                return -1;
            }
            if (chmod(path, (mode_t)mode) ||
                stage_attributes(path, j, (int)i, mode))
                return -1;
            int fd = open(path, O_RDONLY);
            if (fd < 0)
                return -1;
            int flushed = fsync(fd);
            int closed = close(fd);
            if (flushed || closed)
                return -1;
            continue;
        }
        data = ak_member(j, (int)i, "data");
        hex = ak_copy(j->text + j->tokens[data].start + 1, (size_t)n * 2);
        bytes = malloc((size_t)n + 1);
        if (!hex || !bytes) {
            free(hex);
            free(bytes);
            return -1;
        }
        if (ak_hex(hex, bytes, (size_t)n) ||
            ak_write(path, bytes, (size_t)n, 1) || chmod(path, (mode_t)mode) ||
            stage_attributes(path, j, (int)i, mode)) {
            free(hex);
            free(bytes);
            return -1;
        }
        free(hex);
        free(bytes);
        /* chmod metadata and file bytes must be flushed before publication. */
        {
            int fd = open(path, O_RDONLY);
            if (fd < 0)
                return -1;
            if (fsync(fd)) {
                close(fd);
                return -1;
            }
            close(fd);
        }
    }
    return ak_sync_dir(tree);
}
static int known_path(const ak_json *j, const char *rel, int directory) {
    int arr = ak_member(j, ak_member(j, 0, "body"), "files");
    size_t i, n = strlen(rel);
    for (i = (size_t)arr + 1; i < j->tokens[arr].next; i = j->tokens[i].next) {
        char path[AK_PATH];
        ak_string(j, ak_member(j, (int)i, "path"), path, sizeof path);
        if (!strcmp(path, rel))
            return directory
                       ? ak_equal(j, ak_member(j, (int)i, "type"), "directory")
                       : ak_equal(j, ak_member(j, (int)i, "type"), "file");
        if (directory && !strncmp(path, rel, n) && path[n] == '/')
            return 1;
    }
    return 0;
}
static int walk_known(const char *base, const char *prefix, const ak_json *j) {
    char path[AK_PATH];
    DIR *d;
    struct dirent *e;
    int result = 0;
    if (*prefix) {
        if (ak_path(path, base, prefix))
            return -1;
    } else
        strcpy(path, base);
    d = opendir(path);
    if (!d)
        return -1;
    errno = 0;
    while ((e = readdir(d))) {
        char rel[AK_PATH], full[AK_PATH];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (*prefix) {
            if (ak_path(rel, prefix, e->d_name)) {
                result = -1;
                break;
            }
        } else {
            if (strlen(e->d_name) >= sizeof rel) {
                result = -1;
                break;
            }
            strcpy(rel, e->d_name);
        }
        if (ak_path(full, base, rel) || lstat(full, &st) ||
            S_ISLNK(st.st_mode) || !known_path(j, rel, S_ISDIR(st.st_mode)) ||
            (S_ISDIR(st.st_mode) && walk_known(base, rel, j))) {
            result = -1;
            break;
        }
        errno = 0;
    }
    if (errno)
        result = -1;
    closedir(d);
    return result;
}
static int verify_tree(const char *tree, const ak_json *j) {
    int arr = ak_member(j, ak_member(j, 0, "body"), "files");
    size_t i;
    struct stat root;
    if (lstat(tree, &root) || !S_ISDIR(root.st_mode) || walk_known(tree, "", j))
        return -1;
    for (i = (size_t)arr + 1; i < j->tokens[arr].next; i = j->tokens[i].next) {
        char rel[AK_PATH], path[AK_PATH], hex[65];
        uint64_t n = 0, mode;
        unsigned char head[64];
        size_t hn = 0;
        struct stat st;
        ak_string(j, ak_member(j, (int)i, "path"), rel, sizeof rel);
        if (ak_path(path, tree, rel) || lstat(path, &st))
            return -1;
        if (ak_equal(j, ak_member(j, (int)i, "type"), "directory")) {
            if (!S_ISDIR(st.st_mode))
                return -1;
            continue;
        }
        uint64_t expected_size;
        if (ak_u64(j, ak_member(j, (int)i, "size"), &expected_size) ||
            ak_stream_hash(path, &n, hex, head, &hn) != APKG_OK ||
            n != expected_size)
            return -1;
        if (!ak_equal(j, ak_member(j, (int)i, "sha256"), hex) ||
            ak_u64(j, ak_member(j, (int)i, "mode"), &mode) ||
            (st.st_mode & 0777) != (mode_t)mode)
            return -1;
    }
    return 0;
}
static int tree_identity(const char *txn, const char *tree, int save) {
    char text[128];
    struct stat st;
    unsigned char *bytes = NULL;
    size_t n;
    int len, result;
    if (lstat(tree, &st) || !S_ISDIR(st.st_mode))
        return -1;
    len =
        snprintf(text, sizeof text,
                 "%"
                 "llu"
                 ":%"
                 "llu",
                 (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
    if (save)
        return write_in(txn, "tree-id", text, (size_t)len);
    if (read_in_limit(txn, "tree-id", &bytes, &n, 128))
        return -1;
    result = n == (size_t)len && !memcmp(text, bytes, n) ? 0 : -1;
    free(bytes);
    return result;
}
static int sync_tree(const char *path) {
    DIR *d = opendir(path);
    struct dirent *e;
    int bad = 0;
    if (!d)
        return -1;
    errno = 0;
    while ((e = readdir(d))) {
        char p[AK_PATH];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (ak_path(p, path, e->d_name) || lstat(p, &st)) {
            bad = 1;
            break;
        }
        if (S_ISDIR(st.st_mode) && sync_tree(p)) {
            bad = 1;
            break;
        }
        errno = 0;
    }
    if (errno)
        bad = 1;
    closedir(d);
    return bad ? -1 : ak_sync_dir(path);
}
static int state_publish(const char *state, const char *txn, size_t max) {
    unsigned char *bytes = NULL;
    size_t n;
    char next[AK_PATH], meta[AK_PATH];
    int r;
    if (read_in_limit(txn, "new-state", &bytes, &n, max))
        return -1;
    strcpy(meta, state);
    *strrchr(meta, '/') = 0;
    if (ak_path(next, meta, "state-next")) {
        free(bytes);
        return -1;
    }
    if (pkg_fs_path_is_link(next) != 0) {
        free(bytes);
        return -1;
    }
    r = ak_write(next, bytes, n, ak_exists(next) == 0);
    free(bytes);
    if (r)
        return -1;
#ifdef __AROS__
    {
        struct pkg_fs_id id;
        unsigned char *data = NULL;
        size_t len;
        if (pkg_fs_identity(state, &id) || ak_read(next, &data, &len, AK_MAX))
            return -1;
        r = pkg_fs_replace_if_same(state, &id, data, len);
        free(data);
        if (r)
            return -1;
        if (unlink(next))
            return -1;
    }
#else
    if (rename(next, state))
        return -1;
#endif
    return ak_sync_dir(meta);
}
static int exact_file(const char *path, const unsigned char *expected,
                      size_t n) {
    unsigned char *bytes = NULL;
    size_t len;
    int r;
    if (ak_read(path, &bytes, &len, n))
        return -1;
    r = len == n && !memcmp(bytes, expected, n) ? 0 : -1;
    free(bytes);
    return r;
}
static void result_set(apkg_result *r, apkg_status code, apkg_outcome outcome) {
    r->view.status = code;
    r->view.outcome = outcome;
}
static int emit(const apkg_observer *, apkg_result *, uint64_t *, apkg_phase,
                int, const char *);
#include "changes.inc"
/* Caller holds the parent lock. No parser accepts an arbitrary write path. */
static apkg_status recover_locked(apkg_ctx *c, apkg_result *result) {
    char txn[AK_PATH], state[AK_PATH], tree[AK_PATH], live[AK_PATH], name[256],
        root_hex[AK_PATH * 2 + 1], expected_root[AK_PATH * 2 + 1], before[65],
        sha[65], actual[65];
    unsigned char *raw = NULL, *newstate = NULL, *oldstate = NULL,
                  *recorded_sha = NULL;
    size_t n = 0, nn = 0, on = 0, sn = 0;
    ak_json plan = {0}, newjson = {0}, descriptor = {0};
    int body, init, decision;
    apkg_digest hash;
    apkg_status r = APKG_RECOVERY_REQUIRED;
    if (paths(c, txn, state))
        return APKG_LIMIT;
    strcpy(result->record, txn);
    result->view.record.data = (unsigned char *)result->record;
    result->view.record.size = strlen(txn);
    if (ak_exists(txn) == 0) {
        apkg_snapshot *inspection = NULL;
        apkg_status inspected = apkg_snapshot_read(c, &inspection, NULL);
        if (inspected != APKG_OK) {
            result_set(result, inspected, APKG_UNCHANGED);
            return inspected;
        }
        if (inspection->view.state == APKG_ROOT_PENDING ||
            inspection->view.state == APKG_ROOT_LEGACY ||
            inspection->view.state == APKG_ROOT_INVALID) {
            apkg_snapshot_free(inspection);
            result_set(result, APKG_UNSUPPORTED, APKG_NEEDS_RECOVERY);
            return APKG_UNSUPPORTED;
        }
        apkg_snapshot_free(inspection);
        result_set(result, APKG_OK, APKG_UNCHANGED);
        return APKG_OK;
    }
    if (pkg_fs_path_is_link(txn) != 0 || has(txn, "ready") != 1 ||
        read_in_limit(txn, "plan", &raw, &n,
                      c->object_max < c->memory_max / 32
                          ? c->object_max
                          : c->memory_max / 32) ||
        read_in_limit(txn, "plan-sha", &recorded_sha, &sn, 64) || sn != 64 ||
        ak_json_parse(raw, n, &plan) || !ak_object(&plan, "plan"))
        goto done;
    ak_hash(raw, n, &hash);
    ak_tohex(hash.bytes, 32, sha);
    if (memcmp(sha, recorded_sha, 64))
        goto done;
    body = ak_member(&plan, 0, "body");
    if (ak_equal(&plan, ak_member(&plan, body, "action"), "changes")) {
        r = recover_changes_locked(c, result, txn, &plan);
        goto done;
    }
    {
        static const char *const f[] = {"action",    "anchor_dev", "anchor_ino",
                                        "before",    "generation", "new_state",
                                        "root_path", "target_path"};
        uint64_t dev, ino;
        struct stat st;
        char parent[AK_PATH];
        if (!ak_fields(&plan, body, f, 8) ||
            ak_u64(&plan, ak_member(&plan, body, "anchor_dev"), &dev) ||
            ak_u64(&plan, ak_member(&plan, body, "anchor_ino"), &ino))
            goto done;
        if (stat(c->root, &st) || (uint64_t)st.st_dev != dev ||
            (uint64_t)st.st_ino != ino) {
            if (parent_path(c->root, parent) || stat(parent, &st) ||
                (uint64_t)st.st_dev != dev || (uint64_t)st.st_ino != ino)
                goto done;
        }
    }
    {
        unsigned char *id = NULL;
        size_t id_size;
        if (read_in_limit(txn, "archive-id", &id, &id_size, 32) ||
            id_size != 32 ||
            ak_hex((char *)id, result->view.operation_id.bytes, 16)) {
            free(id);
            goto done;
        }
        free(id);
    }
    ak_tohex((unsigned char *)c->root, strlen(c->root), expected_root);
    if (ak_string(&plan, ak_member(&plan, body, "root_path"), root_hex,
                  sizeof root_hex) ||
        strcmp(root_hex, expected_root) ||
        ak_string(&plan, ak_member(&plan, body, "target_path"), name,
                  sizeof name) ||
        ak_string(&plan, ak_member(&plan, body, "before"), before,
                  sizeof before))
        goto done;
    init = ak_equal(&plan, ak_member(&plan, body, "action"), "initialize");
    if (!init && !ak_equal(&plan, ak_member(&plan, body, "action"), "install"))
        goto done;
    if ((init && *name) || (!init && !ak_valid_name(name)) ||
        read_in_limit(txn, "new-state", &newstate, &nn,
                      c->object_max < c->memory_max / 32
                          ? c->object_max
                          : c->memory_max / 32) ||
        ak_json_parse(newstate, nn, &newjson) || !ak_object(&newjson, "root"))
        goto done;
    {
        int ns = ak_member(&plan, body, "new_state");
        if (ns < 0 || plan.tokens[ns].end - plan.tokens[ns].start != nn ||
            memcmp(plan.text + plan.tokens[ns].start, newstate, nn))
            goto done;
    }
    decision = has(txn, "decision");
    if (decision < 0)
        goto done;
    if (ak_path(tree, txn, "tree"))
        goto done;
    if (ak_fault(6))
        goto done;
    if (!ak_physical(c->anchor, &c->binding))
        goto done;
    if (init) {
        char meta[AK_PATH];
        if (ak_path(meta, c->root, ".apkg"))
            goto done;
        if (!decision) {
            if (ak_exists(meta) != 0)
                goto done; /* nothing managed was published before init decision
                            */
        } else if (exact_file(state, newstate, nn) ==
                   0) { /* already committed */
        } else if (ak_exists(c->root) == 0) {
            if (tree_identity(txn, tree, 0) || ak_publish(tree, c->root))
                goto done;
        } else {
            char staged_meta[AK_PATH];
            if (ak_exists(meta) != 0 || ak_path(staged_meta, tree, ".apkg") ||
                tree_identity(txn, tree, 0) || ak_publish(staged_meta, meta))
                goto done;
        }
    } else {
        int installed, files;
        size_t i, last = 0;
        if (read_in_limit(txn, "old-state", &oldstate, &on,
                          c->object_max < c->memory_max / 32
                              ? c->object_max
                              : c->memory_max / 32))
            goto done;
        ak_hash(oldstate, on, &hash);
        ak_tohex(hash.bytes, 32, actual);
        if (strcmp(actual, before))
            goto done;
        installed =
            ak_member(&newjson, ak_member(&newjson, 0, "body"), "installed");
        if (installed < 0 || newjson.tokens[installed].type != '[')
            goto done;
        for (i = (size_t)installed + 1; i < newjson.tokens[installed].next;
             i = newjson.tokens[i].next)
            last = i;
        if (!last)
            goto done;
        files = ak_member(&newjson, (int)last, "descriptor");
        if (files < 0 ||
            ak_json_parse(newjson.text + newjson.tokens[files].start,
                          newjson.tokens[files].end -
                              newjson.tokens[files].start,
                          &descriptor) ||
            !ak_descriptor(&descriptor))
            goto done;
        if (!ak_equal(&descriptor,
                      ak_member(&descriptor, ak_member(&descriptor, 0, "body"),
                                "name"),
                      name) ||
            ak_path(live, c->root, name))
            goto done;
        if (!decision) {
            if (exact_file(state, oldstate, on))
                goto done;
            if (ak_exists(live) == 1) {
                char held[AK_PATH];
                if (ak_path(held, txn, "restored-payload") ||
                    tree_identity(txn, live, 0) ||
                    verify_tree(live, &descriptor) || ak_publish(live, held) ||
                    verify_tree(held, &descriptor) || ak_sync_dir(c->root) ||
                    ak_sync_dir(txn))
                    goto done;
            }
        } else {
            if (tree_identity(txn, live, 0) || verify_tree(live, &descriptor))
                goto done;
            if (exact_file(state, newstate, nn)) {
                if (exact_file(state, oldstate, on) ||
                    state_publish(state, txn,
                                  c->object_max < c->memory_max / 32
                                      ? c->object_max
                                      : c->memory_max / 32))
                    goto done;
            }
        }
    }
    if (ak_exists(c->root) == 1) {
        if (ak_sync_dir(c->root))
            goto done;
    }
    if (ak_fault(7))
        goto done;
    if (init && decision && ak_resolve(c->root, c->anchor, &c->binding))
        goto done;
    {
        char archived[AK_PATH], parent[AK_PATH];
        unsigned char *id = NULL;
        size_t size;
        unsigned char decoded[16];
        if (read_in_limit(txn, "archive-id", &id, &size, 32) || size != 32 ||
            ak_hex((char *)id, decoded, 16)) {
            free(id);
            goto done;
        }
        if (snprintf(archived, sizeof archived, "%s.apkg-history.%s", c->root,
                     id) >= (int)sizeof archived) {
            free(id);
            goto done;
        }
        free(id);
        if (ak_publish(txn, archived) || parent_path(c->root, parent) ||
            ak_sync_dir(parent))
            goto done;
        strcpy(result->record, archived);
        result->view.record.size = strlen(archived);
    }
    result_set(result, APKG_OK, decision ? APKG_COMMITTED : APKG_RESTORED);
    r = APKG_OK;
done:
    free(raw);
    free(newstate);
    free(oldstate);
    free(recorded_sha);
    ak_json_free(&plan);
    ak_json_free(&newjson);
    ak_json_free(&descriptor);
    if (r != APKG_OK)
        result_set(result, r, APKG_NEEDS_RECOVERY);
    return r;
}
const apkg_result_view *apkg_result_get(const apkg_result *r) {
    return &r->view;
}
void apkg_result_free(apkg_result *r) { free(r); }
apkg_status apkg_recover(apkg_ctx *c, const apkg_observer *o, apkg_result **out,
                         apkg_error **e) {
    mutation_lock lock;
    apkg_result *r;
    apkg_status code;
    int l;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c)
        return APKG_INVALID;
    if (c->mode != APKG_WRITE)
        return APKG_POLICY;
    if (!c->binding.st_ino)
        return APKG_UNSUPPORTED;
    if (!ak_physical(c->anchor, &c->binding))
        return APKG_STALE;
    r = calloc(1, sizeof *r);
    if (!r)
        return APKG_NOMEM;
    *out = r;
    r->view.cancellation_requested = ak_cancelled(o);
    l = lock_take(c, &lock);
    if (l) {
        code = l == 1 ? APKG_BUSY : APKG_IO;
        result_set(r, code, APKG_UNCHANGED);
        return code;
    }
    code = recover_locked(c, r);
    lock_drop(&lock);
    return code;
}
static int emit(const apkg_observer *o, apkg_result *r, uint64_t *seq,
                apkg_phase phase, int cancellable, const char *subject) {
    apkg_event ev = {0};
    ev.operation_id = r->view.operation_id;
    ev.sequence = ++*seq;
    ev.phase = phase;
    ev.subject.data = subject;
    ev.subject.size = strlen(subject);
    ev.total.state = APKG_UNKNOWN;
    ev.cancellable = cancellable;
    ev.unit.data = "files";
    ev.unit.size = 5;
    if (o && o->event && o->event(o->user, &ev) == APKG_REQUEST_CANCEL) {
        r->view.cancellation_requested = 1;
        if (o->cancel)
            apkg_cancel_request(o->cancel);
    }
    if (ak_cancelled(o))
        r->view.cancellation_requested = 1;
    return cancellable && r->view.cancellation_requested;
}
static int preconditions(apkg_ctx *c, const apkg_plan *p,
                         const apkg_snapshot *s, const char *state,
                         int before_payload) {
    char path[AK_PATH];
    errno = 0;
    if (!ak_physical(p->anchor, &p->binding))
        return 0;
    if (ak_path(path, c->root, "db") || ak_exists(path) != 0 ||
        ak_path(path, c->root, ".pkg") || ak_exists(path) != 0)
        return 0;
    if (p->absent) {
        if (strcmp(p->anchor, c->root) && ak_exists(c->root) != 0)
            return 0;
        if (ak_path(path, c->root, ".apkg") || ak_exists(path) != 0)
            return 0;
        if (ak_path(path, c->root, "db") || ak_exists(path) != 0)
            return 0;
        if (ak_path(path, c->root, ".pkg") || ak_exists(path) != 0)
            return 0;
    } else {
        if (exact_file(state, (const unsigned char *)s->raw,
                       s->view.source_record.size))
            return 0;
        if (before_payload &&
            (ak_path(path, c->root, p->name) || ak_exists(path) != 0))
            return 0;
    }
    return 1;
}
apkg_status apkg_execute(apkg_ctx *c, const apkg_plan *p, apkg_approval *a,
                         const apkg_observer *o, apkg_result **out,
                         apkg_error **e) {
    apkg_result *r;
    apkg_snapshot *s = NULL;
    mutation_lock lock;
    apkg_status code = APKG_IO;
    char txn[AK_PATH], state[AK_PATH], tree[AK_PATH], path[AK_PATH], hex[65];
    int l, ready = 0, decision = 0;
    uint64_t seq = 0;
    ak_json descriptor = {0};
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c || !p || !a)
        return APKG_INVALID;
    r = calloc(1, sizeof *r);
    if (!r)
        return APKG_NOMEM;
    *out = r;
    if (a->used) {
        result_set(r, APKG_STALE, APKG_UNCHANGED);
        return APKG_STALE;
    }
    a->used = 1;
    if (c->mode != APKG_WRITE) {
        result_set(r, APKG_POLICY, APKG_UNCHANGED);
        return APKG_POLICY;
    }
    if (memcmp(c->session, a->session, 16) ||
        memcmp(a->digest.bytes, p->view.plan_digest.bytes, 32) ||
        memcmp(c->session, p->session, 16) ||
        !ak_physical(p->anchor, &p->binding)) {
        result_set(r, APKG_STALE, APKG_UNCHANGED);
        return APKG_STALE;
    }
    if (pkg_fs_random(r->view.operation_id.bytes, 16)) {
        result_set(r, APKG_IO, APKG_UNCHANGED);
        return APKG_IO;
    }
    l = lock_take(c, &lock);
    if (l) {
        code = l == 1 ? APKG_BUSY : APKG_IO;
        result_set(r, code, APKG_UNCHANGED);
        return code;
    }
    if (paths(c, txn, state))
        goto done;
    code = apkg_snapshot_read(c, &s, e);
    if (code)
        goto done;
    if (s->view.state == APKG_ROOT_PENDING) {
        code = APKG_RECOVERY_REQUIRED;
        goto done;
    }
    if ((p->absent && s->view.state != APKG_ROOT_ABSENT) ||
        (!p->absent &&
         (s->view.state != APKG_ROOT_READY ||
          memcmp(s->view.snapshot_digest.bytes, p->before.bytes, 32)))) {
        code = APKG_STALE;
        goto done;
    }
    if (p->changes) {
        code = execute_changes_locked(c, p, s, o, r);
        goto done;
    }
    if (!p->absent) {
        if (ak_path(path, c->root, p->name) || ak_exists(path) != 0) {
            code = APKG_STALE;
            goto done;
        }
    }
    if (emit(o, r, &seq, APKG_PHASE_CHECK, 1, c->root)) {
        code = APKG_CANCELLED;
        goto done;
    }
    if (!preconditions(c, p, s, state, 1)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_STALE;
        goto done;
    }
    code = APKG_IO;
    if (mkdir(txn, 0700))
        goto done;
    strcpy(r->record, txn);
    r->view.record.data = (unsigned char *)r->record;
    r->view.record.size = strlen(r->record);
    {
        char archive_id[33];
        ak_tohex(r->view.operation_id.bytes, 16, archive_id);
        if (write_in(txn, "archive-id", archive_id, 32))
            goto prepare_fail;
    }
    if (ak_fault(1) || write_in(txn, "plan", p->raw, p->view.canonical.size))
        goto prepare_fail;
    ak_tohex(p->view.plan_digest.bytes, 32, hex);
    if (write_in(txn, "plan-sha", hex, 64) ||
        write_in(txn, "new-state", p->root_record, strlen(p->root_record)) ||
        (!p->absent &&
         write_in(txn, "old-state", s->raw, s->view.source_record.size)) ||
        ak_path(tree, txn, "tree") || mkdir(tree, 0700))
        goto prepare_fail;
    if (p->absent) {
        char meta[AK_PATH];
        if (ak_path(meta, tree, ".apkg") || mkdir(meta, 0700) ||
            write_in(meta, "state", p->root_record, strlen(p->root_record)))
            goto prepare_fail;
    } else {
        if (ak_json_parse(p->package, p->package_size, &descriptor) ||
            stage_package(c, tree, &descriptor, NULL))
            goto prepare_fail;
    }
    if (sync_tree(tree) || tree_identity(txn, tree, 1) ||
        marker(txn, "ready") || ak_sync_dir(lock.parent))
        goto prepare_fail;
    ready = 1;
    if (ak_fault(2))
        goto fail;
    if (emit(o, r, &seq, APKG_PHASE_STAGE, 1, p->name)) {
        code = APKG_CANCELLED;
        goto fail;
    }
    if (!preconditions(c, p, s, state, 1)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_STALE;
        goto fail;
    }
    if (!p->absent) {
        if (ak_path(path, c->root, p->name) || ak_publish(tree, path) ||
            ak_sync_dir(c->root))
            goto fail;
        if (ak_fault(3))
            goto fail;
        if (emit(o, r, &seq, APKG_PHASE_APPLY, 1, p->name)) {
            code = APKG_CANCELLED;
            goto fail;
        }
    }
    if (!preconditions(c, p, s, state, 0)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_STALE;
        goto fail;
    }
    decision = 1;
    if (marker(txn, "decision"))
        goto fail;
    emit(o, r, &seq, APKG_PHASE_COMMIT, 0, p->name);
    if (ak_fault(4))
        goto fail;
    if (!preconditions(c, p, s, state, 0))
        goto fail;
    if (p->absent) {
        if (ak_exists(c->root) == 0) {
            if (ak_publish(tree, c->root))
                goto fail;
        } else {
            char meta[AK_PATH], staged[AK_PATH];
            if (ak_path(meta, c->root, ".apkg") ||
                ak_path(staged, tree, ".apkg") || ak_publish(staged, meta))
                goto fail;
        }
    } else if (state_publish(state, txn,
                             c->object_max < c->memory_max / 32
                                 ? c->object_max
                                 : c->memory_max / 32))
        goto fail;
    if (ak_sync_dir(c->root) || ak_fault(5))
        goto fail;
    if (p->absent) {
        if (ak_resolve(c->root, c->anchor, &c->binding))
            goto fail;
    }
    {
        apkg_status completed = recover_locked(c, r);
        if (completed != APKG_OK) {
            code = completed;
            goto done;
        }
    }
    r->view.committed_generation.state = APKG_KNOWN;
    r->view.committed_generation.value = p->view.generation + 1;
    result_set(r, APKG_OK, APKG_COMMITTED);
    code = APKG_OK;
    goto done;
prepare_fail: /* No managed publication has occurred. */
    code = errno == ENOMEM    ? APKG_NOMEM
           : errno == EILSEQ  ? APKG_INTEGRITY
           : errno == ENOEXEC ? APKG_TARGET
           : errno == ENOENT  ? APKG_NOT_FOUND
                              : APKG_IO;
    if (ak_remove_tree(txn) || ak_sync_dir(lock.parent)) {
        result_set(r, APKG_IO, APKG_NEEDS_RECOVERY);
    } else
        result_set(r, code, APKG_UNCHANGED);
    code = r->view.status;
    goto done;
fail:
    if (!decision && ready) {
        apkg_status recovered = recover_locked(c, r);
        if (recovered == APKG_OK)
            result_set(r,
                       code == APKG_CANCELLED ? APKG_CANCELLED
                       : code == APKG_STALE   ? APKG_STALE
                       : code == APKG_NOMEM   ? APKG_NOMEM
                                              : APKG_IO,
                       APKG_RESTORED);
        else
            result_set(r, APKG_RECOVERY_REQUIRED, APKG_NEEDS_RECOVERY);
    } else
        result_set(r, APKG_RECOVERY_REQUIRED, APKG_NEEDS_RECOVERY);
    code = r->view.status;
done:
    if (r->view.status == APKG_OK && code != APKG_OK)
        result_set(r, code, APKG_UNCHANGED);
    apkg_snapshot_free(s);
    ak_json_free(&descriptor);
    lock_drop(&lock);
    return code;
}
