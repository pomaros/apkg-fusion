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

/* Paths other system packages of the resulting root own. */
typedef struct {
    char *path;
    int directory;
} owned;
static int owned_compare(const void *a, const void *b) {
    return strcmp(((const owned *)a)->path, ((const owned *)b)->path);
}
static void owned_free(owned *rows, size_t count) {
    if (rows)
        for (size_t i = 0; i < count; i++)
            free(rows[i].path);
    free(rows);
}
static apkg_status owned_build(ak_installed next, int self, size_t max,
                               owned **out, size_t *count) {
    owned *rows = NULL;
    size_t n = 0, k = 0;
    char *path = NULL;
    apkg_status code = APKG_OK;
    *out = NULL;
    *count = 0;
    for (int pass = 0; pass < 2 && !code; pass++) {
        for (size_t e = (size_t)next.array + 1;
             e < next.j->tokens[next.array].next && !code;
             e = next.j->tokens[e].next) {
            int files;
            if ((int)e == self || !entry_system(next.j, (int)e))
                continue;
            files = entry_files(next.j, (int)e);
            for (size_t f = (size_t)files + 1;
                 f < next.j->tokens[files].next; f = next.j->tokens[f].next) {
                if (!pass) {
                    n++;
                    continue;
                }
                if (ak_string(next.j, ak_member(next.j, (int)f, "path"), path,
                              AK_PATH)) {
                    code = APKG_INVALID;
                    break;
                }
                rows[k].path = ak_copy(path, strlen(path));
                if (!rows[k].path) {
                    code = APKG_NOMEM;
                    break;
                }
                rows[k++].directory = ak_equal(
                    next.j, ak_member(next.j, (int)f, "type"), "directory");
            }
        }
        if (!pass) {
            if (!n)
                return APKG_OK;
            if (n > max / sizeof *rows)
                return APKG_LIMIT;
            rows = calloc(n, sizeof *rows);
            path = malloc(AK_PATH);
            if (!rows || !path)
                code = APKG_NOMEM;
        }
    }
    free(path);
    if (code) {
        owned_free(rows, k);
        return code;
    }
    qsort(rows, k, sizeof *rows, owned_compare);
    *out = rows;
    *count = k;
    return APKG_OK;
}

enum {
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
    cursor was, now, created;
    char full[AK_PATH];
    ak_buf lists[L_COUNT];
} scan;
static void list_add(ak_buf *list, const char *path) {
    /* Valid payload paths hold no quote, backslash or control byte. */
    ak_buf_add(list, "%s\"%s\"", list->size ? "," : "", path);
}
static apkg_status refuse(apkg_error **e, apkg_status code, const char *why,
                          const char *subject) {
    if (e && !*e)
        ak_fail(e, code, why, subject);
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
    scan *w = calloc(1, sizeof *w);
    owned *foreign = NULL;
    size_t nforeign = 0, at = 0;
    struct pkg_sha256 observed;
    unsigned char digest[32];
    char hex[65];
    uint64_t bytes = 0;
    int repair = !strcmp(operation, "repair");
    apkg_status code;
    if (!w)
        return APKG_NOMEM;
    for (int i = 0; i < L_COUNT; i++)
        w->lists[i].max = max;
    code = owned_build(next, new_entry, max, &foreign, &nforeign);
    if (code)
        goto done;
    if (cursor_open(&w->was, oj, old_entry < 0 ? -1 : entry_files(oj, old_entry),
                    0) ||
        cursor_open(&w->now, nj, entry_files(nj, new_entry), 0) ||
        cursor_open(&w->created, oj,
                    old_entry < 0
                        ? -1
                        : ak_member(oj, ak_member(oj, old_entry, "placement"),
                                    "directories"),
                    1)) {
        code = APKG_INVALID;
        goto done;
    }
    pkg_sha256_init(&observed);
    while (w->was.valid || w->now.valid) {
        int order = !w->was.valid   ? 1
                    : !w->now.valid ? -1
                                    : strcmp(w->was.path, w->now.path);
        int in_old = order <= 0, in_new = order >= 0, kind;
        const char *path = in_new ? w->now.path : w->was.path;
        int ot = in_old ? (int)w->was.at : -1, nt = in_new ? (int)w->now.at : -1;
        int old_file = in_old && ak_equal(oj, ak_member(oj, ot, "type"), "file");
        int new_dir =
            in_new && ak_equal(nj, ak_member(nj, nt, "type"), "directory");
        int was_created, taken_elsewhere = 0, elsewhere_directory = 0;
        char live[65], sha[65] = "", before[65] = "";
        uint64_t live_size, size = 0;
        if (in_new && ak_member(nj, nt, "amiga") >= 0) {
            code = refuse(e, APKG_UNSUPPORTED,
                          "system packages carry no amiga metadata", path);
            goto done;
        }
        code = live_read(c->root, path, w->full, &kind, live, &live_size);
        if (code)
            goto done;
        pkg_sha256_update(&observed, (const unsigned char *)path,
                          strlen(path) + 1);
        pkg_sha256_update(&observed, (const unsigned char *)"adfo" + kind, 1);
        pkg_sha256_update(&observed, (const unsigned char *)live,
                          strlen(live) + 1);
        was_created = cursor_seek(&w->created, path);
        if (was_created < 0) {
            code = APKG_INVALID;
            goto done;
        }
        while (at < nforeign && strcmp(foreign[at].path, path) < 0)
            at++;
        if (at < nforeign && !strcmp(foreign[at].path, path)) {
            taken_elsewhere = 1;
            elsewhere_directory = foreign[at].directory;
        }
        if (in_new && taken_elsewhere && !(new_dir && elsewhere_directory)) {
            code = refuse(e, APKG_CONFLICT,
                          "another installed system package owns this path",
                          path);
            goto done;
        }
        if (new_dir) {
            if (kind == LIVE_ABSENT) {
                list_add(&w->lists[L_DIRECTORIES], path);
                list_add(&w->lists[L_PLACEMENT], path);
            } else if (kind != LIVE_DIRECTORY) {
                code = refuse(e, APKG_CONFLICT,
                              "a system directory path holds something else",
                              path);
                goto done;
            } else if (was_created)
                list_add(&w->lists[L_PLACEMENT], path);
        } else if (in_new) {
            if (ak_string(nj, ak_member(nj, nt, "sha256"), sha, sizeof sha) ||
                ak_u64(nj, ak_member(nj, nt, "size"), &size) ||
                (old_file &&
                 ak_string(oj, ak_member(oj, ot, "sha256"), before,
                           sizeof before))) {
                code = APKG_INVALID;
                goto done;
            }
            if (kind == LIVE_DIRECTORY || kind == LIVE_OTHER) {
                code = refuse(e, APKG_CONFLICT,
                              "a system file path holds something else", path);
                goto done;
            }
            if (kind == LIVE_ABSENT) {
                list_add(&w->lists[L_WRITE], path);
                bytes += size;
            } else if (!strcmp(live, sha)) {
                /* Already the signed bytes: owned without a write. */
            } else if (old_file) {
                if (!repair && !strcmp(live, before)) {
                    list_add(&w->lists[L_WRITE], path);
                    bytes += size;
                } else if (ak_equal(nj, ak_member(nj, nt, "config"),
                                    "preserve"))
                    list_add(&w->lists[L_KEEP], path);
                else {
                    list_add(&w->lists[L_OVERWRITE], path);
                    bytes += size;
                }
            } else if (takeover) {
                list_add(&w->lists[L_TAKEN], path);
                bytes += size;
            } else {
                code = refuse(e, APKG_CONFLICT,
                              "an unowned file with other content is in the "
                              "way; takeover was not requested",
                              path);
                goto done;
            }
        } else if (old_file) {
            if (ak_string(oj, ak_member(oj, ot, "sha256"), before,
                          sizeof before)) {
                code = APKG_INVALID;
                goto done;
            }
            /* A path another package of the same plan now lists is handed
             * over, not dropped: deleting it would take that package's file. */
            if (kind == LIVE_FILE && !taken_elsewhere)
                list_add(&w->lists[strcmp(live, before) ? L_RELEASE : L_DELETE],
                         path);
        } else if (was_created && kind == LIVE_DIRECTORY && !taken_elsewhere)
            list_add(&w->lists[L_RMDIR], path);
        if ((order <= 0 && cursor_next(&w->was)) ||
            (order >= 0 && cursor_next(&w->now))) {
            code = APKG_INVALID;
            goto done;
        }
    }
    pkg_sha256_final(&observed, digest);
    ak_tohex(digest, 32, hex);
    for (int i = 0; i < L_COUNT; i++)
        if (w->lists[i].bad) {
            code = (apkg_status)w->lists[i].bad;
            goto done;
        }
#define LIST(i) (w->lists[i].data ? w->lists[i].data : "")
    ak_buf_add(record,
               "%s{\"bytes\":\"%" PRIu64 "\",\"delete\":[%s],\"directories\":"
               "[%s],\"keep\":[%s],\"name\":\"%s\",\"observed\":\"%s\","
               "\"operation\":\"%s\",\"overwrite\":[%s],\"release\":[%s],"
               "\"rmdir\":[%s],\"taken\":[%s],\"takeover\":\"%d\",\"write\":"
               "[%s]}",
               record->size > 1 ? "," : "", bytes, LIST(L_DELETE),
               LIST(L_DIRECTORIES), LIST(L_KEEP), name, hex, operation,
               LIST(L_OVERWRITE), LIST(L_RELEASE), LIST(L_RMDIR),
               LIST(L_TAKEN), !!takeover, LIST(L_WRITE));
    ak_buf_add(placement, "{\"directories\":[%s]}", LIST(L_PLACEMENT));
#undef LIST
    code = record->bad      ? (apkg_status)record->bad
           : placement->bad ? (apkg_status)placement->bad
                            : APKG_OK;
done:
    for (int i = 0; i < L_COUNT; i++)
        free(w->lists[i].data);
    owned_free(foreign, nforeign);
    free(w);
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
        if (parsed)
            return parsed == -2 ? APKG_NOMEM : APKG_TRUST;
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
        "bytes",     "delete",    "directories", "keep",     "name",
        "observed",  "operation", "overwrite",   "release",  "rmdir",
        "takeover",  "taken",     "write"};
    static const char *const lists[] = {"delete",    "directories", "keep",
                                        "overwrite", "release",     "rmdir",
                                        "taken",     "write"};
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
        if (!ak_fields(j, (int)i, fields, 13) ||
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
        for (size_t k = 0; k < 8; k++) {
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
typedef struct {
    apkg_ctx *c;
    const ak_place_progress *progress;
    unsigned char *buffer;
    uint64_t bytes, total_bytes, files, total_files;
    char *full, *temp, *blob, *directory, *synced;
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
static int regular(const char *path, struct stat *st) {
    if (lstat(path, st))
        return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    return S_ISREG(st->st_mode) ? 1 : -1;
}
/* Stream one retained object into the destination directory under the
 * reserved name, check its bytes, then put it in place. */
static apkg_status place(apply *a, const char *relative, const char *sha,
                         uint64_t size, uint64_t mode) {
    struct stat st;
    struct pkg_sha256 hash;
    unsigned char digest[32];
    char actual[65];
    uint64_t copied = 0, reported = a->bytes;
    int present, input = -1, output = -1;
    apkg_status code;
    if (ak_path(a->full, a->c->root, relative) ||
        ak_parent(a->full, a->directory) ||
        ak_path(a->temp, a->directory, PLACING))
        return APKG_LIMIT;
    present = regular(a->full, &st);
    if (present < 0)
        return APKG_IO;
    if (present && (uint64_t)st.st_size == size) {
        unsigned char head[64];
        size_t head_size = 0;
        uint64_t n;
        code = ak_stream_hash(a->full, &n, actual, head, &head_size);
        if (code)
            return code;
        if (!strcmp(actual, sha)) {
            /* Done by an earlier attempt; a replacement interrupted after
             * its second step leaves the old bytes aside. */
            char aside[AK_PATH + 24];
            if ((st.st_mode & 0777) != (mode_t)mode &&
                chmod(a->full, (mode_t)mode))
                return APKG_IO;
            if (!pkg_fs_aside_name(a->full, aside, sizeof aside) &&
                regular(aside, &st) == 1 && unlink(aside))
                return APKG_IO;
            a->bytes += size;
            return APKG_OK;
        }
    }
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
    if (!code) {
        /* The mode is metadata of the file about to be published. */
        int fd = open(a->temp, O_RDONLY);
        if (fd < 0 || fsync(fd))
            code = APKG_IO;
        if (fd >= 0 && close(fd) && !code)
            code = APKG_IO;
    }
    if (!code && ak_fault(202))
        code = APKG_IO;
    if (!code && pkg_fs_replace(a->temp, a->full))
        code = APKG_IO;
    if (code) {
        unlink(a->temp);
        return code;
    }
    if (ak_fault(203))
        return APKG_IO;
    a->bytes += size;
    return sync_change(a, a->full);
}
static apkg_status place_list(apply *a, const ak_json *plan, int record,
                              const char *list, ak_installed next, int entry,
                              cursor *paths, cursor *files) {
    const ak_json *nj = next.j;
    if (cursor_open(paths, plan, ak_member(plan, record, list), 1) ||
        cursor_open(files, nj, entry_files(nj, entry), 0))
        return APKG_INVALID;
    for (; paths->valid;) {
        char sha[65];
        uint64_t size, mode;
        int token;
        apkg_status code;
        if (cursor_seek(files, paths->path) != 1)
            return APKG_INVALID;
        token = (int)files->at;
        if (!ak_equal(nj, ak_member(nj, token, "type"), "file") ||
            ak_string(nj, ak_member(nj, token, "sha256"), sha, sizeof sha) ||
            ak_u64(nj, ak_member(nj, token, "size"), &size) ||
            ak_u64(nj, ak_member(nj, token, "mode"), &mode) || mode > 0777)
            return APKG_INVALID;
        code = place(a, paths->path, sha, size, mode);
        if (code)
            return code;
        a->files++;
        progress(a->progress, paths->path, "bytes", a->bytes, a->total_bytes);
        progress(a->progress, paths->path, "files", a->files, a->total_files);
        if (cursor_next(paths))
            return APKG_INVALID;
    }
    return APKG_OK;
}
static size_t list_count(const ak_json *j, int list) {
    size_t n = 0;
    if (list >= 0)
        for (size_t i = (size_t)list + 1; i < j->tokens[list].next;
             i = j->tokens[i].next)
            n++;
    return n;
}
/* Every step leaves the root one step closer and can run again: a file that
 * already holds the signed bytes is done, a dropped file that no longer
 * holds its old bytes stays, a directory that is not empty stays. */
apkg_status ak_place_apply(apkg_ctx *c, const ak_json *plan,
                            const void *old_state, size_t old_size,
                            const ak_place_progress *observer) {
    static const char *const writes[] = {"overwrite", "taken", "write"};
    ak_json before = {0};
    ak_installed old = {0}, next;
    apply a = {0};
    cursor *paths = NULL, *files = NULL;
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
    a.progress = observer;
    a.buffer = malloc(CHUNK);
    a.full = malloc(AK_PATH);
    a.temp = malloc(AK_PATH);
    a.blob = malloc(AK_PATH);
    a.directory = malloc(AK_PATH);
    a.synced = calloc(1, AK_PATH);
    paths = malloc(sizeof *paths);
    files = malloc(sizeof *files);
    if (!a.buffer || !a.full || !a.temp || !a.blob || !a.directory ||
        !a.synced || !paths || !files) {
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
        for (size_t k = 0; k < 3; k++)
            a.total_files +=
                list_count(plan, ak_member(plan, (int)r, writes[k]));
    }
    progress(observer, c->root, "bytes", 0, a.total_bytes);
    for (size_t r = (size_t)system + 1; r < plan->tokens[system].next;
         r = plan->tokens[r].next) {
        const ak_json *nj = next.j;
        int entry;
        if (ak_string(plan, ak_member(plan, (int)r, "name"), name,
                      sizeof name) ||
            (entry = entry_find(next, name)) < 0 ||
            cursor_open(paths, plan, ak_member(plan, (int)r, "directories"),
                        1) ||
            cursor_open(files, nj, entry_files(nj, entry), 0)) {
            code = APKG_INVALID;
            goto done;
        }
        for (; paths->valid;) {
            struct stat st;
            uint64_t mode = 0700;
            int token;
            if (cursor_seek(files, paths->path) != 1 ||
                ak_path(a.full, c->root, paths->path)) {
                code = APKG_INVALID;
                goto done;
            }
            token = ak_member(nj, (int)files->at, "mode");
            if (token >= 0 && (ak_u64(nj, token, &mode) || mode > 0777)) {
                code = APKG_INVALID;
                goto done;
            }
            /* Presence is tested, never read from mkdir's errno: AROS posixc
             * does not report an existing directory as EEXIST. */
            if (lstat(a.full, &st)) {
                if (errno != ENOENT || mkdir(a.full, (mode_t)mode)) {
                    code = APKG_IO;
                    goto done;
                }
                code = sync_change(&a, a.full);
                if (code)
                    goto done;
            } else if (!S_ISDIR(st.st_mode)) {
                code = APKG_IO;
                goto done;
            }
            if (cursor_next(paths)) {
                code = APKG_INVALID;
                goto done;
            }
        }
        if (ak_fault(201)) {
            code = APKG_IO;
            goto done;
        }
        for (size_t k = 0; k < 3; k++) {
            code = place_list(&a, plan, (int)r, writes[k], next, entry, paths,
                              files);
            if (code)
                goto done;
        }
    }
    code = sync_last(&a);
    if (!code && ak_fault(204))
        code = APKG_IO;
    if (code)
        goto done;
    /* Nothing goes before every new file is in place. */
    for (size_t r = (size_t)system + 1; r < plan->tokens[system].next;
         r = plan->tokens[r].next) {
        const ak_json *oj = old.j;
        int entry, list = ak_member(plan, (int)r, "rmdir");
        size_t count = list_count(plan, list);
        if (ak_string(plan, ak_member(plan, (int)r, "name"), name,
                      sizeof name) ||
            cursor_open(paths, plan, ak_member(plan, (int)r, "delete"), 1)) {
            code = APKG_INVALID;
            goto done;
        }
        entry = entry_find(old, name);
        if (cursor_open(files, oj, entry < 0 ? -1 : entry_files(oj, entry),
                        0)) {
            code = APKG_INVALID;
            goto done;
        }
        for (; paths->valid;) {
            struct stat st;
            char sha[65], actual[65];
            unsigned char head[64];
            size_t head_size = 0;
            uint64_t n;
            int present;
            if (cursor_seek(files, paths->path) != 1 ||
                ak_string(oj, ak_member(oj, (int)files->at, "sha256"), sha,
                          sizeof sha) ||
                ak_path(a.full, c->root, paths->path)) {
                code = APKG_INVALID;
                goto done;
            }
            present = regular(a.full, &st);
            if (present == 1) {
                code = ak_stream_hash(a.full, &n, actual, head, &head_size);
                if (code)
                    goto done;
                if (!strcmp(actual, sha)) {
                    if (unlink(a.full)) {
                        code = APKG_IO;
                        goto done;
                    }
                    code = sync_change(&a, a.full);
                    if (code)
                        goto done;
                }
            }
            if (ak_fault(205)) {
                code = APKG_IO;
                goto done;
            }
            if (cursor_next(paths)) {
                code = APKG_INVALID;
                goto done;
            }
        }
        /* Children sort after their parents: remove from the far end. A
         * directory that is not empty, or already gone, simply stays. */
        code = sync_last(&a); /* a flushed directory may be the next to go */
        if (code)
            goto done;
        if (count) {
            size_t *at = calloc(count, sizeof *at), k = 0;
            if (!at) {
                code = APKG_NOMEM;
                goto done;
            }
            for (size_t t = (size_t)list + 1; t < plan->tokens[list].next;
                 t = plan->tokens[t].next)
                at[k++] = t;
            while (k-- > 0 && !code) {
                if (ak_string(plan, (int)at[k], paths->path, AK_PATH) ||
                    ak_path(a.full, c->root, paths->path))
                    code = APKG_INVALID;
                else if (!rmdir(a.full) &&
                         (ak_parent(a.full, a.directory) ||
                          ak_sync_dir(a.directory)))
                    code = APKG_IO;
            }
            free(at);
            if (code)
                goto done;
        }
    }
    code = sync_last(&a);
    if (!code && ak_fault(206))
        code = APKG_IO;
done:
    free(a.buffer);
    free(a.full);
    free(a.temp);
    free(a.blob);
    free(a.directory);
    free(a.synced);
    free(paths);
    free(files);
    ak_json_free(&before);
    return code;
}

/* Presentation of the approved per-file decisions. */
apkg_status ak_place_format(const apkg_plan *p, ak_buf *b) {
    static const char *const lists[] = {"directories", "write",   "overwrite",
                                        "taken",       "keep",    "delete",
                                        "release",     "rmdir"};
    static const char *const labels[] = {
        "Create directory",
        "Write",
        "Overwrite modified file",
        "Take over existing file",
        "Keep edited configuration",
        "Delete",
        "Release (kept, no longer owned)",
        "Remove directory if empty"};
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
        for (size_t k = 0; k < 8; k++) {
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
