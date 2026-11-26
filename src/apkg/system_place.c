/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * System packages: per-file placement in the root itself.
 *
 * A bundle owns the folder ROOT/<name> and is replaced as one tree. A system
 * package owns only the regular files its signed list names, in directories
 * it shares with the user and with other packages, on a volume that cannot
 * hold a second copy. Its plan therefore records one decision per file, the
 * executor places each file in its own directory after the commit decision,
 * and recovery finishes the same list forward: no before-image exists to go
 * back to. See docs/fusion/system-packages.md. */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "pkg_fs.h"
#include "pkg_fs_aside.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define CHUNK 65536u
/* The one temporary name used in a destination directory. One mutation lock
 * means one file in flight; a leftover is replaced by the next attempt. */
#define PLACING ".apkg-placing"

/* Ascending walk over a descriptor's files, or over an array of paths. */
typedef struct {
    const ak_json *j;
    size_t at, end;
    int strings, valid;
    char path[AK_PATH];
} cursor;
static int cursor_load(cursor *c) {
    c->valid = 0;
    if (!c->j || c->at >= c->end)
        return 0;
    if (ak_string(c->j,
                  c->strings ? (int)c->at
                             : ak_member(c->j, (int)c->at, "path"),
                  c->path, AK_PATH))
        return -1;
    c->valid = 1;
    return 0;
}
static int cursor_open(cursor *c, const ak_json *j, int array, int strings) {
    c->j = NULL;
    c->strings = strings;
    c->valid = 0;
    c->at = c->end = 0;
    if (array < 0)
        return 0;
    if (j->tokens[array].type != '[')
        return -1;
    c->j = j;
    c->at = (size_t)array + 1;
    c->end = j->tokens[array].next;
    return cursor_load(c);
}
static int cursor_next(cursor *c) {
    c->at = c->j->tokens[c->at].next;
    return cursor_load(c);
}
/* 1 when the cursor stands on `path` after skipping everything before it. */
static int cursor_seek(cursor *c, const char *path) {
    while (c->valid) {
        int order = strcmp(c->path, path);
        if (order >= 0)
            return order == 0;
        if (cursor_next(c))
            return -1;
    }
    return 0;
}

static int entry_body(const ak_json *j, int entry) {
    return ak_member(j, ak_member(j, entry, "descriptor"), "body");
}
static int entry_system(const ak_json *j, int entry) {
    return ak_equal(j, ak_member(j, entry_body(j, entry), "kind"), "system");
}
static int entry_files(const ak_json *j, int entry) {
    return ak_member(j, entry_body(j, entry), "files");
}
static int entry_find(ak_installed set, const char *name) {
    if (!set.j || set.array < 0)
        return -1;
    for (size_t i = (size_t)set.array + 1; i < set.j->tokens[set.array].next;
         i = set.j->tokens[i].next)
        if (ak_equal(set.j, ak_member(set.j, entry_body(set.j, (int)i), "name"),
                     name))
            return (int)i;
    return -1;
}
static int same_slice(const ak_json *a, int x, const ak_json *b, int y) {
    size_t n;
    if (x < 0 || y < 0)
        return 0;
    n = a->tokens[x].end - a->tokens[x].start;
    return n == b->tokens[y].end - b->tokens[y].start &&
           !memcmp(a->text + a->tokens[x].start, b->text + b->tokens[y].start,
                   n);
}
int ak_package_system(const apkg_package *p) {
    return p && ak_equal(&p->json,
                         ak_member(&p->json, ak_member(&p->json, 0, "body"),
                                   "kind"),
                         "system");
}
/* Local record of the directories the engine created for a system package:
 * only those may go when a later version drops them. */
int ak_placement_valid(const ak_json *j, int token) {
    char path[AK_PATH], previous[AK_PATH] = "";
    int array;
    if (token < 0)
        return 1;
    if (!ak_fields(j, token, (const char *const[]){"directories"}, 1))
        return 0;
    array = ak_member(j, token, "directories");
    if (j->tokens[array].type != '[')
        return 0;
    for (size_t i = (size_t)array + 1; i < j->tokens[array].next;
         i = j->tokens[i].next) {
        if (ak_string(j, (int)i, path, sizeof path) || !ak_valid_path(path) ||
            ak_system_path_reserved(path) ||
            (*previous && strcmp(previous, path) >= 0))
            return 0;
        strcpy(previous, path);
    }
    return 1;
}

enum { LIVE_ABSENT, LIVE_FILE, LIVE_DIRECTORY, LIVE_OTHER };
static apkg_status live_read(const char *root, const char *relative,
                             char *full, int *kind, char sha[65],
                             uint64_t *size) {
    struct stat st;
    unsigned char head[64];
    size_t head_size = 0;
    apkg_status code;
    sha[0] = 0;
    *size = 0;
    if (ak_path(full, root, relative))
        return APKG_LIMIT;
    if (lstat(full, &st)) {
        if (errno != ENOENT && errno != ENOTDIR)
            return APKG_IO;
        *kind = LIVE_ABSENT;
        return APKG_OK;
    }
    if (S_ISDIR(st.st_mode)) {
        *kind = LIVE_DIRECTORY;
        return APKG_OK;
    }
    if (!S_ISREG(st.st_mode)) {
        *kind = LIVE_OTHER;
        return APKG_OK;
    }
    code = ak_stream_hash(full, size, sha, head, &head_size);
    if (code == APKG_NOT_FOUND)
        code = APKG_STALE; /* it was there a moment ago */
    *kind = LIVE_FILE;
    return code;
}

/* AROS volumes do not tell case apart, so neither do ownership and the
 * comparison of two signed lists: every path is looked up by its folded
 * form. A descriptor never lists two paths that fold alike (schema.c). */
typedef struct {
    char *path, *fold;
    int token, directory;
} row;
typedef struct {
    row *rows;
    size_t count;
} table;
static int row_compare(const void *a, const void *b) {
    return strcmp(((const row *)a)->fold, ((const row *)b)->fold);
}
static void table_free(table *t) {
    if (t->rows)
        for (size_t i = 0; i < t->count; i++) {
            free(t->rows[i].path);
            free(t->rows[i].fold);
        }
    free(t->rows);
    t->rows = NULL;
    t->count = 0;
}
static apkg_status table_add(table *t, const char *path, int token,
                             int directory) {
    row *r = &t->rows[t->count];
    size_t n = strlen(path);
    r->path = ak_copy(path, n);
    r->fold = ak_copy(path, n);
    if (!r->path || !r->fold) {
        free(r->path);
        free(r->fold);
        return APKG_NOMEM;
    }
    for (size_t i = 0; i < n; i++)
        if (r->fold[i] >= 'A' && r->fold[i] <= 'Z')
            r->fold[i] = (char)(r->fold[i] + 32);
    r->token = token;
    r->directory = directory;
    t->count++;
    return APKG_OK;
}
/* The files of a descriptor (strings 0) or an array of paths (strings 1),
 * by folded path. array < 0 is an empty table. */
static apkg_status table_load(table *t, const ak_json *j, int array,
                              int strings, size_t max) {
    size_t n = 0;
    char *path;
    apkg_status code = APKG_OK;
    memset(t, 0, sizeof *t);
    if (array < 0)
        return APKG_OK;
    if (j->tokens[array].type != '[')
        return APKG_INVALID;
    for (size_t i = (size_t)array + 1; i < j->tokens[array].next;
         i = j->tokens[i].next)
        n++;
    if (!n)
        return APKG_OK;
    if (n > max / sizeof *t->rows)
        return APKG_LIMIT;
    t->rows = calloc(n, sizeof *t->rows);
    path = malloc(AK_PATH);
    if (!t->rows || !path)
        code = APKG_NOMEM;
    for (size_t i = (size_t)array + 1; !code && i < j->tokens[array].next;
         i = j->tokens[i].next) {
        if (ak_string(j, strings ? (int)i : ak_member(j, (int)i, "path"), path,
                      AK_PATH))
            code = APKG_INVALID;
        else
            code = table_add(t, path, (int)i,
                             !strings &&
                                 ak_equal(j, ak_member(j, (int)i, "type"),
                                          "directory"));
    }
    free(path);
    if (code)
        table_free(t);
    else
        qsort(t->rows, t->count, sizeof *t->rows, row_compare);
    return code;
}
static const row *table_find(const table *t, const char *fold) {
    row key;
    if (!t->count)
        return NULL;
    key.fold = (char *)fold;
    return bsearch(&key, t->rows, t->count, sizeof *t->rows, row_compare);
}
static const row *table_find_path(const table *t, const char *path,
                                  char *scratch) {
    size_t i;
    for (i = 0; path[i]; i++)
        scratch[i] = path[i] >= 'A' && path[i] <= 'Z' ? (char)(path[i] + 32)
                                                      : path[i];
    scratch[i] = 0;
    return table_find(t, scratch);
}
/* Paths the other system packages of the resulting root list. */
static apkg_status foreign_load(table *t, ak_installed next, int self,
                                size_t max) {
    size_t n = 0;
    char *path = malloc(AK_PATH);
    apkg_status code = path ? APKG_OK : APKG_NOMEM;
    memset(t, 0, sizeof *t);
    for (int pass = 0; pass < 2 && !code; pass++) {
        for (size_t e = (size_t)next.array + 1;
             e < next.j->tokens[next.array].next && !code;
             e = next.j->tokens[e].next) {
            int files;
            if ((int)e == self || !entry_system(next.j, (int)e))
                continue;
            files = entry_files(next.j, (int)e);
            for (size_t f = (size_t)files + 1;
                 f < next.j->tokens[files].next && !code;
                 f = next.j->tokens[f].next) {
                if (!pass)
                    n++;
                else if (ak_string(next.j, ak_member(next.j, (int)f, "path"),
                                   path, AK_PATH))
                    code = APKG_INVALID;
                else
                    code = table_add(
                        t, path, (int)f,
                        ak_equal(next.j, ak_member(next.j, (int)f, "type"),
                                 "directory"));
            }
        }
        if (!pass && n) {
            if (n > max / sizeof *t->rows)
                code = APKG_LIMIT;
            else if (!(t->rows = calloc(n, sizeof *t->rows)))
                code = APKG_NOMEM;
        }
    }
    free(path);
    if (code)
        table_free(t);
    else if (t->count)
        qsort(t->rows, t->count, sizeof *t->rows, row_compare);
    return code;
}

enum {
    L_CLEAR,
    L_DELETE,
    L_DIRECTORIES,
    L_KEEP,
    L_OVERWRITE,
    L_RELEASE,
    L_RMDIR,
    L_TAKEN,
    L_WRITE,
    L_PLACEMENT,
    L_COUNT
};
typedef struct {
    const char **paths;
    size_t count, capacity;
} pathlist;
static int path_compare(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static apkg_status list_add(pathlist *list, const char *path) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 64;
        const char **grown = calloc(capacity, sizeof *grown);
        if (!grown)
            return APKG_NOMEM;
        if (list->count)
            memcpy(grown, list->paths, list->count * sizeof *grown);
        free(list->paths);
        list->paths = grown;
        list->capacity = capacity;
    }
    list->paths[list->count++] = path;
    return APKG_OK;
}
/* Ascending by exact path, as every reader of a record expects. Valid
 * payload paths hold no quote, backslash or control byte. */
static void list_write(ak_buf *b, pathlist *list) {
    if (list->count > 1)
        qsort(list->paths, list->count, sizeof *list->paths, path_compare);
    for (size_t i = 0; i < list->count; i++)
        ak_buf_add(b, "%s\"%s\"", i ? "," : "", list->paths[i]);
}
static apkg_status refuse(apkg_error **e, apkg_status code, const char *why,
                          const char *subject) {
    if (e && !*e)
        ak_fail(e, code, why, subject);
    return code;
}
/* Everything under a directory that is to become a file must be the old
 * version's own: one unlisted entry and the directory is not ours to drop. */
static apkg_status only_listed(const char *root, const char *relative,
                               const table *old, char *scratch,
                               apkg_error **e) {
    char *full = malloc(AK_PATH), *child = malloc(AK_PATH);
    DIR *d = NULL;
    struct dirent *entry;
    apkg_status code = APKG_OK;
    if (!full || !child)
        code = APKG_NOMEM;
    else if (ak_path(full, root, relative))
        code = APKG_LIMIT;
    else if (!(d = opendir(full)))
        code = APKG_IO;
    errno = 0;
    while (!code && (entry = readdir(d))) {
        const row *known;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (ak_path(child, relative, entry->d_name)) {
            code = APKG_LIMIT;
            break;
        }
        known = table_find_path(old, child, scratch);
        if (!known)
            code = refuse(e, APKG_CONFLICT,
                          "a directory that becomes a file holds something "
                          "the package does not own",
                          child);
        else if (known->directory)
            code = only_listed(root, child, old, scratch, e);
        errno = 0;
    }
    if (!code && d && errno)
        code = APKG_IO;
    if (d)
        closedir(d);
    free(full);
    free(child);
    return code;
}
/* One decision per path of the old and the new signed list, from the bytes
 * that are there now. The record is a pure function of its inputs and of
 * those bytes: building it again before execution and comparing is the live
 * precondition check. */
static apkg_status participant(apkg_ctx *c, ak_installed old, int old_entry,
                               ak_installed next, int new_entry,
                               const char *name, const char *operation,
                               int takeover, size_t max, ak_buf *record,
                               ak_buf *placement, apkg_error **e) {
    const ak_json *oj = old.j, *nj = next.j;
    table was = {0}, now = {0}, created = {0}, foreign = {0};
    pathlist lists[L_COUNT];
    const char **retyped = NULL; /* folded directories that become files */
    size_t nretyped = 0, io = 0, in = 0;
    char *full = malloc(AK_PATH), *scratch = malloc(AK_PATH);
    struct pkg_sha256 observed;
    unsigned char digest[32];
    char hex[65];
    uint64_t bytes = 0;
    int repair = !strcmp(operation, "repair");
    ak_buf out = {0};
    apkg_status code = full && scratch ? APKG_OK : APKG_NOMEM;
    memset(lists, 0, sizeof lists);
    if (!code)
        code = table_load(&was, oj, old_entry < 0 ? -1 : entry_files(oj, old_entry),
                          0, max);
    if (!code)
        code = table_load(&now, nj, entry_files(nj, new_entry), 0, max);
    if (!code)
        code = table_load(
            &created, oj,
            old_entry < 0 ? -1
                          : ak_member(oj, ak_member(oj, old_entry, "placement"),
                                      "directories"),
            1, max);
    if (!code)
        code = foreign_load(&foreign, next, new_entry, max);
    if (!code && was.count &&
        !(retyped = calloc(was.count, sizeof *retyped)))
        code = APKG_NOMEM;
    pkg_sha256_init(&observed);
#define ADD(list, value)                                                       \
    do {                                                                       \
        code = list_add(&lists[list], value);                                  \
        if (code)                                                              \
            goto done;                                                         \
    } while (0)
    while (!code && (io < was.count || in < now.count)) {
        int order = io >= was.count   ? 1
                    : in >= now.count ? -1
                                      : strcmp(was.rows[io].fold,
                                               now.rows[in].fold);
        const row *o = order <= 0 ? &was.rows[io] : NULL,
                  *n = order >= 0 ? &now.rows[in] : NULL;
        const char *path = n ? n->path : o->path, *fold = n ? n->fold : o->fold;
        const row *elsewhere = table_find(&foreign, fold);
        int old_file = o && !o->directory, kind, inside = 0;
        int was_created = table_find(&created, fold) != NULL;
        char live[65], sha[65] = "", before[65] = "";
        uint64_t live_size, size = 0;
        if (order <= 0)
            io++;
        if (order >= 0)
            in++;
        if (n && ak_member(nj, n->token, "amiga") >= 0) {
            code = refuse(e, APKG_UNSUPPORTED,
                          "system packages carry no amiga metadata", path);
            break;
        }
        for (size_t r = 0; r < nretyped && !inside; r++) {
            size_t length = strlen(retyped[r]);
            inside = !strncmp(fold, retyped[r], length) && fold[length] == '/';
        }
        code = live_read(c->root, path, full, &kind, live, &live_size);
        if (code)
            break;
        pkg_sha256_update(&observed, (const unsigned char *)path,
                          strlen(path) + 1);
        pkg_sha256_update(&observed, (const unsigned char *)"adfo" + kind, 1);
        pkg_sha256_update(&observed, (const unsigned char *)live,
                          strlen(live) + 1);
        if (old_file &&
            ak_string(oj, ak_member(oj, o->token, "sha256"), before,
                      sizeof before)) {
            code = APKG_INVALID;
            break;
        }
        if (n && elsewhere && !(n->directory && elsewhere->directory)) {
            code = refuse(e, APKG_CONFLICT,
                          "another installed system package owns this path",
                          path);
            break;
        }
        if (n && n->directory) {
            if (kind == LIVE_FILE && old_file && !strcmp(live, before)) {
                /* The old version's own file makes way for the directory. */
                ADD(L_CLEAR, path);
                ADD(L_DIRECTORIES, path);
                ADD(L_PLACEMENT, path);
            } else if (kind == LIVE_ABSENT) {
                ADD(L_DIRECTORIES, path);
                ADD(L_PLACEMENT, path);
            } else if (kind != LIVE_DIRECTORY) {
                code = refuse(e, APKG_CONFLICT,
                              "a system directory path holds something else",
                              path);
                break;
            } else if (was_created)
                ADD(L_PLACEMENT, path);
        } else if (n) {
            int preserve =
                ak_equal(nj, ak_member(nj, n->token, "config"), "preserve");
            if (ak_string(nj, ak_member(nj, n->token, "sha256"), sha,
                          sizeof sha) ||
                ak_u64(nj, ak_member(nj, n->token, "size"), &size)) {
                code = APKG_INVALID;
                break;
            }
            if (kind == LIVE_DIRECTORY && o && o->directory) {
                /* The old version's directory makes way for the file when
                 * all it holds is that version's own, unmodified. */
                code = only_listed(c->root, path, &was, scratch, e);
                if (code)
                    break;
                retyped[nretyped++] = fold;
                ADD(L_CLEAR, path);
                ADD(L_WRITE, path);
                bytes += size;
            } else if (kind == LIVE_DIRECTORY || kind == LIVE_OTHER) {
                code = refuse(e, APKG_CONFLICT,
                              "a system file path holds something else", path);
                break;
            } else if (kind == LIVE_ABSENT) {
                ADD(L_WRITE, path);
                bytes += size;
            } else if (!strcmp(live, sha)) {
                /* Already the signed bytes: owned without a write. */
            } else if (old_file) {
                if (!repair && !strcmp(live, before)) {
                    ADD(L_WRITE, path);
                    bytes += size;
                } else if (preserve)
                    ADD(L_KEEP, path);
                else {
                    ADD(L_OVERWRITE, path);
                    bytes += size;
                }
            } else if (!takeover) {
                code = refuse(e, APKG_CONFLICT,
                              "an unowned file with other content is in the "
                              "way; takeover was not requested",
                              path);
                break;
            } else if (preserve) {
                /* A volume someone already configured: their settings are
                 * adopted as they are, never replaced by the defaults. */
                ADD(L_KEEP, path);
            } else {
                ADD(L_TAKEN, path);
                bytes += size;
            }
        } else if (inside) {
            /* Under a directory that becomes a file: it goes first, and only
             * as the old version left it. */
            if (kind == LIVE_ABSENT)
                continue;
            if ((o->directory && kind != LIVE_DIRECTORY) ||
                (!o->directory &&
                 (kind != LIVE_FILE || strcmp(live, before)))) {
                code = refuse(e, APKG_CONFLICT,
                              "a directory that becomes a file holds a "
                              "modified entry",
                              path);
                break;
            }
            ADD(L_CLEAR, path);
        } else if (elsewhere) {
            /* Handed over to another package of this plan: not dropped. */
        } else if (old_file) {
            if (kind == LIVE_FILE)
                ADD(strcmp(live, before) ? L_RELEASE : L_DELETE, path);
        } else if (was_created && kind == LIVE_DIRECTORY)
            ADD(L_RMDIR, path);
    }
#undef ADD
    if (code)
        goto done;
    /* Bytes to place must be in the store. Inline bytes of a development
     * descriptor arrive there when the transaction is prepared. */
    for (int l = 0; l < 3 && !code; l++) {
        const pathlist *list = &lists[l == 0 ? L_OVERWRITE : l == 1 ? L_TAKEN : L_WRITE];
        for (size_t i = 0; i < list->count && !code; i++) {
            const row *n = table_find_path(&now, list->paths[i], scratch);
            struct stat st;
            if (!n || ak_member(nj, n->token, "blob") < 0)
                continue;
            if (ak_string(nj, ak_member(nj, n->token, "sha256"), hex,
                          sizeof hex))
                code = APKG_INVALID;
            else if (ak_blob_path(c, hex, full) || lstat(full, &st))
                code = refuse(e, APKG_NOT_FOUND,
                              "the store holds no bytes for this file: fetch "
                              "the package again, or name the store it was "
                              "installed with",
                              list->paths[i]);
        }
    }
    if (code)
        goto done;
    pkg_sha256_final(&observed, digest);
    ak_tohex(digest, 32, hex);
    out.max = max;
    ak_buf_add(&out, "%s{\"bytes\":\"%" PRIu64 "\",\"clear\":[",
               record->size > 1 ? "," : "", bytes);
    list_write(&out, &lists[L_CLEAR]);
    ak_buf_add(&out, "],\"delete\":[");
    list_write(&out, &lists[L_DELETE]);
    ak_buf_add(&out, "],\"directories\":[");
    list_write(&out, &lists[L_DIRECTORIES]);
    ak_buf_add(&out, "],\"keep\":[");
    list_write(&out, &lists[L_KEEP]);
    ak_buf_add(&out,
               "],\"name\":\"%s\",\"observed\":\"%s\",\"operation\":\"%s\","
               "\"overwrite\":[",
               name, hex, operation);
    list_write(&out, &lists[L_OVERWRITE]);
    ak_buf_add(&out, "],\"release\":[");
    list_write(&out, &lists[L_RELEASE]);
    ak_buf_add(&out, "],\"rmdir\":[");
    list_write(&out, &lists[L_RMDIR]);
    ak_buf_add(&out, "],\"taken\":[");
    list_write(&out, &lists[L_TAKEN]);
    ak_buf_add(&out, "],\"takeover\":\"%d\",\"write\":[", !!takeover);
    list_write(&out, &lists[L_WRITE]);
    ak_buf_add(&out, "]}");
    ak_buf_add(placement, "{\"directories\":[");
    list_write(placement, &lists[L_PLACEMENT]);
    ak_buf_add(placement, "]}");
    if (!out.bad)
        ak_buf_add(record, "%s", out.data);
    code = out.bad          ? (apkg_status)out.bad
           : record->bad    ? (apkg_status)record->bad
           : placement->bad ? (apkg_status)placement->bad
                            : APKG_OK;
done:
    for (int i = 0; i < L_COUNT; i++)
        free(lists[i].paths);
    free(out.data);
    free(retyped);
    free(full);
    free(scratch);
    table_free(&was);
    table_free(&now);
    table_free(&created);
    table_free(&foreign);
    return code;
}
/* A bundle is the folder ROOT/<name>: no system file may live under a name a
 * bundle of the resulting root holds, in either order of arrival. */
static apkg_status coexist(ak_installed next, int entry, apkg_error **e) {
    const ak_json *j = next.j;
    int files = entry_files(j, entry);
    char *top = malloc(AK_PATH), name[256];
    apkg_status code = APKG_OK;
    if (!top)
        return APKG_NOMEM;
    for (size_t f = (size_t)files + 1; f < j->tokens[files].next && !code;
         f = j->tokens[f].next) {
        if (ak_string(j, ak_member(j, (int)f, "path"), top, AK_PATH)) {
            code = APKG_INVALID;
            break;
        }
        if (strchr(top, '/'))
            continue; /* its first component has an entry of its own */
        for (size_t b = (size_t)next.array + 1;
             b < j->tokens[next.array].next; b = j->tokens[b].next) {
            if (entry_system(j, (int)b))
                continue;
            if (ak_string(j, ak_member(j, entry_body(j, (int)b), "name"), name,
                          sizeof name)) {
                code = APKG_INVALID;
                break;
            }
            if (!strcasecmp(name, top)) {
                code = refuse(e, APKG_CONFLICT,
                              "a system package path and a package folder "
                              "share this name",
                              name);
                break;
            }
        }
    }
    free(top);
    return code;
}
static void slice_add(ak_buf *b, const char *text, size_t n) {
    /* Descriptors run to megabytes: append the bytes, not a format. */
    if (b->bad)
        return;
    if (n > b->max - b->size) {
        b->bad = APKG_LIMIT;
        return;
    }
    if (b->size + n + 1 > b->cap) {
        size_t cap = (b->size + n + 1) * 2;
        char *p = malloc(cap);
        if (!p) {
            b->bad = APKG_NOMEM;
            return;
        }
        if (b->size)
            memcpy(p, b->data, b->size);
        free(b->data);
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->size, text, n);
    b->size += n;
    b->data[b->size] = 0;
}
static void entry_copy(ak_buf *b, const ak_json *j, int entry) {
    ak_buf_add(b, "%s", b->size > 1 ? "," : "");
    slice_add(b, j->text + j->tokens[entry].start,
              j->tokens[entry].end - j->tokens[entry].start);
}
/* The committed record of a placed system package: its signed descriptor and
 * the directories the engine created for it. */
static void entry_render(ak_buf *b, const ak_json *j, int entry,
                         const char *placement) {
    static const char *const members[] = {"adoption", "descriptor", "placement",
                                          "reason", "signature"};
    int first = 1;
    ak_buf_add(b, "%s{", b->size > 1 ? "," : "");
    for (size_t k = 0; k < 5; k++) {
        int token = ak_member(j, entry, members[k]);
        if (k == 2) {
            ak_buf_add(b, "%s\"placement\":%s", first ? "" : ",", placement);
            first = 0;
            continue;
        }
        if (token < 0)
            continue;
        ak_buf_add(b, "%s\"%s\":", first ? "" : ",", members[k]);
        slice_add(b, j->text + j->tokens[token].start,
                  j->tokens[token].end - j->tokens[token].start);
        first = 0;
    }
    ak_buf_add(b, "}");
}
static int approved_find(const ak_json *approved, int array, const char *name) {
    if (!approved || array < 0)
        return -1;
    for (size_t i = (size_t)array + 1; i < approved->tokens[array].next;
         i = approved->tokens[i].next)
        if (ak_equal(approved, ak_member(approved, (int)i, "name"), name))
            return (int)i;
    return -1;
}
apkg_status ak_place_plan(apkg_ctx *c, ak_installed old, ak_installed next,
                           const char *policy, apkg_plan_kind kind,
                           const char *focus, const ak_json *approved,
                           int approved_array, char **system_out,
                           char **installed_out, apkg_error **e) {
    const ak_json *oj = old.j, *nj = next.j;
    ak_json trust = {0};
    ak_buf records = {0}, installed = {0}, placement = {0};
    size_t max = c->object_max < c->memory_max / 32 ? c->object_max
                                                    : c->memory_max / 32;
    int any = 0, new_bundle = 0, participants = 0;
    char name[256], authority[33], key[65];
    apkg_status code = APKG_OK;
    *system_out = *installed_out = NULL;
    if (!nj || next.array < 0 || nj->tokens[next.array].type != '[' || !oj ||
        old.array < 0 || oj->tokens[old.array].type != '[')
        return APKG_INVALID;
    for (size_t i = (size_t)old.array + 1; i < oj->tokens[old.array].next;
         i = oj->tokens[i].next) {
        int other;
        if (!entry_system(oj, (int)i))
            continue;
        any = 1;
        if (ak_string(oj, ak_member(oj, entry_body(oj, (int)i), "name"), name,
                      sizeof name))
            return APKG_INVALID;
        other = entry_find(next, name);
        if (other < 0)
            return refuse(e, APKG_UNSUPPORTED,
                          "a system package cannot be removed", name);
        if (!entry_system(nj, other))
            return refuse(e, APKG_UNSUPPORTED,
                          "a system package cannot become a bundle", name);
    }
    for (size_t i = (size_t)next.array + 1; i < nj->tokens[next.array].next;
         i = nj->tokens[i].next) {
        int other;
        if (ak_string(nj, ak_member(nj, entry_body(nj, (int)i), "name"), name,
                      sizeof name))
            return APKG_INVALID;
        other = entry_find(old, name);
        if (!entry_system(nj, (int)i)) {
            if (other < 0)
                new_bundle = 1;
            continue;
        }
        any = 1;
        if (other >= 0 && !entry_system(oj, other))
            return refuse(e, APKG_UNSUPPORTED,
                          "a bundle cannot become a system package", name);
    }
    if (!any)
        return APKG_OK;
    records.max = installed.max = placement.max = max;
    ak_buf_add(&records, "[");
    ak_buf_add(&installed, "[");
    if (policy) {
        int parsed = ak_json_parse(policy, strlen(policy), &trust);
        if (parsed) {
            free(records.data);
            free(installed.data);
            return parsed == -2 ? APKG_NOMEM : APKG_TRUST;
        }
    }
    for (size_t i = (size_t)next.array + 1;
         i < nj->tokens[next.array].next && !code; i = nj->tokens[i].next) {
        const char *operation = NULL;
        int body = entry_body(nj, (int)i), was, record = -1, takeover;
        if (!entry_system(nj, (int)i)) {
            entry_copy(&installed, nj, (int)i);
            continue;
        }
        if (ak_string(nj, ak_member(nj, body, "name"), name, sizeof name) ||
            ak_string(nj, ak_member(nj, body, "authority"), authority,
                      sizeof authority) ||
            ak_string(nj,
                      ak_member(nj, ak_member(nj, (int)i, "signature"),
                                "public_key"),
                      key, sizeof key)) {
            code = APKG_INVALID;
            break;
        }
        was = entry_find(old, name);
        record = approved_find(approved, approved_array, name);
        if (was < 0)
            operation = "install";
        else if (!same_slice(nj, ak_member(nj, (int)i, "descriptor"), oj,
                             ak_member(oj, was, "descriptor"))) {
            if (!ak_equal(oj, ak_member(oj, entry_body(oj, was), "authority"),
                          authority)) {
                code = refuse(e, APKG_TRUST,
                              "a system package keeps its authority", name);
                break;
            }
            operation = "upgrade";
        } else if (approved ? record >= 0
                            : kind == APKG_PLAN_REPAIR && focus &&
                                  !strcmp(focus, name))
            operation = "repair";
        if (operation || new_bundle) {
            code = coexist(next, (int)i, e);
            if (code)
                break;
        }
        if (!operation) {
            entry_copy(&installed, nj, (int)i); /* stays as committed */
            continue;
        }
        if (kind == APKG_PLAN_ROLLBACK || kind == APKG_PLAN_ADOPT) {
            code = refuse(e, APKG_UNSUPPORTED,
                          kind == APKG_PLAN_ADOPT
                              ? "a system package cannot be adopted as a bundle"
                              : "a system package transaction cannot be rolled "
                                "back",
                          name);
            break;
        }
        if (approved) {
            if (record < 0 ||
                !ak_equal(approved, ak_member(approved, record, "operation"),
                          operation)) {
                code = APKG_STALE;
                break;
            }
            takeover =
                ak_equal(approved, ak_member(approved, record, "takeover"), "1");
        } else {
            takeover = c->takeover;
            if (!policy || !ak_policy_system(&trust, authority, key)) {
                code = refuse(e, APKG_TRUST,
                              "the root's policy does not let this authority "
                              "place system packages",
                              name);
                break;
            }
        }
        free(placement.data);
        memset(&placement, 0, sizeof placement);
        placement.max = max;
        code = participant(c, old, was, next, (int)i, name, operation, takeover,
                           max, &records, &placement, e);
        if (code)
            break;
        entry_render(&installed, nj, (int)i, placement.data);
        participants++;
    }
    ak_buf_add(&records, "]");
    ak_buf_add(&installed, "]");
    if (!code && (records.bad || installed.bad))
        code = (apkg_status)(records.bad ? records.bad : installed.bad);
    if (!code && approved) {
        /* The approved bytes must be what the live root still implies. */
        size_t n = approved->tokens[approved_array].end -
                   approved->tokens[approved_array].start;
        if (n != records.size ||
            memcmp(approved->text + approved->tokens[approved_array].start,
                   records.data, n))
            code = APKG_STALE;
    }
    if (!code && participants) {
        *system_out = records.data;
        *installed_out = installed.data;
        records.data = installed.data = NULL;
    }
    free(records.data);
    free(installed.data);
    free(placement.data);
    ak_json_free(&trust);
    return code;
}
/* The record names only paths; shapes are checked before anything acts. */
int ak_place_records_valid(const ak_json *j, int array) {
    static const char *const fields[] = {
        "bytes",   "clear",    "delete",    "directories", "keep",
        "name",    "observed", "operation", "overwrite",   "release",
        "rmdir",   "takeover", "taken",     "write"};
    static const char *const lists[] = {"clear",       "delete", "directories",
                                        "keep",        "overwrite",
                                        "release",     "rmdir",  "taken",
                                        "write"};
    char *path, *previous;
    int valid = 0;
    if (array < 0)
        return 1;
    if (j->tokens[array].type != '[' || ak_empty(j, array))
        return 0;
    path = malloc(AK_PATH);
    previous = malloc(AK_PATH);
    if (!path || !previous)
        goto done;
    for (size_t i = (size_t)array + 1; i < j->tokens[array].next;
         i = j->tokens[i].next) {
        char name[256], text[65];
        unsigned char digest[32];
        uint64_t bytes;
        if (!ak_fields(j, (int)i, fields, 14) ||
            ak_string(j, ak_member(j, (int)i, "name"), name, sizeof name) ||
            !ak_valid_name(name) ||
            ak_u64(j, ak_member(j, (int)i, "bytes"), &bytes) ||
            ak_string(j, ak_member(j, (int)i, "observed"), text, sizeof text) ||
            ak_hex(text, digest, 32) ||
            (!ak_equal(j, ak_member(j, (int)i, "takeover"), "0") &&
             !ak_equal(j, ak_member(j, (int)i, "takeover"), "1")) ||
            (!ak_equal(j, ak_member(j, (int)i, "operation"), "install") &&
             !ak_equal(j, ak_member(j, (int)i, "operation"), "upgrade") &&
             !ak_equal(j, ak_member(j, (int)i, "operation"), "repair")))
            goto done;
        for (size_t x = (size_t)array + 1; x < i; x = j->tokens[x].next)
            if (ak_equal(j, ak_member(j, (int)x, "name"), name))
                goto done;
        for (size_t k = 0; k < 9; k++) {
            int list = ak_member(j, (int)i, lists[k]);
            *previous = 0;
            if (j->tokens[list].type != '[')
                goto done;
            for (size_t p = (size_t)list + 1; p < j->tokens[list].next;
                 p = j->tokens[p].next) {
                if (ak_string(j, (int)p, path, AK_PATH) ||
                    !ak_valid_path(path) || ak_system_path_reserved(path) ||
                    (*previous && strcmp(previous, path) >= 0))
                    goto done;
                strcpy(previous, path);
            }
        }
    }
    valid = 1;
done:
    free(path);
    free(previous);
    return valid;
}
static int plan_arrays(const ak_json *plan, int *system, ak_installed *next) {
    int body = ak_member(plan, 0, "body");
    *system = ak_member(plan, body, "system");
    next->j = plan;
    next->array = ak_member(
        plan, ak_member(plan, ak_member(plan, body, "new_state"), "body"),
        "installed");
    return *system >= 0 && next->array < 0 ? -1 : 0;
}
static int state_installed(const void *state, size_t size, ak_json *j,
                           ak_installed *out) {
    int parsed = ak_json_parse(state, size, j);
    if (parsed)
        return parsed;
    out->j = j;
    out->array = ak_member(j, ak_member(j, 0, "body"), "installed");
    return out->array < 0 ? -1 : 0;
}
apkg_status ak_place_recheck(apkg_ctx *c, const ak_json *plan,
                              const void *old_state, size_t old_size) {
    ak_json before = {0};
    ak_installed old, next;
    char *records = NULL, *installed = NULL;
    int system, parsed;
    apkg_status code;
    if (plan_arrays(plan, &system, &next))
        return APKG_INVALID;
    if (system < 0)
        return APKG_OK;
    parsed = state_installed(old_state, old_size, &before, &old);
    if (parsed) {
        ak_json_free(&before);
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    }
    code = ak_place_plan(c, old, next, NULL, APKG_PLAN_RESOLVE, NULL, plan,
                          system, &records, &installed, NULL);
    /* The directories recorded as created are part of the approved state. */
    if (!code && (!installed ||
                  strlen(installed) != plan->tokens[next.array].end -
                                           plan->tokens[next.array].start ||
                  memcmp(installed, plan->text + plan->tokens[next.array].start,
                         strlen(installed))))
        code = APKG_STALE;
    if (code == APKG_CONFLICT || code == APKG_UNSUPPORTED || code == APKG_TRUST)
        code = APKG_STALE;
    free(records);
    free(installed);
    ak_json_free(&before);
    return code;
}

static void progress(const ak_place_progress *p, const char *subject,
                     const char *unit, uint64_t completed, uint64_t total) {
    apkg_event event = {0};
    if (!p || !p->observer || !p->observer->event)
        return;
    event.operation_id = p->result->view.operation_id;
    event.sequence = ++*p->sequence;
    event.phase = p->phase;
    event.subject = (apkg_text){subject, strlen(subject)};
    event.completed = completed;
    event.total.state = APKG_KNOWN;
    event.total.value = total;
    event.unit = (apkg_text){unit, strlen(unit)};
    /* Placement runs after the commit decision: a request to cancel is
     * recorded and the transaction still finishes. */
    if (p->observer->event(p->observer->user, &event) == APKG_REQUEST_CANCEL)
        p->result->view.cancellation_requested = 1;
}
static int regular(const char *path, struct stat *st) {
    if (lstat(path, st))
        return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    return S_ISREG(st->st_mode) ? 1 : -1;
}
/* The paths of one list of a record, in the record's ascending order. */
typedef struct {
    char **paths;
    size_t count;
} strings;
static void strings_free(strings *l) {
    if (l->paths)
        for (size_t i = 0; i < l->count; i++)
            free(l->paths[i]);
    free(l->paths);
    l->paths = NULL;
    l->count = 0;
}
static apkg_status strings_load(strings *l, const ak_json *j, int record,
                                const char *name) {
    int list = ak_member(j, record, name);
    size_t n = 0;
    char *path;
    l->paths = NULL;
    l->count = 0;
    if (list < 0 || j->tokens[list].type != '[')
        return APKG_INVALID;
    for (size_t i = (size_t)list + 1; i < j->tokens[list].next;
         i = j->tokens[i].next)
        n++;
    if (!n)
        return APKG_OK;
    l->paths = calloc(n, sizeof *l->paths);
    path = malloc(AK_PATH);
    if (!l->paths || !path) {
        free(path);
        strings_free(l);
        return APKG_NOMEM;
    }
    for (size_t i = (size_t)list + 1; i < j->tokens[list].next;
         i = j->tokens[i].next) {
        if (ak_string(j, (int)i, path, AK_PATH) ||
            !(l->paths[l->count] = ak_copy(path, strlen(path)))) {
            free(path);
            strings_free(l);
            return APKG_INVALID;
        }
        l->count++;
    }
    free(path);
    return APKG_OK;
}
static int plan_arrays(const ak_json *plan, int *system, ak_installed *next);
static int state_installed(const void *state, size_t size, ak_json *j,
                           ak_installed *out);
static const char *const placed_lists[] = {"overwrite", "taken", "write"};
/* The last check before the commit decision, after which nothing can be
 * taken back: every object to place is in the store with its signed bytes,
 * and the destination volume has room. An in-place replacement needs the new
 * file beside the old one until it takes the name, so the room needed is the
 * highest point of that sequence, not the net growth. */
apkg_status ak_place_ready(apkg_ctx *c, const ak_json *plan, apkg_error **e) {
    ak_installed next;
    table now = {0};
    strings list = {0};
    char *full = malloc(AK_PATH), *scratch = malloc(AK_PATH), name[256];
    unsigned long long available = 0, unit = 0;
    int64_t running = 0, peak = 0;
    uint64_t entries = 0, state_size;
    int system;
    apkg_status code = full && scratch ? APKG_OK : APKG_NOMEM;
    if (!code && plan_arrays(plan, &system, &next))
        code = APKG_INVALID;
    if (code || system < 0)
        goto done;
    for (size_t r = (size_t)system + 1; r < plan->tokens[system].next && !code;
         r = plan->tokens[r].next) {
        int entry;
        if (ak_string(plan, ak_member(plan, (int)r, "name"), name,
                      sizeof name) ||
            (entry = entry_find(next, name)) < 0) {
            code = APKG_INVALID;
            break;
        }
        code = table_load(&now, plan, entry_files(plan, entry), 0,
                          c->memory_max);
        if (!code)
            code = strings_load(&list, plan, (int)r, "directories");
        entries += list.count;
        strings_free(&list);
        for (size_t k = 0; k < 3 && !code; k++) {
            code = strings_load(&list, plan, (int)r, placed_lists[k]);
            for (size_t i = 0; i < list.count && !code; i++) {
                const row *n = table_find_path(&now, list.paths[i], scratch);
                struct stat st;
                char sha[65];
                uint64_t size;
                if (!n ||
                    ak_string(plan, ak_member(plan, n->token, "sha256"), sha,
                              sizeof sha) ||
                    ak_u64(plan, ak_member(plan, n->token, "size"), &size) ||
                    ak_path(full, c->root, list.paths[i])) {
                    code = APKG_INVALID;
                    break;
                }
                code = ak_blob_check(c, sha, size, NULL);
                if (code) {
                    refuse(e, code,
                           code == APKG_NOT_FOUND
                               ? "the store no longer holds the bytes of a "
                                 "file to place; nothing was written"
                               : "a stored object does not match its signed "
                                 "hash; nothing was written",
                           list.paths[i]);
                    break;
                }
                if (running + (int64_t)size > peak)
                    peak = running + (int64_t)size;
                running += (int64_t)size;
                if (regular(full, &st) == 1)
                    running -= (int64_t)st.st_size;
                entries++;
            }
            strings_free(&list);
        }
        table_free(&now);
    }
    if (code)
        goto done;
    {
        int state = ak_member(plan, ak_member(plan, 0, "body"), "new_state");
        state_size = plan->tokens[state].end - plan->tokens[state].start;
    }
    if (pkg_fs_free_bytes(c->root, &available, &unit)) {
        code = APKG_IO;
        goto done;
    }
    {
        /* Every file and directory may round up to one allocation unit; the
         * state is written beside the old one before it replaces it. */
        uint64_t needed = (uint64_t)peak + (entries + 4) * unit +
                          2 * state_size + 262144u;
        if (needed > available) {
            char detail[96];
            snprintf(detail, sizeof detail,
                     "%" PRIu64 " bytes needed, %llu free", needed, available);
            code = refuse(e, APKG_LIMIT,
                          "the destination volume has too little free space "
                          "for this placement; nothing was written",
                          detail);
        }
    }
done:
    strings_free(&list);
    table_free(&now);
    free(full);
    free(scratch);
    return code;
}
static const char *const held_lists[] = {"overwrite", "taken", "write",
                                         "delete",    "clear", "rmdir"};
/* Whether something holds what the plan is about to replace or remove. DOS
 * does not delete an object that is open or locked, and placement comes after
 * the decision, where nothing is taken back: a held file is refused here,
 * while the root is still as it was. Each existing file of overwrite, taken,
 * write, delete and clear and each directory of clear and rmdir is asked for
 * with an exclusive lock that is released at once. A holder may still appear
 * afterwards; placement then stops with APKG_BUSY and recovery finishes once
 * the object is free.
 *
 * Passing these probes does not make the running system safe to update:
 * where a replacement is not one step a file is absent under its name between
 * two renames, so the SYS: the system runs from is refused outright. */
apkg_status ak_place_busy(apkg_ctx *c, const ak_json *plan, int point,
                          apkg_error **e) {
    ak_installed next;
    strings list = {0};
    struct stat st;
    char *full = malloc(AK_PATH);
    int system;
    apkg_status code = full ? APKG_OK : APKG_NOMEM;
    if (!code && plan_arrays(plan, &system, &next))
        code = APKG_INVALID;
    if (code || system < 0 || plan->tokens[system].next == (size_t)system + 1)
        goto done;
    if (!pkg_fs_replace_one_step() &&
        (ak_running_system(c->root) || ak_fault(1306))) {
        code = refuse(e, APKG_UNSUPPORTED,
                      "this root is the running system, and a file replaced "
                      "here is absent under its name for a moment; update it "
                      "from another running system",
                      c->root);
        goto done;
    }
    for (size_t r = (size_t)system + 1; r < plan->tokens[system].next && !code;
         r = plan->tokens[r].next)
        for (size_t k = 0; k < sizeof held_lists / sizeof *held_lists && !code;
             k++) {
            code = strings_load(&list, plan, (int)r, held_lists[k]);
            for (size_t i = 0; i < list.count && !code; i++) {
                int held;
                if (ak_path(full, c->root, list.paths[i])) {
                    code = APKG_LIMIT;
                    break;
                }
                held = ak_in_use_object(full);
                /* The test build's stand-in for a holder; only what exists
                 * can be held. */
                if (!held && !lstat(full, &st) && ak_fault(point))
                    held = 1;
                if (held < 0)
                    code = refuse(e, APKG_IO,
                                  "whether this object is in use could not "
                                  "be established; nothing was written",
                                  list.paths[i]);
                else if (held)
                    code = refuse(e, APKG_BUSY,
                                  "in use; close what uses it and repeat the "
                                  "operation. If no program using it is "
                                  "running, a library or driver may still "
                                  "hold it; restarting the system releases "
                                  "it. Nothing was written",
                                  list.paths[i]);
            }
            strings_free(&list);
        }
done:
    strings_free(&list);
    free(full);
    return code;
}
typedef struct {
    char *path;
    mode_t mode;
} restore;
typedef struct {
    apkg_ctx *c;
    const ak_place_progress *progress;
    unsigned char *buffer;
    uint64_t bytes, total_bytes, files, total_files;
    char *full, *temp, *blob, *directory, *synced, *writable, *scratch;
    restore *restores;
    size_t nrestores, crestores;
    ak_buf kept;
    apkg_error **error;
} apply;
static apkg_status sync_change(apply *a, const char *full) {
    if (ak_parent(full, a->directory))
        return APKG_IO;
    if (*a->synced && strcmp(a->synced, a->directory) && ak_sync_dir(a->synced))
        return APKG_IO;
    strcpy(a->synced, a->directory);
    return APKG_OK;
}
static apkg_status sync_last(apply *a) {
    apkg_status code = *a->synced && ak_sync_dir(a->synced) ? APKG_IO : APKG_OK;
    *a->synced = 0;
    return code;
}
/* A signed directory mode may withhold owner write. The directory that is
 * about to change is opened for the duration and gets its mode back last. */
static apkg_status writable(apply *a, const char *full) {
    struct stat st;
    if (ak_parent(full, a->directory))
        return APKG_IO;
    if (!strcmp(a->writable, a->directory))
        return APKG_OK;
    if (stat(a->directory, &st))
        return APKG_IO;
    if (S_ISDIR(st.st_mode) && !(st.st_mode & 0200)) {
        if (a->nrestores == a->crestores) {
            size_t capacity = a->crestores ? a->crestores * 2 : 16;
            restore *grown = calloc(capacity, sizeof *grown);
            if (!grown)
                return APKG_NOMEM;
            if (a->nrestores)
                memcpy(grown, a->restores, a->nrestores * sizeof *grown);
            free(a->restores);
            a->restores = grown;
            a->crestores = capacity;
        }
        a->restores[a->nrestores].path =
            ak_copy(a->directory, strlen(a->directory));
        if (!a->restores[a->nrestores].path)
            return APKG_NOMEM;
        a->restores[a->nrestores++].mode = st.st_mode & 0777;
        if (chmod(a->directory, (st.st_mode & 0777) | 0200))
            return APKG_IO;
    }
    strcpy(a->writable, a->directory);
    return APKG_OK;
}
/* A holder that appeared after the last check. The decision is made, so the
 * transaction stays pending with what is placed so far in place, and recovery
 * finishes it once the object is free. */
static apkg_status held_after(apply *a, const char *relative) {
    return a->error && !*a->error
               ? ak_fail(a->error, APKG_BUSY,
                         "in use, so the update could not finish; close what "
                         "uses it or restart the system, then run recovery",
                         relative)
               : APKG_BUSY;
}
/* The old bytes of a replaced file, left aside under their own name. */
static apkg_status aside_drop(apply *a, const char *aside,
                              const char *relative) {
    int held = ak_fault(210);
    if (!held && !unlink(aside))
        return APKG_OK;
    if (!held)
        held = errno == EBUSY || ak_in_use_object(aside) == 1;
    return held ? held_after(a, relative) : APKG_IO;
}
/* Stream one retained object into the destination directory under the
 * reserved name, check its bytes, then put it in place. Unless the plan
 * said to replace whatever is there (`force`), a file that holds neither the
 * new nor the old signed bytes was edited after the approval: it stays, and
 * the journal records it as kept. */
static apkg_status place(apply *a, const char *relative, const char *sha,
                         uint64_t size, uint64_t mode, const char *old_sha,
                         int force) {
    struct stat st;
    struct pkg_sha256 hash;
    unsigned char digest[32];
    char actual[65], aside[AK_PATH + 24];
    uint64_t copied = 0, reported = a->bytes;
    int present, input = -1, output = -1, has_aside;
    apkg_status code;
    if (ak_path(a->full, a->c->root, relative) ||
        ak_parent(a->full, a->directory) ||
        ak_path(a->temp, a->directory, PLACING) ||
        pkg_fs_aside_name(a->full, aside, sizeof aside))
        return APKG_LIMIT;
    present = regular(a->full, &st);
    if (present < 0)
        return APKG_IO;
    if (present && (!force || (uint64_t)st.st_size == size)) {
        unsigned char head[64];
        size_t head_size = 0;
        uint64_t n;
        mode_t live_mode = st.st_mode & 0777;
        code = ak_stream_hash(a->full, &n, actual, head, &head_size);
        if (code)
            return code;
        if (!strcmp(actual, sha)) {
            /* Done by an earlier attempt; a replacement interrupted after
             * its second step leaves the old bytes aside. */
            if (live_mode != (mode_t)mode) {
                code = writable(a, a->full);
                if (code)
                    return code;
                if (chmod(a->full, (mode_t)mode))
                    return APKG_IO;
            }
            if (regular(aside, &st) == 1) {
                code = writable(a, a->full);
                if (code)
                    return code;
                code = aside_drop(a, aside, relative);
                if (code)
                    return code;
            }
            a->bytes += size;
            return APKG_OK;
        }
        if (!force && !(old_sha && !strcmp(actual, old_sha))) {
            ak_buf_add(&a->kept, "%s\n", relative);
            a->bytes += size;
            return a->kept.bad ? (apkg_status)a->kept.bad : APKG_OK;
        }
    }
    /* Absent with an aside: a replacement stopped between its two renames.
     * Placing the new file finishes it. */
    code = writable(a, a->full);
    if (code)
        return code;
    present = regular(a->temp, &st);
    if (present < 0 || (present && unlink(a->temp)))
        return APKG_IO;
    code = ak_blob_path(a->c, sha, a->blob);
    if (code)
        return code;
    if (pkg_fs_path_is_link(a->blob) != 0)
        return APKG_INTEGRITY;
    input = open(a->blob, O_RDONLY);
    if (input < 0)
        return errno == ENOENT ? APKG_NOT_FOUND : APKG_IO;
    output = open(a->temp, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (output < 0) {
        close(input);
        return APKG_IO;
    }
    pkg_sha256_init(&hash);
    code = APKG_OK;
    for (;;) {
        ssize_t n = read(input, a->buffer, CHUNK);
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
        pkg_sha256_update(&hash, a->buffer, (size_t)n);
        while (done < (size_t)n) {
            ssize_t w = write(output, a->buffer + done, (size_t)n - done);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                code = APKG_IO;
                break;
            }
            done += (size_t)w;
        }
        if (!code && ak_fault(208))
            code = APKG_IO;
        if (code)
            break;
        copied += (uint64_t)n;
        if (a->bytes + copied - reported >= 1048576u) {
            reported = a->bytes + copied;
            progress(a->progress, relative, "bytes", reported, a->total_bytes);
        }
    }
    if (!code) {
        pkg_sha256_final(&hash, digest);
        ak_tohex(digest, 32, actual);
        if (copied != size || strcmp(actual, sha))
            code = APKG_INTEGRITY;
    }
    if (close(input) && !code)
        code = APKG_IO;
    if (!code && fsync(output))
        code = APKG_IO;
    if (close(output) && !code)
        code = APKG_IO;
    if (!code && chmod(a->temp, (mode_t)mode))
        code = APKG_IO;
    /* The mode is metadata of the file about to be published. ak_sync_file
     * is the engine's portable form: fsync() of a descriptor opened for
     * reading fails with EBADF on ABIv11, and there the volume flush after
     * the replacement carries it. */
    if (!code && ak_sync_file(a->temp))
        code = APKG_IO;
    if (!code && ak_fault(202))
        code = APKG_IO;
    has_aside = regular(aside, &st) == 1;
    if (!code && pkg_fs_replace(a->temp, a->full)) {
        /* The new file may already carry the name while the old bytes,
         * still open somewhere, could not be deleted from their aside. */
        int held = errno == EBUSY || ak_in_use_object(aside) == 1;
        code = held && regular(a->temp, &st) == 0 ? held_after(a, relative)
                                                   : APKG_IO;
    }
    if (code) {
        unlink(a->temp);
        return code;
    }
    /* Where the replacement was one atomic step it left an older aside. */
    if (has_aside && regular(aside, &st) == 1) {
        code = aside_drop(a, aside, relative);
        if (code)
            return code;
    }
    if (ak_fault(203))
        return APKG_IO;
    a->bytes += size;
    return sync_change(a, a->full);
}
static apkg_status remove_file(apply *a, const ak_json *oj, const table *was,
                               const char *relative, int strict) {
    const row *o = table_find_path(was, relative, a->scratch);
    struct stat st;
    char sha[65], actual[65];
    unsigned char head[64];
    size_t head_size = 0;
    uint64_t n;
    apkg_status code;
    if (!o || ak_path(a->full, a->c->root, relative))
        return APKG_INVALID;
    if (lstat(a->full, &st))
        return errno == ENOENT || errno == ENOTDIR ? APKG_OK : APKG_IO;
    if (o->directory) {
        if (!S_ISDIR(st.st_mode))
            return APKG_OK; /* already what the new version lists */
        /* Its own pending flush and write permission end with it. */
        if (!strcmp(a->synced, a->full)) {
            code = sync_last(a);
            if (code)
                return code;
        }
        if (!strcmp(a->writable, a->full))
            *a->writable = 0;
        code = writable(a, a->full);
        if (code)
            return code;
        if (rmdir(a->full)) {
            if (!strict)
                return APKG_OK;
            return ak_in_use_object(a->full) == 1 ? held_after(a, relative)
                                                  : APKG_CONFLICT;
        }
        return ak_parent(a->full, a->directory) || ak_sync_dir(a->directory)
                   ? APKG_IO
                   : APKG_OK;
    }
    if (!S_ISREG(st.st_mode))
        return APKG_OK;
    if (ak_string(oj, ak_member(oj, o->token, "sha256"), sha, sizeof sha))
        return APKG_INVALID;
    code = ak_stream_hash(a->full, &n, actual, head, &head_size);
    if (code)
        return code;
    if (strcmp(actual, sha))
        /* No longer the old signed bytes: it is someone's file now. Where
         * a directory must take its place, that blocks the placement. */
        return strict ? APKG_CONFLICT : APKG_OK;
    code = writable(a, a->full);
    if (code)
        return code;
    {
        int held = ak_fault(211);
        if (held || unlink(a->full))
            return held || errno == EBUSY || ak_in_use_object(a->full) == 1
                       ? held_after(a, relative)
                       : APKG_IO;
    }
    return sync_change(a, a->full);
}
/* Every step leaves the root one step closer and can run again: a file that
 * already holds the signed bytes is done, a dropped file that no longer
 * holds its old bytes stays, a directory that is not empty stays. */
apkg_status ak_place_apply(apkg_ctx *c, const ak_json *plan,
                           const void *old_state, size_t old_size,
                           const char *journal,
                           const ak_place_progress *observer,
                           apkg_error **e) {
    ak_json before = {0};
    ak_installed old = {0}, next;
    apply a = {0};
    table now = {0}, was = {0};
    strings list = {0};
    char name[256];
    int system, parsed;
    apkg_status code = APKG_OK;
    if (plan_arrays(plan, &system, &next))
        return APKG_INVALID;
    if (system < 0)
        return APKG_OK;
    parsed = state_installed(old_state, old_size, &before, &old);
    if (parsed) {
        ak_json_free(&before);
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    }
    a.c = c;
    a.error = e;
    a.progress = observer;
    a.kept.max = c->object_max;
    a.buffer = malloc(CHUNK);
    a.full = malloc(AK_PATH);
    a.temp = malloc(AK_PATH);
    a.blob = malloc(AK_PATH);
    a.directory = malloc(AK_PATH);
    a.synced = calloc(1, AK_PATH);
    a.writable = calloc(1, AK_PATH);
    a.scratch = malloc(AK_PATH);
    if (!a.buffer || !a.full || !a.temp || !a.blob || !a.directory ||
        !a.synced || !a.writable || !a.scratch) {
        code = APKG_NOMEM;
        goto done;
    }
    for (size_t r = (size_t)system + 1; r < plan->tokens[system].next;
         r = plan->tokens[r].next) {
        uint64_t bytes;
        if (ak_u64(plan, ak_member(plan, (int)r, "bytes"), &bytes) ||
            bytes > UINT64_MAX - a.total_bytes) {
            code = APKG_INVALID;
            goto done;
        }
        a.total_bytes += bytes;
        for (size_t k = 0; k < 3; k++) {
            int l = ak_member(plan, (int)r, placed_lists[k]);
            if (l < 0) {
                code = APKG_INVALID;
                goto done;
            }
            for (size_t i = (size_t)l + 1; i < plan->tokens[l].next;
                 i = plan->tokens[i].next)
                a.total_files++;
        }
    }
    progress(observer, c->root, "bytes", 0, a.total_bytes);
    for (int pass = 0; pass < 3; pass++) {
        /* 0: make way and place; 1: delete what the new list dropped, once
         * every new file of every package is in place; 2: signed modes. */
        for (size_t r = (size_t)system + 1; r < plan->tokens[system].next;
             r = plan->tokens[r].next) {
            const ak_json *nj = next.j, *oj = old.j;
            int entry, was_entry;
            if (ak_string(plan, ak_member(plan, (int)r, "name"), name,
                          sizeof name) ||
                (entry = entry_find(next, name)) < 0) {
                code = APKG_INVALID;
                goto done;
            }
            was_entry = entry_find(old, name);
            code = table_load(&now, nj, entry_files(nj, entry), 0,
                              c->memory_max);
            if (!code)
                code = table_load(&was, oj,
                                  was_entry < 0 ? -1
                                                : entry_files(oj, was_entry),
                                  0, c->memory_max);
            if (code)
                goto done;
            if (pass == 0) {
                /* A path that changes type goes first, children before
                 * their directory, and only as the old version left it. */
                code = strings_load(&list, plan, (int)r, "clear");
                for (size_t i = list.count; !code && i > 0; i--) {
                    code = remove_file(&a, oj, &was, list.paths[i - 1], 1);
                    if (!code && ak_fault(207))
                        code = APKG_IO;
                }
                strings_free(&list);
                if (!code)
                    code = sync_last(&a);
                if (!code)
                    code = strings_load(&list, plan, (int)r, "directories");
                for (size_t i = 0; !code && i < list.count; i++) {
                    struct stat st;
                    uint64_t mode = 0700;
                    const row *n = table_find_path(&now, list.paths[i], a.scratch);
                    int token = n ? ak_member(nj, n->token, "mode") : -1;
                    if (!n || ak_path(a.full, c->root, list.paths[i]) ||
                        (token >= 0 &&
                         (ak_u64(nj, token, &mode) || mode > 0777))) {
                        code = APKG_INVALID;
                        break;
                    }
                    /* Created writable whatever its signed mode, which is
                     * applied when nothing is left to put inside. Presence
                     * is tested, never read from mkdir's errno: AROS posixc
                     * does not report an existing directory as EEXIST. */
                    if (lstat(a.full, &st)) {
                        code = errno == ENOENT ? writable(&a, a.full) : APKG_IO;
                        if (!code && mkdir(a.full, (mode_t)mode | 0700))
                            code = APKG_IO;
                        if (!code)
                            code = sync_change(&a, a.full);
                    } else if (!S_ISDIR(st.st_mode))
                        code = APKG_IO;
                    else if ((st.st_mode & 0700) != 0700 &&
                             chmod(a.full, (st.st_mode & 0777) | 0700))
                        code = APKG_IO;
                }
                strings_free(&list);
                if (!code && ak_fault(201))
                    code = APKG_IO;
                for (size_t k = 0; k < 3 && !code; k++) {
                    code = strings_load(&list, plan, (int)r, placed_lists[k]);
                    for (size_t i = 0; !code && i < list.count; i++) {
                        const row *n = table_find_path(&now, list.paths[i],
                                                       a.scratch),
                                  *o;
                        char sha[65], old_sha[65];
                        uint64_t size, mode;
                        if (!n || n->directory ||
                            ak_string(nj, ak_member(nj, n->token, "sha256"),
                                      sha, sizeof sha) ||
                            ak_u64(nj, ak_member(nj, n->token, "size"),
                                   &size) ||
                            ak_u64(nj, ak_member(nj, n->token, "mode"),
                                   &mode) ||
                            mode > 0777) {
                            code = APKG_INVALID;
                            break;
                        }
                        o = table_find(&was, n->fold);
                        if (o && !o->directory &&
                            ak_string(oj, ak_member(oj, o->token, "sha256"),
                                      old_sha, sizeof old_sha)) {
                            code = APKG_INVALID;
                            break;
                        }
                        code = place(&a, list.paths[i], sha, size, mode,
                                     o && !o->directory ? old_sha : NULL,
                                     k < 2);
                        if (code)
                            break;
                        a.files++;
                        progress(observer, list.paths[i], "bytes", a.bytes,
                                 a.total_bytes);
                        progress(observer, list.paths[i], "files", a.files,
                                 a.total_files);
                    }
                    strings_free(&list);
                }
            } else if (pass == 1) {
                code = strings_load(&list, plan, (int)r, "delete");
                for (size_t i = 0; !code && i < list.count; i++) {
                    code = remove_file(&a, oj, &was, list.paths[i], 0);
                    if (!code && ak_fault(205))
                        code = APKG_IO;
                }
                strings_free(&list);
                /* A flushed directory may be the next to go. */
                if (!code)
                    code = sync_last(&a);
                if (!code && ak_fault(209))
                    code = APKG_IO;
                /* Children sort after their parents: remove from the far
                 * end. A directory that is not empty, or already gone,
                 * simply stays. */
                if (!code)
                    code = strings_load(&list, plan, (int)r, "rmdir");
                for (size_t i = list.count; !code && i > 0; i--)
                    code = remove_file(&a, oj, &was, list.paths[i - 1], 0);
                strings_free(&list);
            } else {
                code = strings_load(&list, plan, (int)r, "directories");
                for (size_t i = 0; !code && i < list.count; i++) {
                    uint64_t mode = 0700;
                    const row *n = table_find_path(&now, list.paths[i], a.scratch);
                    int token = n ? ak_member(nj, n->token, "mode") : -1;
                    if (!n || ak_path(a.full, c->root, list.paths[i]) ||
                        (token >= 0 && ak_u64(nj, token, &mode)))
                        code = APKG_INVALID;
                    else if (chmod(a.full, (mode_t)mode))
                        code = APKG_IO;
                }
                strings_free(&list);
            }
            table_free(&now);
            table_free(&was);
            if (code)
                goto done;
        }
        code = sync_last(&a);
        if (!code && pass == 0 && ak_fault(204))
            code = APKG_IO;
        if (code)
            goto done;
    }
    for (size_t i = a.nrestores; i > 0; i--)
        if (chmod(a.restores[i - 1].path, a.restores[i - 1].mode) &&
            errno != ENOENT) /* it may have been one of those removed */
            code = APKG_IO;
    if (!code && a.kept.size && journal) {
        /* What recovery left as its user had edited it meanwhile. */
        char path[AK_PATH];
        if (ak_path(path, journal, "kept") ||
            (ak_exists(path) == 1 && unlink(path)) ||
            ak_write(path, a.kept.data, a.kept.size, 1) ||
            ak_sync_dir(journal))
            code = APKG_IO;
    }
    if (!code && ak_fault(206))
        code = APKG_IO;
done:
    strings_free(&list);
    table_free(&now);
    table_free(&was);
    for (size_t i = 0; i < a.nrestores; i++)
        free(a.restores[i].path);
    free(a.restores);
    free(a.kept.data);
    free(a.buffer);
    free(a.full);
    free(a.temp);
    free(a.blob);
    free(a.directory);
    free(a.synced);
    free(a.writable);
    free(a.scratch);
    ak_json_free(&before);
    return code;
}

/* Presentation of the approved per-file decisions. */
apkg_status ak_place_format(const apkg_plan *p, ak_buf *b) {
    static const char *const lists[] = {"directories", "write",   "overwrite",
                                        "taken",       "keep",    "delete",
                                        "release",     "rmdir",   "clear"};
    static const char *const labels[] = {
        "Create directory",
        "Write",
        "Overwrite modified file",
        "Take over existing file",
        "Keep edited configuration",
        "Delete",
        "Release (kept, no longer owned)",
        "Remove directory if empty",
        "Remove first (its path changes type)"};
    ak_json system = {0}, state = {0};
    ak_installed next;
    cursor *paths = NULL, *files = NULL;
    apkg_status code = APKG_OK;
    int parsed;
    if (!p->system)
        return APKG_OK;
    parsed = ak_json_parse(p->system, strlen(p->system), &system);
    if (!parsed)
        parsed = state_installed(p->root_record, strlen(p->root_record), &state,
                                 &next);
    paths = malloc(sizeof *paths);
    files = malloc(sizeof *files);
    if (parsed || !paths || !files) {
        code = parsed == -1 ? APKG_INVALID : APKG_NOMEM;
        goto done;
    }
    for (size_t r = 1; r < system.tokens[0].next; r = system.tokens[r].next) {
        char name[256], operation[16], bytes[24];
        int entry;
        if (ak_string(&system, ak_member(&system, (int)r, "name"), name,
                      sizeof name) ||
            ak_string(&system, ak_member(&system, (int)r, "operation"),
                      operation, sizeof operation) ||
            ak_string(&system, ak_member(&system, (int)r, "bytes"), bytes,
                      sizeof bytes) ||
            (entry = entry_find(next, name)) < 0) {
            code = APKG_INVALID;
            goto done;
        }
        ak_buf_add(b,
                   "\nSystem package: %s (%s)\nFiles are placed one by one in "
                   "the root itself; no previous copy is kept and the change "
                   "cannot be rolled back.\nTakeover of existing unowned "
                   "files: %s\nBytes to write: %s\n",
                   name, operation,
                   ak_equal(&system, ak_member(&system, (int)r, "takeover"),
                            "1")
                       ? "requested"
                       : "not requested",
                   bytes);
        for (size_t k = 0; k < 9; k++) {
            if (cursor_open(paths, &system,
                            ak_member(&system, (int)r, lists[k]), 1) ||
                cursor_open(files, &state, entry_files(&state, entry), 0)) {
                code = APKG_INVALID;
                goto done;
            }
            for (; paths->valid;) {
                ak_buf_add(b, "%s %s", labels[k], paths->path);
                if (k >= 1 && k <= 3 && cursor_seek(files, paths->path) == 1) {
                    char sha[65], size[24], mode[24];
                    if (ak_string(&state,
                                  ak_member(&state, (int)files->at, "sha256"),
                                  sha, sizeof sha) ||
                        ak_string(&state,
                                  ak_member(&state, (int)files->at, "size"),
                                  size, sizeof size) ||
                        ak_string(&state,
                                  ak_member(&state, (int)files->at, "mode"),
                                  mode, sizeof mode)) {
                        code = APKG_INVALID;
                        goto done;
                    }
                    ak_buf_add(b, "; %s bytes, mode %s, SHA-256 %s", size, mode,
                               sha);
                }
                ak_buf_add(b, "\n");
                if (cursor_next(paths)) {
                    code = APKG_INVALID;
                    goto done;
                }
            }
        }
    }
done:
    free(paths);
    free(files);
    ak_json_free(&system);
    ak_json_free(&state);
    return code;
}
/* Per-file report of an installed system package. An edited configuration
 * file is the user's, not damage. */
apkg_status ak_place_verify(apkg_ctx *c, const ak_json *descriptor,
                             const apkg_observer *observer, uint64_t counts[4],
                             ak_buf *rows) {
    int files =
        ak_member(descriptor, ak_member(descriptor, 0, "body"), "files");
    char *path = malloc(AK_PATH), *full = malloc(AK_PATH);
    apkg_status code = APKG_OK;
    if (!path || !full || files < 0) {
        free(path);
        free(full);
        return files < 0 ? APKG_INVALID : APKG_NOMEM;
    }
    for (size_t f = (size_t)files + 1;
         f < descriptor->tokens[files].next && !code;
         f = descriptor->tokens[f].next) {
        char live[65];
        uint64_t size;
        int kind, state, directory, config;
        if (ak_cancelled(observer)) {
            code = APKG_CANCELLED;
            break;
        }
        if (ak_string(descriptor, ak_member(descriptor, (int)f, "path"), path,
                      AK_PATH)) {
            code = APKG_INVALID;
            break;
        }
        code = live_read(c->root, path, full, &kind, live, &size);
        if (code)
            break;
        directory = ak_equal(descriptor, ak_member(descriptor, (int)f, "type"),
                             "directory");
        config = ak_equal(descriptor, ak_member(descriptor, (int)f, "config"),
                          "preserve");
        if (kind == LIVE_ABSENT)
            state = 3;
        else if (directory)
            state = kind == LIVE_DIRECTORY ? 0 : 1;
        else if (kind == LIVE_FILE &&
                 ak_equal(descriptor, ak_member(descriptor, (int)f, "sha256"),
                          live))
            state = 0;
        else
            state = config && kind == LIVE_FILE ? 2 : 1;
        counts[state]++;
        ak_buf_add(rows, "%s %s%s\n",
                   (const char *const[]){"ok", "changed", "edited",
                                         "missing"}[state],
                   path, config ? " [configuration]" : "");
    }
    free(path);
    free(full);
    return code;
}
