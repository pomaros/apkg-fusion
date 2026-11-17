/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak
 *
 * Native first half of migrating an arospkg root: inspect it where it is,
 * and copy it. The original root is only read. Nothing here registers a
 * package with the common engine; the copy keeps the classic registry bytes
 * in .apkg-legacy/db and a record of what was found. */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "pkg_fs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define MG_CHUNK 65536u
#define MG_PACKAGES 4096u
#define MG_RELATIONS 16u
#define MG_FORMAT "apkg-legacy-inspection-1"
#define MG_ARCHIVE ".apkg-legacy"

/* ---- JSON as the classic client writes it: whitespace, numbers, any key
 * order. The engine's own reader accepts canonical documents only. ---- */
typedef struct mj mj;
struct mj {
    char type; /* { [ " 0 t f n */
    char *key, *text;
    mj *child, *next;
};
typedef struct {
    const char *s;
    size_t n, p, nodes;
    int nomem;
} mj_reader;
static void mj_free(mj *v) {
    while (v) {
        mj *next = v->next;
        mj_free(v->child);
        free(v->key);
        free(v->text);
        free(v);
        v = next;
    }
}
static void mj_space(mj_reader *r) {
    while (r->p < r->n && (r->s[r->p] == ' ' || r->s[r->p] == '\n' ||
                           r->s[r->p] == '\r' || r->s[r->p] == '\t'))
        r->p++;
}
static int mj_hex4(mj_reader *r, unsigned *out) {
    unsigned v = 0;
    if (r->n - r->p < 4)
        return -1;
    for (int i = 0; i < 4; i++) {
        char c = r->s[r->p++];
        unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                     : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                     : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10)
                                            : 16u;
        if (d > 15)
            return -1;
        v = v * 16 + d;
    }
    *out = v;
    return 0;
}
static char *mj_string(mj_reader *r) {
    size_t at = 0;
    char *out;
    if (r->p >= r->n || r->s[r->p] != '"')
        return NULL;
    r->p++;
    out = malloc(r->n - r->p + 1);
    if (!out) {
        r->nomem = 1;
        return NULL;
    }
    while (r->p < r->n) {
        unsigned char c = (unsigned char)r->s[r->p++];
        if (c == '"') {
            out[at] = 0;
            return out;
        }
        if (c < 32)
            break;
        if (c != '\\') {
            out[at++] = (char)c;
            continue;
        }
        if (r->p >= r->n)
            break;
        c = (unsigned char)r->s[r->p++];
        if (c == 'u') {
            unsigned u, low;
            if (mj_hex4(r, &u))
                break;
            if (u >= 0xd800 && u < 0xdc00) {
                if (r->n - r->p < 6 || r->s[r->p] != '\\' ||
                    r->s[r->p + 1] != 'u')
                    break;
                r->p += 2;
                if (mj_hex4(r, &low) || low < 0xdc00 || low > 0xdfff)
                    break;
                u = 0x10000 + ((u - 0xd800) << 10) + (low - 0xdc00);
            } else if (u >= 0xdc00 && u <= 0xdfff)
                break;
            if (!u)
                break;
            if (u < 0x80)
                out[at++] = (char)u;
            else if (u < 0x800) {
                out[at++] = (char)(0xc0 | (u >> 6));
                out[at++] = (char)(0x80 | (u & 63));
            } else if (u < 0x10000) {
                out[at++] = (char)(0xe0 | (u >> 12));
                out[at++] = (char)(0x80 | ((u >> 6) & 63));
                out[at++] = (char)(0x80 | (u & 63));
            } else {
                out[at++] = (char)(0xf0 | (u >> 18));
                out[at++] = (char)(0x80 | ((u >> 12) & 63));
                out[at++] = (char)(0x80 | ((u >> 6) & 63));
                out[at++] = (char)(0x80 | (u & 63));
            }
            continue;
        }
        const char *from = "\"\\/bfnrt", *to = "\"\\/\b\f\n\r\t";
        const char *hit = strchr(from, (int)c);
        if (!hit || !c)
            break;
        out[at++] = to[hit - from];
    }
    free(out);
    return NULL;
}
static mj *mj_value(mj_reader *r, unsigned depth) {
    mj *v;
    mj_space(r);
    if (depth > 32 || r->p >= r->n || ++r->nodes > AK_TOKENS)
        return NULL;
    v = calloc(1, sizeof *v);
    if (!v) {
        r->nomem = 1;
        return NULL;
    }
    char c = r->s[r->p];
    if (c == '"') {
        v->type = '"';
        v->text = mj_string(r);
        if (!v->text)
            goto bad;
    } else if (c == '{' || c == '[') {
        char end = c == '{' ? '}' : ']';
        mj **tail = &v->child;
        v->type = c;
        r->p++;
        mj_space(r);
        if (r->p < r->n && r->s[r->p] == end) {
            r->p++;
            return v;
        }
        for (;;) {
            char *key = NULL;
            mj *item;
            if (c == '{') {
                mj_space(r);
                key = mj_string(r);
                if (!key)
                    goto bad;
                for (mj *seen = v->child; seen; seen = seen->next)
                    if (!strcmp(seen->key, key)) { /* a duplicate key */
                        free(key);
                        goto bad;
                    }
                mj_space(r);
                if (r->p >= r->n || r->s[r->p++] != ':') {
                    free(key);
                    goto bad;
                }
            }
            item = mj_value(r, depth + 1);
            if (!item) {
                free(key);
                goto bad;
            }
            item->key = key;
            *tail = item;
            tail = &item->next;
            mj_space(r);
            if (r->p >= r->n)
                goto bad;
            if (r->s[r->p] == end) {
                r->p++;
                return v;
            }
            if (r->s[r->p++] != ',')
                goto bad;
        }
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        size_t a = r->p;
        if (c == '-')
            r->p++;
        size_t digits = r->p;
        while (r->p < r->n && r->s[r->p] >= '0' && r->s[r->p] <= '9')
            r->p++;
        if (r->p == digits || (r->s[digits] == '0' && r->p - digits > 1))
            goto bad;
        /* A fraction or an exponent is no integer; the record has none. */
        if (r->p < r->n &&
            (r->s[r->p] == '.' || r->s[r->p] == 'e' || r->s[r->p] == 'E'))
            goto bad;
        v->type = '0';
        v->text = ak_copy(r->s + a, r->p - a);
        if (!v->text) {
            r->nomem = 1;
            goto bad;
        }
    } else {
        static const char *const words[] = {"true", "false", "null"};
        int found = 0;
        for (int i = 0; i < 3 && !found; i++) {
            size_t n = strlen(words[i]);
            if (r->n - r->p >= n && !memcmp(r->s + r->p, words[i], n)) {
                v->type = words[i][0];
                r->p += n;
                found = 1;
            }
        }
        if (!found)
            goto bad;
    }
    return v;
bad:
    mj_free(v);
    return NULL;
}
/* 0, -1 unreadable, -2 out of memory. */
static int mj_parse(const void *data, size_t n, mj **out) {
    mj_reader r = {data, n, 0, 0, 0};
    *out = mj_value(&r, 1);
    if (*out) {
        mj_space(&r);
        if (r.p == n)
            return 0;
        mj_free(*out);
        *out = NULL;
    }
    return r.nomem ? -2 : -1;
}
static const mj *mj_get(const mj *object, const char *key) {
    if (!object || object->type != '{')
        return NULL;
    for (const mj *v = object->child; v; v = v->next)
        if (!strcmp(v->key, key))
            return v;
    return NULL;
}
static const char *mj_text(const mj *v) {
    return v && v->type == '"' ? v->text : NULL;
}
/* A non-negative integer; a negative one or another type fails. */
static int mj_u64(const mj *v, uint64_t *out) {
    uint64_t n = 0;
    if (!v || v->type != '0' || v->text[0] == '-')
        return -1;
    for (const char *s = v->text; *s; s++) {
        unsigned d = (unsigned)(*s - '0');
        if (n > (UINT64_MAX - d) / 10)
            return -1;
        n = n * 10 + d;
    }
    *out = n;
    return 0;
}
static size_t mj_count(const mj *list) {
    size_t n = 0;
    for (const mj *v = list ? list->child : NULL; v; v = v->next)
        n++;
    return n;
}
static int mj_only(const mj *object, const char *const *allowed, size_t n) {
    for (const mj *v = object->child; v; v = v->next) {
        size_t i = 0;
        while (i < n && strcmp(v->key, allowed[i]))
            i++;
        if (i == n)
            return 0;
    }
    return 1;
}

/* ---- What an installed record states. ---- */
typedef struct {
    char *path, sha[65];
    uint64_t size;
    int seen;
} mg_file;
typedef struct {
    char name[256], version[256], arch[64], abi[64], reason[16];
    char icon_state[16], icon_sha[65], icon_now[65], record_sha[65];
    char *registry, *archive; /* previous-version references, or NULL */
    char depends[MG_RELATIONS][64], excludes[2 * MG_RELATIONS][64];
    size_t ndepends, nexcludes, nfiles;
    int has_revision, has_abi, icon_found;
    uint64_t revision, unchanged, changed, unrecorded;
    mg_file *files;
} mg_package;
typedef struct {
    apkg_status code;
    char why[AK_PATH + 320];
    char source[AK_PATH], destination[AK_PATH], selfupdate[AK_PATH];
    const char *envarc, *env;
    int quiescent, json;
    mg_package *packages;
    size_t count;
} mg;
static apkg_status refuse(mg *m, apkg_status code, const char *why,
                          const char *subject) {
    if (!m->code) {
        m->code = code;
        snprintf(m->why, sizeof m->why, "%s%s%s", why,
                 subject && *subject ? ": " : "", subject ? subject : "");
    }
    return code;
}
static int fold(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int fold_compare(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = fold((unsigned char)*a), y = fold((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
}
static int fold_prefix(const char *text, const char *prefix, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (fold((unsigned char)text[i]) != fold((unsigned char)prefix[i]))
            return 0;
    return 1;
}
static int files_order(const void *a, const void *b) {
    return fold_compare(((const mg_file *)a)->path, ((const mg_file *)b)->path);
}
static int packages_order(const void *a, const void *b) {
    return strcmp(((const mg_package *)a)->name,
                  ((const mg_package *)b)->name);
}
static int names_order(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static int sha_valid(const char *s) {
    if (!s || strlen(s) != 64)
        return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')))
            return 0;
    return 1;
}
/* [0-9]{1,9}(\.[0-9]{1,9}){0,7}: arospkg's dotted version. */
static int dotted(const char *s) {
    for (unsigned parts = 0;; parts++) {
        unsigned digits = 0;
        while (*s >= '0' && *s <= '9') {
            s++;
            digits++;
        }
        if (!digits || digits > 9 || parts > 7)
            return 0;
        if (!*s)
            return 1;
        if (*s++ != '.')
            return 0;
    }
}
/* arospkg's relation: an id, and with `minimum` an optional " >= version". */
static int relation(const char *s, int minimum, char id[64]) {
    size_t n = 0;
    if (!(*s >= 'a' && *s <= 'z'))
        return 0;
    while ((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9') ||
           (s[n] && strchr("._-", s[n])))
        n++;
    if (n > 64 - 1)
        return 0;
    memcpy(id, s, n);
    id[n] = 0;
    if (!s[n])
        return 1;
    return minimum && !strncmp(s + n, " >= ", 4) && dotted(s + n + 4);
}
static int requirement_type(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 23)
        return 0;
    for (; *s; s++)
        if (!(*s >= 'a' && *s <= 'z'))
            return 0;
    return 1;
}
static int requirement_id(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 63 || !((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                          (*s >= '0' && *s <= '9')))
        return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || strchr("._+-", *s)))
            return 0;
    return 1;
}
static void package_free(mg_package *p) {
    for (size_t i = 0; i < p->nfiles; i++)
        free(p->files[i].path);
    free(p->files);
    free(p->registry);
    free(p->archive);
}
static int copy_text(char *out, size_t n, const char *text) {
    if (!text || strlen(text) >= n)
        return -1;
    strcpy(out, text);
    return 0;
}
/* NULL when the record is one this reader interprets, else what is wrong. */
static const char *record_read(const mj *r, mg_package *p, int *nomem) {
    static const char *const allowed[] = {
        "schema", "name", "version", "revision", "arch", "abi", "subdir",
        "installed", "origin", "archive_sha256", "archive_size", "reason",
        "relations_schema", "dir", "requires_system", "icon", "previous",
        "launch", "contents", "post_install_notes", "depends", "recommends",
        "conflicts", "replaces"};
    uint64_t number;
    const mj *v;
    const char *text;
    if (!r || r->type != '{')
        return "the record is not an object";
    if (!mj_only(r, allowed, sizeof allowed / sizeof *allowed))
        return "the record has fields this reader does not know";
    if (mj_u64(mj_get(r, "schema"), &number) || number != 1)
        return "unsupported record schema";
    text = mj_text(mj_get(r, "name"));
    if (!text || !ak_valid_name(text) || copy_text(p->name, sizeof p->name, text) ||
        !mj_text(mj_get(r, "dir")) || strcmp(mj_text(mj_get(r, "dir")), text))
        return "the package name and its drawer do not identify one package";
    text = mj_text(mj_get(r, "version"));
    if (!text || !*text || copy_text(p->version, sizeof p->version, text))
        return "no version recorded";
    if ((v = mj_get(r, "revision"))) {
        if (mj_u64(v, &p->revision))
            return "invalid revision";
        p->has_revision = 1;
    }
    text = (v = mj_get(r, "reason")) ? mj_text(v) : "unrecorded";
    if (!text || (strcmp(text, "explicit") && strcmp(text, "dependency") &&
                  strcmp(text, "unrecorded")))
        return "unknown installation reason";
    strcpy(p->reason, text);
    if ((v = mj_get(r, "relations_schema")) &&
        (mj_u64(v, &number) || number != 1))
        return "unsupported relation schema";
    text = mj_text(mj_get(r, "arch"));
    if (!text || !*text || copy_text(p->arch, sizeof p->arch, text))
        return "no CPU recorded";
    if ((v = mj_get(r, "abi"))) {
        if (copy_text(p->abi, sizeof p->abi, mj_text(v)))
            return "invalid ABI fact";
        p->has_abi = *p->abi != 0;
    }
    if (!sha_valid(mj_text(mj_get(r, "archive_sha256"))) ||
        mj_u64(mj_get(r, "archive_size"), &number))
        return "invalid identity of the original archive";
    if (!mj_text(mj_get(r, "origin")) || !mj_text(mj_get(r, "installed")))
        return "origin or installation time is not text";
    if ((v = mj_get(r, "subdir")) && !mj_text(v))
        return "invalid subdir";
    /* System requirements: the outcome of the installation-time probe stays
     * in the retained record. */
    if ((v = mj_get(r, "requires_system"))) {
        static const char *const fields[] = {"type", "id", "min_version",
                                             "state", "found_version",
                                             "checked"};
        if (v->type != '[' || mj_count(v) > MG_RELATIONS)
            return "invalid system requirement list";
        for (const mj *e = v->child; e; e = e->next) {
            const mj *f;
            if (e->type != '{' || !mj_only(e, fields, 6) ||
                !mj_text(mj_get(e, "type")) ||
                !requirement_type(mj_text(mj_get(e, "type"))) ||
                !mj_text(mj_get(e, "id")) ||
                !requirement_id(mj_text(mj_get(e, "id"))))
                return "unreadable system requirement";
            if ((f = mj_get(e, "min_version")) &&
                (mj_u64(f, &number) || number > 65535))
                return "unreadable system requirement";
            if ((f = mj_get(e, "state")) &&
                (!mj_text(f) || (strcmp(f->text, "satisfied") &&
                                 strcmp(f->text, "missing") &&
                                 strcmp(f->text, "undetermined"))))
                return "unreadable system requirement";
            if (((f = mj_get(e, "found_version")) && f->type != '0') ||
                ((f = mj_get(e, "checked")) && !mj_text(f)))
                return "unreadable system requirement";
            for (const mj *o = v->child; o != e; o = o->next)
                if (!strcmp(mj_text(mj_get(o, "type")),
                            mj_text(mj_get(e, "type"))) &&
                    !strcmp(mj_text(mj_get(o, "id")), mj_text(mj_get(e, "id"))))
                    return "duplicate system requirement";
        }
    }
    static const char *const families[] = {"depends", "conflicts", "replaces"};
    for (int k = 0; k < 3; k++) {
        if (!(v = mj_get(r, families[k])))
            continue;
        if (v->type != '[' || mj_count(v) > MG_RELATIONS)
            return "invalid relation list";
        for (const mj *e = v->child; e; e = e->next) {
            char id[64];
            if (!mj_text(e) || !relation(e->text, k == 0, id))
                return "unreadable relation";
            if (!strcmp(id, p->name))
                return "a relation names its own package";
            for (const mj *o = v->child; o != e; o = o->next)
                if (!strcmp(o->text, e->text))
                    return "duplicate relation";
            strcpy(k ? p->excludes[p->nexcludes++] : p->depends[p->ndepends++],
                   id);
        }
    }
    if ((v = mj_get(r, "recommends"))) {
        static const char *const fields[] = {"id", "min_version", "reason"};
        if (v->type != '[' || mj_count(v) > MG_RELATIONS)
            return "invalid relation list";
        for (const mj *e = v->child; e; e = e->next) {
            char id[64];
            const mj *f;
            if (e->type != '{' || !mj_only(e, fields, 3) ||
                !mj_text(mj_get(e, "id")) ||
                !relation(mj_text(mj_get(e, "id")), 0, id) ||
                ((f = mj_get(e, "reason")) && !mj_text(f)) ||
                ((f = mj_get(e, "min_version")) &&
                 (!mj_text(f) || !dotted(f->text))))
                return "unreadable recommendation";
            for (const mj *o = v->child; o != e; o = o->next)
                if (!strcmp(mj_text(mj_get(o, "id")), mj_text(mj_get(e, "id"))))
                    return "duplicate recommendation";
        }
    }
    strcpy(p->icon_state, "none");
    if ((v = mj_get(r, "icon"))) {
        const mj *f;
        if (v->type != '{')
            return "unknown drawer icon state";
        if ((f = mj_get(v, "state"))) {
            if (!mj_text(f) || (strcmp(f->text, "none") &&
                                strcmp(f->text, "kept-existing") &&
                                strcmp(f->text, "installed")))
                return "unknown drawer icon state";
            strcpy(p->icon_state, f->text);
        }
        if (sha_valid(mj_text(mj_get(v, "sha256"))))
            strcpy(p->icon_sha, mj_text(mj_get(v, "sha256")));
    }
    if ((v = mj_get(r, "previous"))) {
        const mj *f;
        if (v->type != '{')
            return "invalid references to the previous version";
        if ((f = mj_get(v, "registry")) &&
            (!mj_text(f) || !(p->registry = ak_copy(f->text, strlen(f->text)))))
            return mj_text(f) ? (*nomem = 1, "out of memory")
                              : "invalid references to the previous version";
        if ((f = mj_get(v, "archive")) &&
            (!mj_text(f) || !(p->archive = ak_copy(f->text, strlen(f->text)))))
            return mj_text(f) ? (*nomem = 1, "out of memory")
                              : "invalid references to the previous version";
    }
    v = mj_get(r, "contents");
    if (!v || v->type != '[')
        return "the record has no file baseline";
    size_t n = mj_count(v);
    p->files = calloc(n ? n : 1, sizeof *p->files);
    if (!p->files) {
        *nomem = 1;
        return "out of memory";
    }
    for (const mj *e = v->child; e; e = e->next) {
        static const char *const fields[] = {"path", "sha256", "size"};
        mg_file *f = &p->files[p->nfiles];
        text = e->type == '{' ? mj_text(mj_get(e, "path")) : NULL;
        if (!text || mj_count(e) != 3 || !mj_only(e, fields, 3) ||
            !ak_valid_path(text) || !sha_valid(mj_text(mj_get(e, "sha256"))) ||
            mj_u64(mj_get(e, "size"), &f->size))
            return "invalid file baseline";
        f->path = ak_copy(text, strlen(text));
        if (!f->path) {
            *nomem = 1;
            return "out of memory";
        }
        strcpy(f->sha, mj_text(mj_get(e, "sha256")));
        p->nfiles++;
    }
    if (p->nfiles > 1)
        qsort(p->files, p->nfiles, sizeof *p->files, files_order);
    for (size_t i = 1; i < p->nfiles; i++)
        if (!fold_compare(p->files[i - 1].path, p->files[i].path))
            return "the file baseline names one file twice";
    return NULL;
}

/* ---- Directories. ---- */
static void names_free(char **names, size_t n) {
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
}
/* Sorted entry names; APKG_NOT_FOUND when there is no such directory. */
static apkg_status names_read(const char *path, char ***out, size_t *count) {
    char **list = NULL;
    size_t n = 0, capacity = 0;
    apkg_status code = APKG_OK;
    struct dirent *e;
    DIR *d = opendir(path);
    *out = NULL;
    *count = 0;
    if (!d)
        return errno == ENOENT ? APKG_NOT_FOUND : APKG_IO;
    errno = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (n == capacity) {
            size_t wider = capacity ? capacity * 2 : 32;
            char **next = wider > AK_TOKENS ? NULL : malloc(wider * sizeof *next);
            if (!next) {
                code = wider > AK_TOKENS ? APKG_LIMIT : APKG_NOMEM;
                break;
            }
            if (n)
                memcpy(next, list, n * sizeof *next);
            free(list);
            list = next;
            capacity = wider;
        }
        if (!(list[n] = ak_copy(e->d_name, strlen(e->d_name)))) {
            code = APKG_NOMEM;
            break;
        }
        n++;
        errno = 0;
    }
    if (!code && errno)
        code = APKG_IO;
    closedir(d);
    if (code) {
        names_free(list, n);
        return code;
    }
    if (n > 1)
        qsort(list, n, sizeof *list, names_order);
    *out = list;
    *count = n;
    return APKG_OK;
}
static int is_directory(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ---- One walk serves inspection, copying and the check of the copy. The
 * inventory digest covers every node's type, mode, size, content hash,
 * Amiga attributes and path, in sorted order. ---- */
typedef struct {
    mg *m;
    const char *from, *to; /* to == NULL reads only */
    int classify;
    struct pkg_sha256 inventory;
    uint64_t files, directories, bytes;
    unsigned dates_lost;
    unsigned char *buffer;
} mg_walk;
static void line(mg_walk *w, char type, unsigned mode, uint64_t size,
                 const char *sha, const char *attributes, const char *rel) {
    char head[128];
    int n = snprintf(head, sizeof head, "%c %o %" PRIu64 " %s ", type, mode,
                     size, sha);
    pkg_sha256_update(&w->inventory, head, (size_t)n);
    if (attributes)
        pkg_sha256_update(&w->inventory, attributes, strlen(attributes));
    pkg_sha256_update(&w->inventory, " ", 1);
    pkg_sha256_update(&w->inventory, rel, strlen(rel) + 1);
}
static void classify(mg *m, const char *rel, const char *sha, uint64_t size) {
    const char *slash = strchr(rel, '/'), *base = strrchr(rel, '/');
    size_t n = slash ? (size_t)(slash - rel) : strlen(rel);
    base = base ? base + 1 : rel;
    if (!strcmp(base, "self-update.json") && !*m->selfupdate)
        strcpy(m->selfupdate, rel);
    if (!slash) { /* <root>/<id>.info is the drawer icon of package <id> */
        if (n <= 5 || fold_compare(rel + n - 5, ".info"))
            return;
        n -= 5;
    }
    for (size_t i = 0; i < m->count; i++) {
        mg_package *p = &m->packages[i];
        if (strlen(p->name) != n || !fold_prefix(rel, p->name, n))
            continue;
        if (!slash) {
            p->icon_found = 1;
            strcpy(p->icon_now, sha);
            return;
        }
        mg_file key, *hit;
        key.path = (char *)(slash + 1);
        hit = p->nfiles ? bsearch(&key, p->files, p->nfiles, sizeof key,
                                  files_order)
                        : NULL;
        if (!hit || hit->seen)
            p->unrecorded++;
        else {
            hit->seen = 1;
            if (hit->size == size && !strcmp(hit->sha, sha))
                p->unchanged++;
            else
                p->changed++;
        }
        return;
    }
}
static apkg_status file_node(mg_walk *w, const char *rel, const char *from,
                             const char *to, char sha[65], uint64_t *size) {
    struct stat before, after;
    struct pkg_sha256 hash;
    unsigned char digest[32];
    uint64_t total = 0;
    apkg_status code = APKG_OK;
    int flags = O_RDONLY, in, out = -1;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    in = open(from, flags);
    if (in < 0)
        return refuse(w->m, APKG_IO, "cannot read", rel);
    if (fstat(in, &before) || !S_ISREG(before.st_mode)) {
        close(in);
        return refuse(w->m, APKG_IO, "cannot read", rel);
    }
    if (to && (out = open(to, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) {
        close(in);
        return refuse(w->m, APKG_IO, "cannot create the copy of", rel);
    }
    pkg_sha256_init(&hash);
    while (!code) {
        ssize_t n = read(in, w->buffer, MG_CHUNK);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            code = refuse(w->m, APKG_IO, "cannot read", rel);
        if (n <= 0)
            break;
        pkg_sha256_update(&hash, w->buffer, (size_t)n);
        total += (uint64_t)n;
        for (size_t done = 0; out >= 0 && !code && done < (size_t)n;) {
            ssize_t k = write(out, w->buffer + done, (size_t)n - done);
            if (k < 0 && errno == EINTR)
                continue;
            if (k <= 0)
                code = refuse(w->m, APKG_IO,
                              "cannot write the copy (is the volume full?) of",
                              rel);
            else
                done += (size_t)k;
        }
    }
    if (!code && (fstat(in, &after) || after.st_size != before.st_size ||
                  after.st_mtime != before.st_mtime ||
                  (uint64_t)after.st_size != total))
        code = refuse(w->m, APKG_STALE, "a file changed while it was read", rel);
    close(in);
    if (out >= 0) {
#ifndef __AROS__ /* there the whole volume is flushed once, at the end */
        if (!code && fsync(out))
            code = refuse(w->m, APKG_IO, "cannot write the copy of", rel);
#endif
        if (close(out) && !code)
            code = refuse(w->m, APKG_IO, "cannot write the copy of", rel);
    }
    pkg_sha256_final(&hash, digest);
    ak_tohex(digest, 32, sha);
    *size = total;
    return code;
}
/* Dates first: a protection that forbids writing would refuse them. */
static apkg_status finish_node(mg_walk *w, const char *rel, const char *to,
                               const struct stat *st, const char *attributes) {
    struct utimbuf times;
    unsigned mode = (unsigned)(st->st_mode & 0777);
    times.actime = st->st_atime;
    times.modtime = st->st_mtime;
    if (utime(to, &times))
        w->dates_lost++;
    if (chmod(to, (mode_t)mode))
        return refuse(w->m, APKG_IO, "cannot set the protection of the copy of",
                      rel);
    apkg_status code = ak_attributes_write(to, attributes, mode);
    return code ? refuse(w->m, code,
                         "cannot set the Amiga attributes of the copy of", rel)
                : APKG_OK;
}
static apkg_status walk_in(mg_walk *w, const char *rel, unsigned depth,
                           char *from, char *sub, char *to) {
    char **names = NULL, *directory = from + AK_PATH;
    size_t n = 0;
    apkg_status code;
    if (depth > 32)
        return refuse(w->m, APKG_LIMIT, "drawers nested deeper than 32 levels",
                      rel);
    if (*rel ? ak_path(directory, w->from, rel)
             : (strcpy(directory, w->from), 0))
        return refuse(w->m, APKG_LIMIT, "path too long", rel);
    code = names_read(directory, &names, &n);
    if (code)
        return refuse(w->m, code == APKG_NOT_FOUND ? APKG_STALE : code,
                      "cannot list the drawer", *rel ? rel : w->from);
    for (size_t i = 0; i < n && !code; i++) {
        struct stat st;
        char *attributes = NULL, sha[65] = "-";
        uint64_t size = 0;
        unsigned mode;
        int joined = *rel ? snprintf(sub, AK_PATH, "%s/%s", rel, names[i])
                          : snprintf(sub, AK_PATH, "%s", names[i]);
        if (joined < 0 || joined >= AK_PATH || ak_path(from, w->from, sub) ||
            (w->to && ak_path(to, w->to, sub))) {
            code = refuse(w->m, APKG_LIMIT, "path too long", names[i]);
            break;
        }
        if (!ak_valid_path(sub)) {
            code = refuse(w->m, APKG_UNSUPPORTED,
                          "a name the common engine cannot represent (only "
                          "ASCII without \\ : \" is supported)",
                          sub);
            break;
        }
        if (lstat(from, &st)) {
            code = refuse(w->m, APKG_STALE, "an entry vanished while it was read",
                          sub);
            break;
        }
        if (!S_ISDIR(st.st_mode) && !(S_ISREG(st.st_mode) && st.st_nlink == 1)) {
            code = refuse(w->m, APKG_UNSUPPORTED,
                          "a link or special file cannot be migrated", sub);
            break;
        }
        if (!ak_metadata_supported(from, &st)) {
            code = refuse(w->m, APKG_UNSUPPORTED,
                          "ownership or attributes that cannot be preserved",
                          sub);
            break;
        }
        mode = (unsigned)(st.st_mode & 0777);
        code = ak_attributes_read(from, mode, &attributes);
        if (code) {
            refuse(w->m, code, "cannot read the Amiga attributes of", sub);
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            /* The buffers are reused below this level; keep the name. */
            char *kept = ak_copy(sub, strlen(sub));
            if (!kept)
                code = refuse(w->m, APKG_NOMEM, "out of memory", "");
            else if (w->to && mkdir(to, 0700))
                code = refuse(w->m, APKG_IO, "cannot create the copy of", sub);
            if (!code) {
                char *deeper = malloc(3 * (size_t)AK_PATH + AK_PATH);
                w->directories++;
                line(w, 'D', mode, 0, "-", attributes, kept);
                if (!deeper)
                    code = refuse(w->m, APKG_NOMEM, "out of memory", "");
                else
                    code = walk_in(w, kept, depth + 1, deeper,
                                   deeper + 2 * AK_PATH, deeper + 3 * AK_PATH);
                free(deeper);
            }
            if (!code && w->to) {
                if (ak_path(to, w->to, kept))
                    code = refuse(w->m, APKG_LIMIT, "path too long", kept);
                else
                    code = finish_node(w, kept, to, &st, attributes);
            }
            free(kept);
        } else {
            code = file_node(w, sub, from, w->to ? to : NULL, sha, &size);
            if (!code) {
                w->files++;
                w->bytes += size;
                line(w, 'F', mode, size, sha, attributes, sub);
                if (w->classify)
                    classify(w->m, sub, sha, size);
                if (w->to)
                    code = finish_node(w, sub, to, &st, attributes);
            }
        }
        free(attributes);
    }
    names_free(names, n);
    return code;
}
/* The digest of `from`; with `to`, a copy made on the way. */
static apkg_status walk(mg *m, const char *from, const char *to, int classify_,
                        mg_walk *w, char digest[65]) {
    struct stat st;
    unsigned char raw[32];
    char *attributes = NULL, *paths;
    apkg_status code;
    memset(w, 0, sizeof *w);
    w->m = m;
    w->from = from;
    w->to = to;
    w->classify = classify_;
    pkg_sha256_init(&w->inventory);
    if (lstat(from, &st) || !S_ISDIR(st.st_mode))
        return refuse(m, APKG_UNSUPPORTED, "not a physical directory", from);
    if (!ak_metadata_supported(from, &st))
        return refuse(m, APKG_UNSUPPORTED,
                      "ownership or attributes that cannot be preserved", from);
    code = ak_attributes_read(from, (uint64_t)(st.st_mode & 0777), &attributes);
    if (code)
        return refuse(m, code, "cannot read the Amiga attributes of", from);
    /* An exclusive claim: a destination that exists is never written into. */
    if (to && mkdir(to, 0700)) {
        free(attributes);
        return refuse(m, errno == EEXIST ? APKG_CONFLICT : APKG_IO,
                      "cannot create the destination", to);
    }
    w->buffer = malloc(MG_CHUNK);
    paths = malloc(3 * (size_t)AK_PATH + AK_PATH);
    if (!w->buffer || !paths)
        code = refuse(m, APKG_NOMEM, "out of memory", "");
    w->directories = 1;
    line(w, 'D', (unsigned)(st.st_mode & 0777), 0, "-", attributes, ".");
    if (!code)
        code = walk_in(w, "", 0, paths, paths + 2 * AK_PATH, paths + 3 * AK_PATH);
    if (!code && to)
        code = finish_node(w, ".", to, &st, attributes);
    free(paths);
    free(w->buffer);
    w->buffer = NULL;
    free(attributes);
    pkg_sha256_final(&w->inventory, raw);
    ak_tohex(raw, 32, digest);
    return code;
}

/* ---- The classic registry. ---- */
static apkg_status registry_read(mg *m) {
    static const char *const converted[] = {".pkg", ".apkg", MG_ARCHIVE};
    char path[AK_PATH], **names = NULL;
    size_t n = 0;
    apkg_status code;
    for (int i = 0; i < 3; i++)
        if (ak_path(path, m->source, converted[i]) || ak_exists(path))
            return refuse(m, APKG_POLICY,
                          "the root is mixed or already converted; it has",
                          converted[i]);
    if (ak_path(path, m->source, "db/lock.owner") || ak_exists(path))
        return refuse(m, APKG_BUSY,
                      "the classic client holds, or left, its lock "
                      "(db/lock.owner); let it finish or run its doctor "
                      "command, then try again",
                      "");
    if (ak_path(path, m->source, "db/transactions"))
        return refuse(m, APKG_LIMIT, "path too long", m->source);
    code = names_read(path, &names, &n);
    names_free(names, n);
    if (code && code != APKG_NOT_FOUND)
        return refuse(m, code, "cannot list the drawer", "db/transactions");
    if (!code && n)
        return refuse(m, APKG_RECOVERY_REQUIRED,
                      "an operation of the classic client is unfinished "
                      "(db/transactions); run its doctor command first",
                      "");
    if (ak_path(path, m->source, "db/replace.json") || ak_exists(path))
        return refuse(m, APKG_RECOVERY_REQUIRED,
                      "a replacement by the classic client is unfinished "
                      "(db/replace.json); any writing command of that client "
                      "finishes or reports it",
                      "");
    if (ak_path(path, m->source, "db/installed"))
        return refuse(m, APKG_LIMIT, "path too long", m->source);
    code = names_read(path, &names, &n);
    if (code)
        return refuse(m, code,
                      "this is not a root of the classic client: no registry "
                      "of installed packages (db/installed)",
                      m->source);
    if (!n || n > MG_PACKAGES) {
        names_free(names, n);
        return n ? refuse(m, APKG_LIMIT, "more installed packages than supported",
                          "")
                 : refuse(m, APKG_NOT_FOUND,
                          "no package is installed; there is nothing to migrate",
                          "");
    }
    m->packages = calloc(n, sizeof *m->packages);
    if (!m->packages)
        code = refuse(m, APKG_NOMEM, "out of memory", "");
    for (size_t i = 0; i < n && !code; i++) {
        mg_package *p = &m->packages[m->count];
        unsigned char *raw = NULL;
        size_t size = 0, length = strlen(names[i]);
        mj *record = NULL;
        int nomem = 0, parsed;
        const char *wrong;
        char file[AK_PATH];
        if (snprintf(file, sizeof file, "db/installed/%s", names[i]) >=
                (int)sizeof file ||
            ak_path(path, m->source, file)) {
            code = refuse(m, APKG_LIMIT, "path too long", names[i]);
            break;
        }
        if (ak_read(path, &raw, &size, AK_MAX)) {
            code = refuse(m, APKG_IO, "cannot read the installed record", file);
            break;
        }
        m->count++; /* owned from here, so cleanup frees what it holds */
        pkg_sha256_hex(raw, size, p->record_sha);
        parsed = mj_parse(raw, size, &record);
        free(raw);
        if (parsed) {
            code = refuse(m, parsed == -2 ? APKG_NOMEM : APKG_INVALID,
                          "the installed record is not readable JSON", file);
            break;
        }
        wrong = record_read(record, p, &nomem);
        mj_free(record);
        if (wrong) {
            code = refuse(m, nomem ? APKG_NOMEM : APKG_UNSUPPORTED, wrong, file);
            break;
        }
        if (length != strlen(p->name) + 5 ||
            strncmp(names[i], p->name, length - 5) ||
            strcmp(names[i] + length - 5, ".json")) {
            code = refuse(m, APKG_INVALID,
                          "the record's file name is not its package name", file);
            break;
        }
        if (ak_path(path, m->source, p->name) || !is_directory(path))
            code = refuse(m, APKG_INTEGRITY,
                          "the drawer of an installed package is missing",
                          p->name);
    }
    names_free(names, n);
    if (code)
        return code;
    qsort(m->packages, m->count, sizeof *m->packages, packages_order);
    /* The recorded set must be consistent on its own. */
    for (size_t i = 0; i < m->count; i++) {
        const mg_package *p = &m->packages[i];
        for (size_t k = 0; k < p->ndepends + p->nexcludes; k++) {
            const char *other = k < p->ndepends
                                    ? p->depends[k]
                                    : p->excludes[k - p->ndepends];
            int installed = 0;
            char pair[600];
            for (size_t x = 0; x < m->count; x++)
                if (!strcmp(m->packages[x].name, other))
                    installed = 1;
            snprintf(pair, sizeof pair, "%s, %s", p->name, other);
            if (k < p->ndepends && !installed)
                return refuse(m, APKG_DEPENDENCY,
                              "a recorded dependency is not installed", pair);
            if (k >= p->ndepends && installed)
                return refuse(m, APKG_CONFLICT,
                              "a recorded conflict or replacement names an "
                              "installed package",
                              pair);
        }
    }
    return APKG_OK;
}

/* ---- Output. ---- */
static void text(ak_buf *b, const char *s) {
    ak_buf_add(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            ak_buf_add(b, "\\%c", c);
        else if (c < 32)
            ak_buf_add(b, "\\u%04x", c);
#ifdef __AROS__ /* names there are ISO 8859-1 */
        else if (c >= 128)
            ak_buf_add(b, "%c%c", 0xc0 | (c >> 6), 0x80 | (c & 63));
#endif
        else
            ak_buf_add(b, "%c", c);
    }
    ak_buf_add(b, "\"");
}
static const char *icon_fact(const mg_package *p) {
    if (strcmp(p->icon_state, "installed"))
        return p->icon_state;
    if (!p->icon_found)
        return "installed-missing";
    if (!*p->icon_sha)
        return "installed-hash-unrecorded";
    return strcmp(p->icon_sha, p->icon_now) ? "installed-changed"
                                            : "installed-delivered";
}
static uint64_t missing(const mg_package *p) {
    uint64_t n = 0;
    for (size_t i = 0; i < p->nfiles; i++)
        n += !p->files[i].seen;
    return n;
}
static void stamp(char out[32]) {
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    if (!utc || !strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", utc))
        strcpy(out, "unknown");
}
/* One drawer of the classic client's environment (arospkg selfup.h): the
 * self-update journal and the copies record in ENVARC:arospkg, the lock in
 * ENV:arospkg. `given` is a copy the operator handed over. */
static apkg_status environment_drawer(mg *m, const char *path, int envarc,
                                      int given, ak_buf *b, int *inspected) {
    const char *label = envarc ? "ENVARC:arospkg" : "ENV:arospkg";
    char **names = NULL, when[32], held[AK_PATH] = "";
    size_t n = 0;
    int copies = 0, result = 0;
    apkg_status code;
    *inspected = 0;
    if (!path) {
        ak_buf_add(b, "{\"state\":\"not-inspected\"}");
        return APKG_OK;
    }
    code = names_read(path, &names, &n);
    if (code == APKG_NOT_FOUND && !given)
        code = APKG_OK; /* the client never wrote there */
    else if (code || !is_directory(path)) {
        names_free(names, n);
        return refuse(m, code ? code : APKG_UNSUPPORTED,
                      "the copy of the classic client's drawer is not a "
                      "physical directory",
                      path);
    }
    for (size_t i = 0; i < n && !code; i++) {
        size_t length = strlen(names[i]);
        if (length > 4 && !strcmp(names[i] + length - 4, ".new"))
            code = refuse(m, APKG_RECOVERY_REQUIRED,
                          "a partial write of the classic client needs its "
                          "recovery first",
                          names[i]);
        else if (envarc && !strcmp(names[i], "self-update.json"))
            code = refuse(m, APKG_RECOVERY_REQUIRED,
                          "a self-update of the classic client is unfinished "
                          "(ENVARC:arospkg/self-update.json); start that "
                          "client once to finish or undo it",
                          "");
        else if (envarc && !strcmp(names[i], "copies.json"))
            copies = 1;
        else if (!envarc && !strcmp(names[i], "self-update-result.json"))
            result = 1;
        else if (!envarc && !strncmp(names[i], "self-update.", 12) && !*held)
            snprintf(held, sizeof held, "%s", names[i]);
    }
    names_free(names, n);
    if (!code && *held)
        code = refuse(m, APKG_BUSY,
                      "a self-update of the classic client holds its lock in "
                      "ENV:arospkg; let it finish or restart, then try again",
                      held);
    if (code)
        return code;
    stamp(when);
    ak_buf_add(b, "{\"checked\":[%s]",
               envarc ? "\"self-update.json\",\"*.new\",\"copies.json\""
                      : "\"self-update.lock\",\"self-update.<nonce>\","
                        "\"*.new\",\"self-update-result.json\"");
    if (envarc && copies) {
        /* Where apkg copies such as C:apkg are: kept, not converted. */
        char file[AK_PATH];
        unsigned char *raw = NULL;
        size_t size = 0;
        mj *parsed = NULL;
        if (ak_path(file, path, "copies.json") ||
            ak_read(file, &raw, &size, 65536) || mj_parse(raw, size, &parsed)) {
            free(raw);
            return refuse(m, APKG_INVALID,
                          "the record of apkg copies is not readable",
                          "ENVARC:arospkg/copies.json");
        }
        mj_free(parsed);
        ak_buf_add(b, ",\"copies\":\"retained-not-converted\",\"copies_record\":\"");
        for (size_t i = 0; i < size; i++)
            ak_buf_add(b, "%02x", raw[i]);
        ak_buf_add(b, "\"");
        free(raw);
    } else if (envarc)
        ak_buf_add(b, ",\"copies\":\"none\"");
    ak_buf_add(b, ",\"drawer\":\"%s\",\"inspected_at\":\"%s\",\"source\":\"%s\","
                  "\"state\":\"inspected\"",
               label, when, given ? "copy" : "this-system");
    if (!envarc)
        ak_buf_add(b, ",\"unread_result\":%s", result ? "true" : "false");
    ak_buf_add(b, "}");
    *inspected = 1;
    return APKG_OK;
}
static apkg_status environment(mg *m, ak_buf *b, const char **state) {
    const char *envarc = m->envarc, *env = m->env;
    int given_envarc = envarc != NULL, given_env = env != NULL, a = 0, c = 0;
    apkg_status code;
#ifdef __AROS__ /* on the system itself the drawers are read where they are */
    if (!envarc)
        envarc = "ENVARC:arospkg";
    if (!env)
        env = "ENV:arospkg";
#endif
    ak_buf_add(b, "{\"env\":");
    code = environment_drawer(m, env, 0, given_env, b, &c);
    if (code)
        return code;
    ak_buf_add(b, ",\"envarc\":");
    code = environment_drawer(m, envarc, 1, given_envarc, b, &a);
    if (code)
        return code;
    *state = a && c ? "inspected" : a || c ? "partial" : "not-inspected";
    ak_buf_add(b,
               ",\"meaning\":\"what the drawers held when they were read; it "
               "does not show that no self-update runs now\",\"state\":\"%s\"}",
               *state);
    return APKG_OK;
}
static void document(const mg *m, const mg_walk *w, const char *digest,
                     const char *environment_json, ak_buf *b) {
    ak_buf_add(b, "{\"body\":{\"destination\":");
    if (*m->destination)
        text(b, m->destination);
    else
        ak_buf_add(b, "null");
    ak_buf_add(b,
               ",\"environment\":%s,\"inventory\":{\"bytes\":\"%" PRIu64
               "\",\"directories\":\"%" PRIu64 "\",\"files\":\"%" PRIu64
               "\",\"sha256\":\"%s\"},\"packages\":[",
               environment_json, w->bytes, w->directories, w->files, digest);
    for (size_t i = 0; i < m->count; i++) {
        const mg_package *p = &m->packages[i];
        ak_buf_add(b, "%s{\"abi\":", i ? "," : "");
        if (p->has_abi)
            text(b, p->abi);
        else
            ak_buf_add(b, "null");
        ak_buf_add(b, ",\"arch\":");
        text(b, p->arch);
        ak_buf_add(b,
                   ",\"authentication\":\"none-recorded\",\"files\":{\"changed\":"
                   "\"%" PRIu64 "\",\"missing\":\"%" PRIu64
                   "\",\"recorded\":\"%" PRIu64 "\",\"unchanged\":\"%" PRIu64
                   "\",\"unrecorded\":\"%" PRIu64
                   "\"},\"icon\":\"%s\",\"name\":\"%s\",\"previous\":{\"archive\":",
                   p->changed, missing(p), (uint64_t)p->nfiles, p->unchanged,
                   p->unrecorded, icon_fact(p), p->name);
        if (p->archive && *p->archive)
            text(b, p->archive);
        else
            ak_buf_add(b, "null");
        ak_buf_add(b, ",\"registry\":");
        if (p->registry && *p->registry)
            text(b, p->registry);
        else
            ak_buf_add(b, "null");
        ak_buf_add(b, "},\"reason\":\"%s\",\"record_sha256\":\"%s\",\"revision\":",
                   p->reason, p->record_sha);
        if (p->has_revision)
            ak_buf_add(b, "\"%" PRIu64 "\"", p->revision);
        else
            ak_buf_add(b, "null");
        ak_buf_add(b, ",\"version\":");
        text(b, p->version);
        ak_buf_add(b, "}");
    }
    ak_buf_add(b, "],\"quiescent_operator_assertion\":%s,\"source\":",
               m->quiescent ? "true" : "false");
    text(b, m->source);
    ak_buf_add(b, "},\"format\":\"" MG_FORMAT "\"}");
}
static void summary(const mg *m, const mg_walk *w, const char *digest,
                    const char *environment_state) {
    printf("Classic root: %s\n", m->source);
    for (size_t i = 0; i < m->count; i++) {
        const mg_package *p = &m->packages[i];
        const char *icon = icon_fact(p);
        printf("  %s %s", p->name, p->version);
        if (p->has_revision)
            printf(" revision %" PRIu64, p->revision);
        printf(" (%s%s%s, %s): files recorded %" PRIu64 ": %" PRIu64
               " unchanged, %" PRIu64 " changed, %" PRIu64
               " missing; other files in the drawer: %" PRIu64,
               p->arch, p->has_abi ? " " : "", p->abi,
               !strcmp(p->reason, "explicit")     ? "installed on request"
               : !strcmp(p->reason, "dependency") ? "installed as a dependency"
                                                  : "reason not recorded",
               (uint64_t)p->nfiles, p->unchanged, p->changed, missing(p),
               p->unrecorded);
        if (strcmp(icon, "none"))
            printf("; drawer icon: %s",
                   !strcmp(icon, "installed-delivered") ? "as delivered"
                   : !strcmp(icon, "installed-changed") ? "changed since installation"
                   : !strcmp(icon, "installed-missing") ? "missing"
                   : !strcmp(icon, "kept-existing")
                       ? "the user's own, kept at installation"
                       : "installed, delivered bytes not recorded");
        putchar('\n');
    }
    printf("Classic client's own state (ENVARC:arospkg, ENV:arospkg): %s\n",
           environment_state);
    printf("Inventory: %" PRIu64 " files in %" PRIu64 " drawers, %" PRIu64
           " bytes, SHA-256 %s\n",
           w->files, w->directories, w->bytes, digest);
}

/* ---- Paths. ---- */
static int same_path(const char *a, const char *b) {
#ifdef __AROS__
    return !fold_compare(a, b);
#else
    return !strcmp(a, b);
#endif
}
static int inside(const char *path, const char *directory) {
    size_t n = strlen(directory);
    if (strlen(path) <= n)
        return 0;
#ifdef __AROS__
    if (!fold_prefix(path, directory, n))
        return 0;
#else
    if (strncmp(path, directory, n))
        return 0;
#endif
    return path[n] == '/' || (n && (directory[n - 1] == '/' ||
                                    directory[n - 1] == ':'));
}
/* The destination must not exist; its parent must. */
static apkg_status destination_path(mg *m, const char *typed) {
    char parent[AK_PATH], canonical[AK_PATH], trimmed[AK_PATH];
    const char *leaf;
    size_t n = strlen(typed);
    if (!n || n >= AK_PATH)
        return refuse(m, APKG_INVALID, "invalid destination", typed);
    strcpy(trimmed, typed);
    while (n > 1 && trimmed[n - 1] == '/')
        trimmed[--n] = 0;
    leaf = strrchr(trimmed, '/');
    if (!leaf)
        leaf = strrchr(trimmed, ':');
    if (leaf) {
        size_t head = (size_t)(leaf - trimmed) + (*leaf == ':' || leaf == trimmed);
        memcpy(parent, trimmed, head);
        parent[head] = 0;
        leaf++;
    } else {
        strcpy(parent, ".");
        leaf = trimmed;
    }
    if (!*leaf || !strcmp(leaf, ".") || !strcmp(leaf, "..") || !ak_valid_path(leaf))
        return refuse(m, APKG_INVALID, "invalid destination name", typed);
    if (pkg_fs_canonical_dir(parent, canonical, sizeof canonical))
        return refuse(m, APKG_NOT_FOUND,
                      "the drawer that is to hold the copy does not exist",
                      parent);
    if (ak_path(m->destination, canonical, leaf))
        return refuse(m, APKG_LIMIT, "path too long", typed);
    if (ak_exists(m->destination))
        return refuse(m, APKG_CONFLICT,
                      "the destination exists; a copy is only made into a new "
                      "drawer",
                      m->destination);
    if (same_path(m->destination, m->source) ||
        inside(m->destination, m->source) || inside(m->source, m->destination))
        return refuse(m, APKG_INVALID,
                      "the copy cannot be inside the root it is made from",
                      m->destination);
    return APKG_OK;
}

static apkg_status run(mg *m, const char *source, const char *destination) {
    char digest[65], copied[65], check[65], path[AK_PATH], other[AK_PATH];
    const char *environment_state = "not-inspected";
    ak_buf env = {0}, record = {0};
    mg_walk seen, made, verified;
    apkg_status code;
    env.max = 1048576;
    record.max = AK_MAX;
    if (pkg_fs_path_is_link(source) != 0 ||
        pkg_fs_canonical_dir(source, m->source, sizeof m->source))
        return refuse(m, APKG_NOT_FOUND, "no such physical directory", source);
    if (destination && (code = destination_path(m, destination)))
        return code;
    if ((code = registry_read(m)) || (code = environment(m, &env, &environment_state)))
        goto done;
    if (env.bad) {
        code = refuse(m, (apkg_status)env.bad, "out of memory", "");
        goto done;
    }
    if ((code = walk(m, m->source, NULL, 1, &seen, digest)))
        goto done;
    if (*m->selfupdate) {
        code = refuse(m, APKG_RECOVERY_REQUIRED,
                      "self-update state of the classic client lies in the "
                      "root; start that client once to finish or undo it",
                      m->selfupdate);
        goto done;
    }
    if (!destination) {
        if (m->json) {
            document(m, &seen, digest, env.data, &record);
            if (record.bad)
                code = refuse(m, (apkg_status)record.bad, "out of memory", "");
            else
                puts(record.data);
        } else {
            summary(m, &seen, digest, environment_state);
            puts("Nothing was changed.");
        }
        goto done;
    }
    /* Copy, hashing what is read: equal digests show that the bytes copied
     * are the bytes inspected. Then read the copy back. */
    if ((code = walk(m, m->source, m->destination, 0, &made, copied)))
        goto partial;
    if (strcmp(copied, digest)) {
        code = refuse(m, APKG_STALE,
                      "the root changed while it was copied; no classic client "
                      "may run during migration",
                      m->source);
        goto partial;
    }
    if (ak_sync_dir(m->destination)) {
        code = refuse(m, APKG_IO, "cannot flush the copy", m->destination);
        goto partial;
    }
    if ((code = walk(m, m->destination, NULL, 0, &verified, check)))
        goto partial;
    if (strcmp(check, digest)) {
        code = refuse(m, APKG_INTEGRITY, "the copy differs from the original",
                      m->destination);
        goto partial;
    }
    /* The classic registry leaves the place a classic client looks in. */
    if (ak_path(path, m->destination, MG_ARCHIVE) || mkdir(path, 0700) ||
        ak_path(path, m->destination, "db") ||
        ak_path(other, m->destination, MG_ARCHIVE "/db") || rename(path, other)) {
        code = refuse(m, APKG_IO, "cannot set the classic registry aside in",
                      m->destination);
        goto partial;
    }
    document(m, &seen, digest, env.data, &record);
    if (record.bad) {
        code = refuse(m, (apkg_status)record.bad, "out of memory", "");
        goto partial;
    }
    if (ak_path(path, m->destination, MG_ARCHIVE "/inspection.json") ||
        ak_write(path, record.data, record.size, 1) ||
        ak_sync_dir(m->destination)) {
        code = refuse(m, APKG_IO, "cannot write the inspection record in",
                      m->destination);
        goto partial;
    }
    pkg_sha256_hex(record.data, record.size, check);
    if (m->json) {
        ak_buf answer = {0};
        answer.max = 2 * AK_PATH;
        ak_buf_add(&answer, "{\"destination\":");
        text(&answer, m->destination);
        ak_buf_add(&answer, ",\"inspection_sha256\":\"%s\",\"status\":\"copied\"}",
                   check);
        if (!answer.bad)
            puts(answer.data);
        free(answer.data);
    } else {
        summary(m, &seen, digest, environment_state);
        printf("Copied to: %s\n"
               "The classic registry is kept in: %s/" MG_ARCHIVE "/db\n"
               "Inspection record: %s/" MG_ARCHIVE "/inspection.json\n"
               "SHA-256 of that record: %s\n",
               m->destination, m->destination, m->destination, check);
        if (made.dates_lost)
            printf("Note: %u dates of files or drawers could not be kept.\n",
                   made.dates_lost);
        puts("The original root was only read. The copy is not yet registered "
             "with the common engine.");
    }
    goto done;
partial:
    fprintf(stderr,
            "An unfinished copy remains at %s. It is never reused: delete it "
            "before trying again.\n",
            m->destination);
done:
    free(env.data);
    free(record.data);
    return code;
}
/* ---- Registration: the copy's packages as unsigned entries. ---- */
/* Builds the adoption set that registers the packages of a copied root with
 * the common engine. Each row carries a descriptor made from the record the
 * classic client wrote (reserved authority, files named by hash, no bytes)
 * and that record as evidence. Nothing is run and no launch data is chosen:
 * the record stays in the evidence, application.json stays in the drawer. */
static int strcmp_files(const void *a, const void *b) {
    return strcmp(((const mg_file *)a)->path, ((const mg_file *)b)->path);
}
typedef struct {
    char name[64], minimum[64];
} mg_need;
static int needs_order(const void *a, const void *b) {
    return strcmp(((const mg_need *)a)->name, ((const mg_need *)b)->name);
}
static const char *launch_text(const mj *object, const char *key) {
    const mj *launch = object ? mj_get(object, "launch") : NULL;
    const char *t = launch && launch->type == '{' ? mj_text(mj_get(launch, key))
                                                  : NULL;
    return t ? t : "";
}
static apkg_status register_row(mg *m, const char *root, const char *target,
                                const char *file, ak_buf *rows, FILE *notes) {
    char path[AK_PATH], drawer[AK_PATH];
    unsigned char *raw = NULL, *app = NULL;
    size_t size = 0, app_size = 0, nneeds = 0;
    mj *record = NULL, *application = NULL;
    mg_package p = {0};
    mg_need needs[MG_RELATIONS];
    int nomem = 0, parsed;
    const char *wrong;
    const mj *v;
    apkg_status code = APKG_OK;
    ak_buf d = {0}, e = {0};
    char prefix[160];
    d.max = e.max = AK_MAX;
    char relative[AK_PATH];
    if (snprintf(relative, sizeof relative, MG_ARCHIVE "/db/installed/%s",
                 file) >= (int)sizeof relative ||
        ak_path(path, root, relative))
        return refuse(m, APKG_LIMIT, "path too long", file);
    if (ak_read(path, &raw, &size, AK_MAX))
        return refuse(m, APKG_IO, "cannot read the retained record", file);
    parsed = mj_parse(raw, size, &record);
    if (parsed) {
        code = refuse(m, parsed == -2 ? APKG_NOMEM : APKG_INVALID,
                      "the retained record is not readable JSON", file);
        goto done;
    }
    wrong = record_read(record, &p, &nomem);
    if (wrong) {
        code = refuse(m, nomem ? APKG_NOMEM : APKG_UNSUPPORTED, wrong, file);
        goto done;
    }
    if (strlen(file) != strlen(p.name) + 5 ||
        strncmp(file, p.name, strlen(p.name))) {
        code = refuse(m, APKG_INVALID,
                      "the record's file name is not its package name", file);
        goto done;
    }
    if ((v = mj_get(record, "dir")) &&
        (!mj_text(v) || strcmp(v->text, p.name))) {
        code = refuse(m, APKG_UNSUPPORTED,
                      "the package's drawer is not named after the package",
                      p.name);
        goto done;
    }
    for (const char *c = p.version; *c; c++)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') ||
              (*c >= 'A' && *c <= 'Z') || strchr("._-+", *c))) {
            code = refuse(m, APKG_UNSUPPORTED,
                          "the recorded version has characters the common "
                          "engine does not accept",
                          p.name);
            goto done;
        }
    /* The record must be for the system this root is for. */
    snprintf(prefix, sizeof prefix, "%s-aros-%s-", p.arch,
             p.has_abi ? p.abi : "");
    if (p.has_abi ? strncmp(target, prefix, strlen(prefix)) != 0
                  : (strncmp(target, p.arch, strlen(p.arch)) != 0 ||
                     target[strlen(p.arch)] != '-')) {
        code = refuse(m, APKG_TARGET,
                      "the package was recorded for another system than this "
                      "root's target",
                      p.name);
        goto done;
    }
    if (ak_path(drawer, root, p.name) || !is_directory(drawer)) {
        code = refuse(m, APKG_INTEGRITY, "the package's drawer is missing",
                      p.name);
        goto done;
    }
    /* Dependencies among the migrated packages keep protecting removal. A
     * recorded conflict, replacement or recommendation stays in the evidence:
     * under the reserved authority it could never match a signed package. */
    if ((v = mj_get(record, "depends")))
        for (const mj *item = v->child; item; item = item->next) {
            const char *t = mj_text(item), *min;
            if (!t || nneeds == MG_RELATIONS ||
                !relation(t, 1, needs[nneeds].name)) {
                code = refuse(m, APKG_UNSUPPORTED, "unreadable dependency",
                              p.name);
                goto done;
            }
            min = strstr(t, " >= ");
            snprintf(needs[nneeds].minimum, sizeof needs[nneeds].minimum, "%s",
                     min ? min + 4 : "");
            nneeds++;
        }
    if (nneeds > 1)
        qsort(needs, nneeds, sizeof *needs, needs_order);
    if (p.nfiles > 1)
        qsort(p.files, p.nfiles, sizeof *p.files, strcmp_files);
    /* Launch data: check what the record names, choose nothing. */
    v = mj_get(record, "launch");
    if (v) {
        const char *exe = launch_text(record, "executable");
        struct stat st;
        char full[AK_PATH];
        if (v->type != '{' || !*exe || !ak_valid_path(exe) ||
            ak_path(full, drawer, exe)) {
            code = refuse(m, APKG_INVALID,
                          "the recorded launch data names no valid program "
                          "inside the drawer",
                          p.name);
            goto done;
        }
        if (lstat(full, &st) || !S_ISREG(st.st_mode))
            fprintf(notes,
                    "note: %s: the recorded program \"%s\" is not in the "
                    "drawer\n",
                    p.name, exe);
        if (!ak_path(full, drawer, "application.json") &&
            !ak_read(full, &app, &app_size, AK_MAX)) {
            if (mj_parse(app, app_size, &application))
                fprintf(notes,
                        "note: %s: application.json in the drawer is not "
                        "readable; the record's launch data is kept as "
                        "evidence\n",
                        p.name);
            else if (strcmp(exe, launch_text(application, "executable")) ||
                     strcmp(launch_text(record, "mode"),
                            launch_text(application, "mode")))
                fprintf(notes,
                        "note: %s: launch data disagrees. Record: program "
                        "\"%s\", mode \"%s\". application.json: program "
                        "\"%s\", mode \"%s\". Neither is chosen; both are "
                        "kept.\n",
                        p.name, exe, launch_text(record, "mode"),
                        launch_text(application, "executable"),
                        launch_text(application, "mode"));
        }
    }
    /* Evidence. */
    ak_buf_add(&e, "{\"body\":{\"baseline\":[");
    for (size_t i = 0; i < p.nfiles; i++) {
        ak_buf_add(&e, "%s{\"path\":", i ? "," : "");
        text(&e, p.files[i].path);
        ak_buf_add(&e, ",\"sha256\":\"%s\",\"size\":\"%" PRIu64 "\"}",
                   p.files[i].sha, p.files[i].size);
    }
    ak_buf_add(&e, "],\"reason\":\"%s\",\"record\":\"",
               !strcmp(p.reason, "explicit") || !strcmp(p.reason, "dependency")
                   ? p.reason
                   : "unrecorded");
    for (size_t i = 0; i < size; i++)
        ak_buf_add(&e, "%02x", raw[i]);
    ak_buf_add(&e, "\",\"source\":\"arospkg-v1\"},\"format\":\"apkg-json-1\","
                   "\"requires\":[],\"schema\":\"1\",\"type\":\"adoption\"}");
    /* Descriptor. */
    ak_buf_add(&d, "{\"body\":{\"authority\":\"" AK_UNSIGNED_AUTHORITY
                   "\",\"files\":[");
    for (size_t i = 0; i < p.nfiles; i++) {
        struct stat st;
        char full[AK_PATH];
        unsigned mode = 0644;
        if (!ak_path(full, drawer, p.files[i].path) && !lstat(full, &st) &&
            S_ISREG(st.st_mode))
            mode = (unsigned)(st.st_mode & 0777) | 0400;
        ak_buf_add(&d, "%s{\"blob\":\"%s\",\"mode\":\"%u\",\"path\":",
                   i ? "," : "", p.files[i].sha, mode);
        text(&d, p.files[i].path);
        ak_buf_add(&d,
                   ",\"sha256\":\"%s\",\"size\":\"%" PRIu64
                   "\",\"type\":\"file\"}",
                   p.files[i].sha, p.files[i].size);
    }
    ak_buf_add(&d, "],\"kind\":\"%s\",\"name\":\"%s\",\"relations\":{"
                   "\"conflicts\":[],\"depends\":[",
               v ? "application" : "data", p.name);
    for (size_t i = 0; i < nneeds; i++)
        ak_buf_add(&d,
                   "%s{\"authority\":\"" AK_UNSIGNED_AUTHORITY
                   "\",\"min_revision\":\"0\",\"min_version\":\"%s\","
                   "\"name\":\"%s\",\"target\":\"%s\"}",
                   i ? "," : "", needs[i].minimum, needs[i].name, target);
    ak_buf_add(&d,
               "],\"recommends\":[],\"replaces\":[]},\"revision\":\"%" PRIu64
               "\",\"target\":\"%s\",\"version\":\"%s\"},\"format\":"
               "\"apkg-json-1\",\"requires\":[],\"schema\":\"1\",\"type\":"
               "\"descriptor\"}",
               p.has_revision ? p.revision : 0, target, p.version);
    if (d.bad || e.bad) {
        code = refuse(m, d.bad == APKG_NOMEM || e.bad == APKG_NOMEM
                             ? APKG_NOMEM
                             : APKG_LIMIT,
                      "the record is too large to register", p.name);
        goto done;
    }
    ak_buf_add(rows, "%s{\"descriptor\":%s,\"evidence\":%s}",
               rows->size > 30 ? "," : "", d.data, e.data);
done:
    free(d.data);
    free(e.data);
    free(raw);
    free(app);
    mj_free(record);
    mj_free(application);
    package_free(&p);
    return code;
}
/* Writes ROOT/.apkg-legacy/adoption-set.json and returns its path. */
int ak_migrate_manifest(const char *root, char *manifest, size_t manifest_size) {
    mg *m = calloc(1, sizeof *m);
    apkg_open_options o = {0};
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    char path[AK_PATH], target[64] = "", **names = NULL;
    size_t n = 0;
    ak_buf rows = {0};
    apkg_status code;
    if (!m)
        return APKG_NOMEM;
    o.contract_version = 1;
    o.struct_size = sizeof o;
    o.mode = APKG_READ;
    o.root.data = root;
    o.root.size = strlen(root);
    code = apkg_open(&o, &c, NULL);
    if (!code)
        code = apkg_snapshot_read(c, &s, NULL);
    if (!code && (s->view.state != APKG_ROOT_READY || !s->target ||
                  strlen(s->target) >= sizeof target))
        code = APKG_POLICY;
    if (!code)
        strcpy(target, s->target);
    apkg_snapshot_free(s);
    apkg_close(c);
    if (code) {
        refuse(m, code,
               "the copy is not an initialized root of the common engine; "
               "run \"apkg init\" on it first",
               root);
        goto done;
    }
    if (ak_path(path, root, MG_ARCHIVE "/db/installed")) {
        code = refuse(m, APKG_LIMIT, "path too long", root);
        goto done;
    }
    code = names_read(path, &names, &n);
    if (code || !n) {
        code = refuse(m, code ? code : APKG_NOT_FOUND,
                      "this drawer holds no registry set aside by \"apkg "
                      "migrate copy\"",
                      root);
        goto done;
    }
    if (n > 128) {
        code = refuse(m, APKG_LIMIT,
                      "more than 128 packages; one registration cannot hold "
                      "them yet",
                      root);
        goto done;
    }
    rows.max = AK_MAX;
    ak_buf_add(&rows, "{\"body\":{\"packages\":[");
    for (size_t i = 0; i < n && !code; i++)
        code = register_row(m, root, target, names[i], &rows, stderr);
    if (code)
        goto done;
    ak_buf_add(&rows, "]},\"format\":\"apkg-json-1\",\"requires\":[],"
                      "\"schema\":\"1\",\"type\":\"adoption-set\"}");
    if (rows.bad) {
        code = refuse(m, rows.bad, "the adoption set is too large", root);
        goto done;
    }
    if (manifest_size < AK_PATH ||
        ak_path(manifest, root, MG_ARCHIVE "/adoption-set.json")) {
        code = refuse(m, APKG_LIMIT, "path too long", root);
        goto done;
    }
    if (ak_write(manifest, rows.data, rows.size, 0))
        code = refuse(m, APKG_IO, "cannot write the adoption set", manifest);
done:
    if (code)
        fprintf(stderr, "registration refused: %s\n",
                *m->why ? m->why : "internal error");
    names_free(names, n);
    free(rows.data);
    free(m);
    return (int)code;
}
int ak_migrate_main(int argc, char **argv) {
    static const char usage[] =
        "Usage: apkg migrate check ROOT [--json]\n"
        "       apkg migrate copy ROOT NEW-DRAWER --quiescent [--json]\n"
        "Both accept --legacy-envarc DRAWER and --legacy-env DRAWER: copies of "
        "ENVARC:arospkg and ENV:arospkg\n(on AROS the drawers themselves are "
        "read when no copy is named).\n"
        "check reads a root of the classic client and changes nothing. copy "
        "makes a checked copy in a new\ndrawer and sets the classic registry "
        "aside inside the copy; the original is only read.\n--quiescent states "
        "that no classic client runs during the copy.\n";
    const char *words[3] = {NULL, NULL, NULL};
    int nwords = 0, copy, bad = 0;
    mg *m;
    apkg_status code;
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        fputs(usage, stdout);
        return APKG_OK;
    }
    m = calloc(1, sizeof *m);
    if (!m)
        return APKG_NOMEM;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json"))
            m->json = 1;
        else if (!strcmp(argv[i], "--quiescent"))
            m->quiescent = 1;
        else if (!strcmp(argv[i], "--legacy-envarc") && i + 1 < argc)
            m->envarc = argv[++i];
        else if (!strcmp(argv[i], "--legacy-env") && i + 1 < argc)
            m->env = argv[++i];
        else if (argv[i][0] == '-' || nwords == 3)
            bad = 1;
        else
            words[nwords++] = argv[i];
    }
    copy = nwords && !strcmp(words[0], "copy");
    if (bad || !nwords || (!copy && strcmp(words[0], "check")) ||
        nwords != (copy ? 3 : 2) || (!copy && m->quiescent)) {
        fputs(usage, stderr);
        free(m);
        return APKG_INVALID;
    }
    if (copy && !m->quiescent)
        code = refuse(m, APKG_POLICY,
                      "copy needs --quiescent: no classic client (apkg, "
                      "PkgManager) may run while the root is copied",
                      "");
    else
        code = run(m, words[1], copy ? words[2] : NULL);
    if (code)
        fprintf(stderr, "migration refused: %s\n",
                *m->why ? m->why : "internal error");
    for (size_t i = 0; i < m->count; i++)
        package_free(&m->packages[i]);
    free(m->packages);
    free(m);
    return (int)code;
}
