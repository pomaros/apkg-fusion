/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Context/snapshot model follows arospkg core.c; portable services from pkg. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "internal.h"
#include "pkg_fs.h"
#include "pkg_target.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *ak_copy(const void *v, size_t n) {
    char *p;
    if (n == SIZE_MAX || (!v && n))
        return NULL;
    p = malloc(n + 1);
    if (p) {
        if (n)
            memcpy(p, v, n);
        p[n] = 0;
    }
    return p;
}
void ak_hash(const void *p, size_t n, apkg_digest *d) {
    struct pkg_sha256 h;
    pkg_sha256_init(&h);
    pkg_sha256_update(&h, p, n);
    pkg_sha256_final(&h, d->bytes);
}
apkg_status ak_fail(apkg_error **e, apkg_status c, const char *msg,
                    const char *subject) {
    if (e) {
        *e = calloc(1, sizeof **e);
        if (*e) {
            (*e)->message = ak_copy(msg, strlen(msg));
            (*e)->subject = ak_copy(subject, strlen(subject));
            (*e)->view.code = c;
            if ((*e)->message) {
                (*e)->view.message.data = (*e)->message;
                (*e)->view.message.size = strlen(msg);
            }
            if ((*e)->subject) {
                (*e)->view.subject.data = (*e)->subject;
                (*e)->view.subject.size = strlen(subject);
            }
        }
    }
    return c;
}
const apkg_error_view *apkg_error_get(const apkg_error *e) { return &e->view; }
void apkg_error_free(apkg_error *e) {
    if (e) {
        free(e->message);
        free(e->subject);
        free(e);
    }
}
int ak_path(char *o, const char *a, const char *b) {
    int n = snprintf(o, AK_PATH, "%s%s%s", a,
                     (*a && a[strlen(a) - 1] == ':') ? "" : "/", b);
    return n < 0 || n >= AK_PATH ? -1 : 0;
}
int ak_exists(const char *p) {
    struct stat s;
    if (lstat(p, &s) == 0)
        return 1;
    return errno == ENOENT ? 0 : -1;
}
int ak_physical(const char *p, const struct stat *old) {
    struct stat now;
    char canonical[AK_PATH];
    /* A moved root reached through a new symlink could otherwise retain its
     * inode while acquiring a different parent lock/coordinator location. */
    return pkg_fs_canonical_dir(p, canonical, sizeof canonical) == 0 &&
           !strcmp(p, canonical) && stat(p, &now) == 0 &&
           old->st_dev == now.st_dev && old->st_ino == now.st_ino;
}
int ak_resolve(const char *p, char *o, struct stat *binding) {
    if (pkg_fs_canonical_dir(p, o, AK_PATH))
        return -1;
    return stat(o, binding);
}
int ak_read(const char *p, unsigned char **out, size_t *size, size_t max) {
    int fd;
    struct stat st;
    size_t n = 0;
    unsigned char *b;
    *out = NULL;
    *size = 0;
    if (pkg_fs_path_is_link(p) != 0) {
        errno = ELOOP;
        return -1;
    }
    int flags = O_RDONLY;
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(p, flags);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > max) {
        close(fd);
        errno = EFBIG;
        return -1;
    }
    b = malloc((size_t)st.st_size + 1);
    if (!b) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    while (n < (size_t)st.st_size) {
        ssize_t r = read(fd, b + n, (size_t)st.st_size - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            free(b);
            close(fd);
            errno = EIO;
            return -1;
        }
        n += (size_t)r;
    }
    {
        unsigned char x;
        if (read(fd, &x, 1) != 0) {
            free(b);
            close(fd);
            errno = EIO;
            return -1;
        }
    }
    if (close(fd)) {
        free(b);
        return -1;
    }
    b[n] = 0;
    *out = b;
    *size = n;
    return 0;
}
int ak_write(const char *p, const void *v, size_t n, int exclusive) {
    int fd = open(p, O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC), 0600);
    const unsigned char *b = v;
    size_t i = 0;
    if (fd < 0)
        return -1;
    while (i < n) {
        ssize_t r = write(fd, b + i, n - i);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            close(fd);
            return -1;
        }
        i += (size_t)r;
    }
    if (fsync(fd)) {
        close(fd);
        return -1;
    }
    return close(fd);
}
int ak_sync_dir(const char *p) {
#ifdef __AROS__
    return pkg_fs_flush_root(p) ? 0 : -1;
#else
    int fd = open(p, O_RDONLY), r;
    if (fd < 0)
        return -1;
    r = fsync(fd);
    if (close(fd))
        r = -1;
    return r;
#endif
}
int ak_remove_tree(const char *p) {
    struct stat st;
    DIR *d;
    struct dirent *e;
    int bad = 0;
    if (lstat(p, &st))
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(p);
    /* This helper only discards private transaction staging, never live trees
     * or retained history. A declared read-only drawer must not strand an
     * incomplete preparation after a later allocation or I/O failure. */
    if ((st.st_mode & 0700) != 0700 && chmod(p, (st.st_mode & 0777) | 0700))
        return -1;
    d = opendir(p);
    if (!d)
        return -1;
    errno = 0;
    while ((e = readdir(d))) {
        char q[AK_PATH];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (ak_path(q, p, e->d_name) || ak_remove_tree(q)) {
            bad = 1;
            break;
        }
        errno = 0;
    }
    if (errno)
        bad = 1;
    closedir(d);
    return bad ? -1 : rmdir(p);
}
apkg_status apkg_open(const apkg_open_options *o, apkg_ctx **out,
                      apkg_error **err) {
    apkg_ctx *c;
    char p[AK_PATH], *slash;
    size_t n;
    if (err)
        *err = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!o || o->contract_version != 1 || o->struct_size != sizeof *o)
        return ak_fail(err, APKG_UNSUPPORTED, "API version/size mismatch", "");
    if (!o->root.data || !o->root.size || o->root.size >= AK_PATH ||
        (!ak_text_path_valid(o->root.data, o->root.size)) ||
        (o->mode != APKG_READ && o->mode != APKG_WRITE))
        return ak_fail(err, APKG_INVALID, "invalid context options", "");
    c = calloc(1, sizeof *c);
    if (!c)
        return APKG_NOMEM;
    c->object_max = o->max_object_bytes ? (size_t)o->max_object_bytes : AK_MAX;
    c->memory_max =
        o->max_memory_bytes ? (size_t)o->max_memory_bytes : 64u * 1024u * 1024u;
    if ((o->max_object_bytes && o->max_object_bytes > SIZE_MAX) ||
        (o->max_memory_bytes && o->max_memory_bytes > SIZE_MAX) ||
        c->object_max > AK_MAX || c->object_max > c->memory_max ||
        c->memory_max < 1048576u) {
        free(c);
        return ak_fail(err, APKG_LIMIT, "unsupported resource budget", "");
    }
    memcpy(p, o->root.data, o->root.size);
    p[o->root.size] = 0;
    while (strlen(p) > 1 && p[strlen(p) - 1] == '/')
        p[strlen(p) - 1] = 0;
    if (!ak_resolve(p, c->root, &c->binding)) {
        strcpy(c->anchor, c->root);
    } else {
        if (errno != ENOENT) {
            free(c);
            return ak_fail(err, APKG_IO, "cannot resolve root", p);
        }
        slash = strrchr(p, '/');
        if (!slash) {
            strcpy(c->root, p);
            strcpy(p, ".");
        } else {
            if (!slash[1] || !strcmp(slash + 1, ".") ||
                !strcmp(slash + 1, "..")) {
                free(c);
                return APKG_INVALID;
            }
            strcpy(c->root, slash + 1);
            if (slash == p)
                slash[1] = 0;
            else
                *slash = 0;
        }
        if (ak_resolve(p, c->anchor, &c->binding)) {
            free(c);
            return ak_fail(err, APKG_UNSUPPORTED, "root parent must exist", p);
        }
        {
            char leaf[AK_PATH];
            strcpy(leaf, c->root);
            if (ak_path(c->root, c->anchor, leaf)) {
                free(c);
                return APKG_LIMIT;
            }
        }
    }
    n = strlen(c->root);
    if (n > AK_PATH - 100) {
        free(c);
        return APKG_LIMIT;
    }
    c->mode = o->mode;
    if (c->mode == APKG_WRITE && pkg_fs_random(c->session, sizeof c->session)) {
        free(c);
        return ak_fail(err, APKG_IO, "no random session identity", "");
    }
    *out = c;
    return APKG_OK;
}
void apkg_close(apkg_ctx *c) { free(c); }
static apkg_text text(const char *s) {
    apkg_text v = {s, s ? strlen(s) : 0};
    return v;
}
static int string_field(const ak_json *j, int o, const char *key, char **out) {
    int i = ak_member(j, o, key);
    size_t n;
    if (i < 0 || j->tokens[i].type != '"')
        return -1;
    n = j->tokens[i].end - j->tokens[i].start;
    *out = malloc(n);
    return *out ? ak_string(j, i, *out, n) : -1;
}
static int pending(apkg_ctx *c, int *out) {
    char p[AK_PATH];
    int n;
    if (snprintf(p, sizeof p, "%s.apkg-transaction", c->root) >= (int)sizeof p)
        return -1;
    n = ak_exists(p);
    if (n < 0)
        return -1;
    *out = n;
    return 0;
}
static int legacy_pending(apkg_ctx *c, uint64_t *count) {
    char db[AK_PATH], txns[AK_PATH];
    DIR *dir;
    struct dirent *entry;
    *count = 0;
    if (ak_path(db, c->root, "db") || ak_path(txns, db, "transactions"))
        return -1;
    if (pkg_fs_path_is_link(db) != 0 || pkg_fs_path_is_link(txns) != 0)
        return -1;
    dir = opendir(txns);
    if (!dir)
        return errno == ENOENT ? 0 : -1;
    errno = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (++*count > AK_TOKENS) {
            closedir(dir);
            return -1;
        }
        errno = 0;
    }
    if (errno) {
        closedir(dir);
        return -1;
    }
    return closedir(dir);
}
apkg_status ak_snapshot(apkg_ctx *c, apkg_snapshot **out, apkg_error **err) {
    apkg_snapshot *s = NULL;
    unsigned char *raw = NULL, *again = NULL;
    size_t len = 0, nagain = 0;
    char path[AK_PATH], meta[AK_PATH];
    ak_json j = {0};
    int pend, pend2, exists, b, arr;
    size_t i;
    static const char *const fields[] = {"generation", "installed", "policy",
                                         "root_id", "target"};
    errno = 0;
    if (pending(c, &pend))
        return ak_fail(err, APKG_IO, "cannot inspect recovery state", c->root);
    s = calloc(1, sizeof *s);
    if (!s)
        return APKG_NOMEM;
    strcpy(s->anchor, c->anchor);
    s->binding = c->binding;
    s->view.pending_count.state = APKG_KNOWN;
    s->view.pending_count.value = (uint64_t)pend;
    if (!ak_physical(c->anchor, &c->binding))
        goto stale;
    /* An absent-root handle binds its ancestor, not a root another process
     * created later. Explicit initialization/recovery rebinds its own handle.
     */
    if (strcmp(c->anchor, c->root) && ak_exists(c->root) != 0)
        goto stale;
    if (ak_path(meta, c->root, ".apkg") || ak_path(path, meta, "state"))
        goto invalid;
    exists = ak_exists(meta);
    if (exists < 0)
        goto io;
    if (!exists) {
        char legacy[AK_PATH];
        int a, d;
        ak_path(legacy, c->root, "db");
        a = ak_exists(legacy);
        ak_path(legacy, c->root, ".pkg");
        d = ak_exists(legacy);
        if (a < 0 || d < 0)
            goto io;
        s->view.state = pend       ? APKG_ROOT_PENDING
                        : (a || d) ? APKG_ROOT_LEGACY
                                   : APKG_ROOT_ABSENT;
        if (a || d) {
            uint64_t legacy_count = 0;
            if (a && legacy_pending(c, &legacy_count))
                goto io;
            if (legacy_count) {
                s->view.state = APKG_ROOT_PENDING;
                s->view.pending_count.value += legacy_count;
            }
            s->view.pending_count.state =
                legacy_count ? APKG_KNOWN : APKG_UNKNOWN;
            s->view.diagnostic =
                text("legacy metadata needs validated migration");
        }
        if (pending(c, &pend2))
            goto io;
        if (pend2 != pend || !ak_physical(c->anchor, &c->binding))
            goto stale;
        *out = s;
        return APKG_OK;
    }
    {
        char legacy[AK_PATH];
        int a, d;
        ak_path(legacy, c->root, "db");
        a = ak_exists(legacy);
        ak_path(legacy, c->root, ".pkg");
        d = ak_exists(legacy);
        if (a < 0 || d < 0)
            goto io;
        if (a || d) {
            s->view.state = APKG_ROOT_INVALID;
            s->view.diagnostic = text("common and legacy databases coexist; "
                                      "validated cutover required");
            *out = s;
            return APKG_OK;
        }
    }
    if (pkg_fs_path_is_link(meta) != 0)
        goto invalid;
    if (ak_read(path, &raw, &len, c->object_max)) {
        if (errno == ENOENT) {
            s->view.state = pend ? APKG_ROOT_PENDING : APKG_ROOT_INVALID;
            s->view.diagnostic = text("common state unavailable");
            *out = s;
            return APKG_OK;
        }
        if (errno == EFBIG)
            goto limit;
        goto io;
    }
    if (len > c->memory_max / 32)
        goto limit;
    if (ak_json_parse(raw, len, &j))
        goto invalid;
    if (!ak_object(&j, "root") ||
        !ak_fields_optional(&j, ak_member(&j, 0, "body"), fields, 5,
                            (const char *const[]){"channel_heads",
                                                  "package_generation",
                                                  "qualification_records"},
                            3))
        goto unsupported;
    b = ak_member(&j, 0, "body");
    {
        char id[33], target[32];
        uint64_t generation;
        if (ak_string(&j, ak_member(&j, b, "root_id"), id, sizeof id) ||
            ak_hex(id, s->view.root_id.bytes, 16) ||
            ak_u64(&j, ak_member(&j, b, "generation"), &generation) ||
            !generation ||
            ak_string(&j, ak_member(&j, b, "target"), target, sizeof target) ||
            pkg_target_parse(target, NULL, NULL, 0))
            goto invalid;
        s->target = ak_copy(target, strlen(target));
        if (!s->target)
            goto nomem;
        s->view.target.state = APKG_KNOWN;
        s->view.target.value = text(s->target);
        s->view.target.evidence = text("root record");
        s->view.identity_state = APKG_KNOWN;
        s->view.generation.state = APKG_KNOWN;
        s->view.generation.value = generation;
        s->package_generation = generation;
        int pg = ak_member(&j, b, "package_generation");
        if (pg >= 0 &&
            (ak_u64(&j, pg, &s->package_generation) || !s->package_generation ||
             s->package_generation > generation))
            goto invalid;
    }
    {
        int p = ak_member(&j, b, "policy");
        size_t start = j.tokens[p].start, n = j.tokens[p].end - start;
        ak_json policy = {0};
        s->policy = ak_copy(j.text + start, n);
        if (!s->policy)
            goto nomem;
        if (ak_json_parse(s->policy, n, &policy) || !ak_policy_valid(&policy)) {
            ak_json_free(&policy);
            goto invalid;
        }
        ak_json_free(&policy);
    }
    {
        int heads = ak_member(&j, b, "channel_heads");
        if (!ak_channel_heads_valid(&j, heads))
            goto invalid;
        s->channel_heads = heads < 0 ? ak_copy("[]", 2) : ak_slice(&j, heads);
        if (!s->channel_heads)
            goto nomem;
    }
    {
        int records = ak_member(&j, b, "qualification_records");
        if (!ak_qualification_records_valid(&j, records))
            goto invalid;
        s->qualification_records =
            records < 0 ? ak_copy("[]", 2) : ak_slice(&j, records);
        if (!s->qualification_records)
            goto nomem;
    }
    arr = ak_member(&j, b, "installed");
    if (arr < 0 || j.tokens[arr].type != '[')
        goto invalid;
    for (i = (size_t)arr + 1; i < j.tokens[arr].next; i = j.tokens[i].next)
        s->nrows++;
    if (s->nrows > (c->memory_max / 2) / (sizeof *s->rows + sizeof *s->records))
        goto limit;
    if (s->nrows) {
        s->rows = calloc(s->nrows, sizeof *s->rows);
        s->records = calloc(s->nrows, sizeof *s->records);
        if (!s->rows || !s->records)
            goto nomem;
    }
    {
        size_t row = 0;
        for (i = (size_t)arr + 1; i < j.tokens[arr].next;
             i = j.tokens[i].next) {
            apkg_installed_view *r = &s->rows[row];
            int desc = ak_member(&j, (int)i, "descriptor"), body;
            char temp[256];
            uint64_t revision;
            static const char *const entry_fields[] = {"descriptor",
                                                       "signature"};
            if ((!ak_fields(&j, (int)i, entry_fields, 2) &&
                 !ak_fields(
                     &j, (int)i,
                     (const char *const[]){"descriptor", "reason", "signature"},
                     3)) ||
                desc < 0)
                goto invalid;
            {
                ak_json d = {0};
                if (ak_json_parse(j.text + j.tokens[desc].start,
                                  j.tokens[desc].end - j.tokens[desc].start,
                                  &d) ||
                    !ak_descriptor(&d)) {
                    ak_json_free(&d);
                    goto invalid;
                }
                ak_json_free(&d);
            }
            body = ak_member(&j, desc, "body");
            ak_hash(j.text + j.tokens[desc].start,
                    j.tokens[desc].end - j.tokens[desc].start,
                    &r->descriptor_digest);
            s->records[row] = ak_copy(j.text + j.tokens[i].start,
                                      j.tokens[i].end - j.tokens[i].start);
            if (!s->records[row])
                goto nomem;
            r->record.data = (unsigned char *)s->records[row];
            r->record.size = strlen(s->records[row]);
            /* Views into independently allocated strings are freed with the
             * row. */
            {
                apkg_text_fact *f[] = {&r->authority, &r->name, &r->version,
                                       &r->target};
                const char *k[] = {"authority", "name", "version", "target"};
                size_t x;
                for (x = 0; x < 4; x++) {
                    char *v = NULL;
                    if (string_field(&j, body, k[x], &v))
                        goto invalid;
                    f[x]->state = APKG_KNOWN;
                    f[x]->value = text(v);
                    f[x]->evidence = text("committed descriptor");
                }
            }
            if (ak_u64(&j, ak_member(&j, body, "revision"), &revision))
                goto invalid;
            r->revision.state = APKG_KNOWN;
            r->revision.value = revision;
            r->reason = APKG_REASON_EXPLICIT;
            if (ak_member(&j, (int)i, "reason") >= 0) {
                if (ak_equal(&j, ak_member(&j, (int)i, "reason"), "dependency"))
                    r->reason = APKG_REASON_DEPENDENCY;
                else if (ak_equal(&j, ak_member(&j, (int)i, "reason"),
                                  "recommendation"))
                    r->reason = APKG_REASON_RECOMMENDATION;
                else if (!ak_equal(&j, ak_member(&j, (int)i, "reason"),
                                   "explicit"))
                    goto invalid;
            }
            if (ak_string(&j, ak_member(&j, body, "name"), temp, sizeof temp))
                goto invalid;
            {
                apkg_digest id;
                ak_hash(temp, strlen(temp), &id);
                memcpy(r->instance_id.bytes, id.bytes, 16);
            }
            row++;
        }
    }
    if (ak_read(path, &again, &nagain, c->object_max) || pending(c, &pend2))
        goto io;
    if (len != nagain || memcmp(raw, again, len) || pend != pend2)
        goto stale;
    s->raw = (char *)raw;
    raw = NULL;
    s->view.source_record.data = (unsigned char *)s->raw;
    s->view.source_record.size = len;
    ak_hash(s->raw, len, &s->view.snapshot_digest);
    s->view.state = pend ? APKG_ROOT_PENDING : APKG_ROOT_READY;
    ak_json_free(&j);
    free(again);
    *out = s;
    return APKG_OK;
unsupported:
    s->view.state = APKG_ROOT_INVALID;
    s->view.diagnostic = text("unsupported root schema or required feature");
    s->raw = (char *)raw;
    raw = NULL;
    s->view.source_record.data = (unsigned char *)s->raw;
    s->view.source_record.size = len;
    ak_json_free(&j);
    *out = s;
    return APKG_OK;
stale:
    ak_json_free(&j);
    free(raw);
    free(again);
    apkg_snapshot_free(s);
    return APKG_BUSY;
limit:
    ak_json_free(&j);
    free(raw);
    free(again);
    apkg_snapshot_free(s);
    return ak_fail(err, APKG_LIMIT, "root exceeds budget", c->root);
invalid:
    if (errno == ENOMEM)
        goto nomem;
    ak_json_free(&j);
    free(raw);
    free(again);
    apkg_snapshot_free(s);
    return ak_fail(err, APKG_INVALID, "invalid common root", c->root);
io:
    if (errno == ENOMEM)
        goto nomem;
    ak_json_free(&j);
    free(raw);
    free(again);
    apkg_snapshot_free(s);
    return ak_fail(err, APKG_IO, "cannot inspect root", c->root);
nomem:
    ak_json_free(&j);
    free(raw);
    free(again);
    apkg_snapshot_free(s);
    return APKG_NOMEM;
}
apkg_status apkg_snapshot_read(apkg_ctx *c, apkg_snapshot **out,
                               apkg_error **e) {
    unsigned i;
    apkg_status r;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c)
        return APKG_INVALID;
    for (i = 0; i < 3; i++) {
        r = ak_snapshot(c, out, e);
        if (r != APKG_BUSY)
            return r;
    }
    return ak_fail(e, APKG_BUSY, "root changed during read", c->root);
}
const apkg_root_view *apkg_snapshot_root(const apkg_snapshot *s) {
    return &s->view;
}
uint64_t apkg_snapshot_installed_count(const apkg_snapshot *s) {
    return s->nrows;
}
apkg_status apkg_snapshot_installed_at(const apkg_snapshot *s, uint64_t i,
                                       const apkg_installed_view **out,
                                       apkg_error **e) {
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!s)
        return APKG_INVALID;
    if (i >= s->nrows)
        return APKG_NOT_FOUND;
    *out = &s->rows[(size_t)i];
    return APKG_OK;
}
void apkg_snapshot_free(apkg_snapshot *s) {
    size_t i;
    if (!s)
        return;
    for (i = 0; i < s->nrows; i++) {
        if (s->rows) {
            free((void *)s->rows[i].authority.value.data);
            free((void *)s->rows[i].name.value.data);
            free((void *)s->rows[i].version.value.data);
            free((void *)s->rows[i].target.value.data);
        }
        if (s->records)
            free(s->records[i]);
    }
    free(s->rows);
    free(s->records);
    free(s->raw);
    free(s->policy);
    free(s->target);
    free(s->diagnostic);
    free(s->channel_heads);
    free(s->qualification_records);
    free(s);
}
