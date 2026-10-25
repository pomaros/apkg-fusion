/* What is new in the catalogue for this machine: the pure half of
 * pkg_news (pkg.h). No AROS calls, so the host tests link the code the
 * guest runs; news_io.c reads the files and keeps the record.
 *
 * Every question here is asked about the build for THIS machine, chosen as
 * index_select_variant() chooses it: among the entries the machine runs
 * natively, the smallest (arch, abi, url). An application whose only new
 * entry is for another CPU is not news here.
 *
 * Both catalogues are read once into rows sorted by id, so comparing two
 * catalogues of a few thousand entries is a sort and a walk, not a search
 * per entry: on a TCG guest that is the difference between instant and
 * seconds. */
#include "news.h"
#include "entries.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char id[PKG_MAXID];       /* lower case: AROS compares names that way */
    char arch[16], abi[8], url[512];
    int  el;                  /* the element in its catalogue */
    int  native;
} cat_row;

typedef struct { const char *json; js_tok *t; int ntok; cat_row *r; int n; } catalogue;

static void lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 'a' - 'A'; }

static void mstr(const char *json, const js_tok *t, int n, int obj, const char *name, char *buf, size_t len)
{
    buf[0] = 0;
    if (js_str(json, t, js_member(json, t, n, obj, name), buf, len) != 0) buf[0] = 0;
}

static int cmp_row(const void *a, const void *b)
{
    const cat_row *x = (const cat_row *)a, *y = (const cat_row *)b;
    int c = strcmp(x->id, y->id);
    if (c) return c;
    /* Natives first, then index_select_variant's order: the first native
       row of a group is the one chosen. */
    if (x->native != y->native) return y->native - x->native;
    if ((c = strcmp(x->arch, y->arch)) != 0) return c;
    if ((c = strcmp(x->abi, y->abi)) != 0) return c;
    return strcmp(x->url, y->url);
}

static void cat_free(catalogue *c) { free(c->t); free(c->r); memset(c, 0, sizeof *c); }

/* 0, or a JS_E* code. */
static int cat_read(catalogue *c, const char *json, size_t len, const char *my_arch, const char *my_abi)
{
    int arr, el, k = 0;
    memset(c, 0, sizeof *c);
    c->json = json;
    c->ntok = js_parse_alloc(json, len, &c->t, PKG_JSON_MAXTOK);
    if (c->ntok < 0) { int rc = c->ntok; c->t = NULL; return rc; }
    arr = js_member(json, c->t, c->ntok, 0, "packages");
    if (arr < 0 || c->t[arr].type != JS_ARR) { cat_free(c); return JS_EINVAL; }
    c->r = (cat_row *)calloc((size_t)c->t[arr].size + 1, sizeof *c->r);
    if (!c->r) { cat_free(c); return JS_ENOMEM; }
    for (el = js_child(c->t, c->ntok, arr); el >= 0; el = js_sibling(c->t, c->ntok, el)) {
        cat_row *r = &c->r[k];
        char why[8];
        if (c->t[el].type != JS_OBJ) continue;
        mstr(json, c->t, c->ntok, el, "id", r->id, sizeof r->id);
        if (!r->id[0]) continue;
        lower(r->id);
        mstr(json, c->t, c->ntok, el, "arch", r->arch, sizeof r->arch);
        mstr(json, c->t, c->ntok, el, "abi", r->abi, sizeof r->abi);
        mstr(json, c->t, c->ntok, el, "url", r->url, sizeof r->url);
        r->el = el;
        r->native = pkg_compat_of(r->arch, r->abi, my_arch, my_abi, why, sizeof why) == PKG_COMPAT_NATIVE;
        k++;
    }
    c->n = k;
    qsort(c->r, (size_t)c->n, sizeof *c->r, cmp_row);
    return 0;
}

/* The first row of id's group, or -1. */
static int cat_find(const catalogue *c, const char *id)
{
    int lo = 0, hi = c->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, d = strcmp(c->r[mid].id, id);
        if (d < 0) lo = mid + 1;
        else if (d > 0) hi = mid - 1;
        else { while (mid > 0 && !strcmp(c->r[mid - 1].id, id)) mid--; return mid; }
    }
    return -1;
}

static int push(pkg_news *nw, const pkg_news_row *r)
{
    if (nw->n == nw->cap) {
        int cap = nw->cap ? nw->cap * 2 : 16;
        pkg_news_row *v = (pkg_news_row *)realloc(nw->rows, (size_t)cap * sizeof *v);
        if (!v) return -1;
        nw->rows = v; nw->cap = cap;
    }
    nw->rows[nw->n++] = *r;
    switch (r->kind) {
    case PKG_NEWS_APP: nw->apps++; break;
    case PKG_NEWS_BUILD: nw->builds++; break;
    case PKG_NEWS_UPDATED: nw->updated++; break;
    case PKG_NEWS_RECENT: nw->recent++; break;
    }
    return 0;
}

/* The reader hands strings over as they stand in the file, escapes and
   all, and the writer escapes again; a catalogue fetched from elsewhere is
   not held to the official one's plain-ASCII rule. So what goes into a row
   is made plain here: anything that would need an escape becomes '?'. */
static void plain(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == '"' || *s == '\\' || (unsigned char)*s >= 0x7f) *s = '?';
}

static void fill_row(const catalogue *c, int el, pkg_news_kind kind, pkg_news_row *r);

static void fill_row_raw(const catalogue *c, int el, pkg_news_kind kind, pkg_news_row *r)
{
    memset(r, 0, sizeof *r);
    r->kind = kind;
    mstr(c->json, c->t, c->ntok, el, "id", r->id, sizeof r->id);
    mstr(c->json, c->t, c->ntok, el, "version", r->version, sizeof r->version);
    mstr(c->json, c->t, c->ntok, el, "arch", r->arch, sizeof r->arch);
    mstr(c->json, c->t, c->ntok, el, "abi", r->abi, sizeof r->abi);
    mstr(c->json, c->t, c->ntok, el, "summary", r->summary, sizeof r->summary);
    r->revision = js_long(c->json, c->t, js_member(c->json, c->t, c->ntok, el, "revision"), 0);
    dates_from_element(c->json, c->t, c->ntok, el, r->first_added, r->app_first_added);
}

static void fill_row(const catalogue *c, int el, pkg_news_kind kind, pkg_news_row *r)
{
    fill_row_raw(c, el, kind, r);
    plain(r->id); plain(r->version); plain(r->arch); plain(r->abi); plain(r->summary);
}

const char *pkg_news_word(pkg_news_kind k)
{
    switch (k) {
    case PKG_NEWS_APP:     return "new-application";
    case PKG_NEWS_BUILD:   return "new-build";
    case PKG_NEWS_UPDATED: return "updated";
    case PKG_NEWS_RECENT:  return "recently-added";
    }
    return "unknown";
}

void pkg_news_free(pkg_news *nw)
{
    if (!nw) return;
    free(nw->rows);
    nw->rows = NULL; nw->n = nw->cap = 0;
}

/* Newest first, then by id. Rows without a date sort last (never listed by
   recent, but kept stable for the first-catalogue case). */
static const char *row_date(const pkg_news_row *r)
{
    return r->kind == PKG_NEWS_BUILD ? r->first_added : r->app_first_added;
}

static int cmp_newest(const void *a, const void *b)
{
    const pkg_news_row *x = (const pkg_news_row *)a, *y = (const pkg_news_row *)b;
    int c = strcmp(row_date(y), row_date(x));
    return c ? c : strcmp(x->id, y->id);
}

static int cmp_kind_id(const void *a, const void *b)
{
    const pkg_news_row *x = (const pkg_news_row *)a, *y = (const pkg_news_row *)b;
    if (x->kind != y->kind) return (int)x->kind - (int)y->kind;
    return strcmp(x->id, y->id);
}

static int recent_from(const catalogue *c, const pkg_recent_opts *o, pkg_news_kind app_kind, pkg_news *out)
{
    int i = 0, start = out->n;
    while (i < c->n) {
        int j = i;
        while (j < c->n && !strcmp(c->r[j].id, c->r[i].id)) j++;
        if (c->r[i].native) {           /* natives sort first in a group */
            pkg_news_row r;
            fill_row(c, c->r[i].el, app_kind, &r);
            /* An application whose build for here came later than the
               application itself is a new build, by its own date; listed
               only when builds are asked for. */
            if (!r.app_first_added[0]) out->unknown_dates++;
            else if (r.first_added[0] && strcmp(r.first_added, r.app_first_added) > 0) {
                if (o->builds && (!o->since[0] || strcmp(r.first_added, o->since) >= 0)) {
                    r.kind = PKG_NEWS_BUILD;
                    if (push(out, &r)) return -1;
                }
            } else if (!o->since[0] || strcmp(r.app_first_added, o->since) >= 0) {
                if (push(out, &r)) return -1;
            }
        }
        i = j;
    }
    if (out->n > start)
        qsort(out->rows + start, (size_t)(out->n - start), sizeof *out->rows, cmp_newest);
    if (o->limit > 0 && out->n - start > o->limit) {
        int k;
        for (k = start + o->limit; k < out->n; k++)
            if (out->rows[k].kind == PKG_NEWS_BUILD) out->builds--;
            else if (out->rows[k].kind == PKG_NEWS_APP) out->apps--;
            else out->recent--;
        out->n = start + o->limit;
    }
    return 0;
}

int news_recent(const char *json, size_t len, const char *my_arch, const char *my_abi,
                const pkg_recent_opts *o, pkg_news *out)
{
    catalogue c; int rc;
    memset(out, 0, sizeof *out);
    if ((rc = cat_read(&c, json, len, my_arch, my_abi)) != 0) return rc;
    rc = recent_from(&c, o, PKG_NEWS_APP, out);
    cat_free(&c);
    if (rc) { pkg_news_free(out); return JS_ENOMEM; }
    return 0;
}

int news_compare(const char *old, size_t olen, const char *nw, size_t nlen,
                 const char *my_arch, const char *my_abi, const char *prev_day, pkg_news *out)
{
    catalogue a, b; int rc, i = 0;
    memset(out, 0, sizeof *out);
    if ((rc = cat_read(&b, nw, nlen, my_arch, my_abi)) != 0) return rc;
    if (!old || cat_read(&a, old, olen, my_arch, my_abi) != 0) {
        /* Nothing to compare with: every entry would be "new", which would
           say nothing. The catalogue's own recent additions instead. */
        pkg_recent_opts o; memset(&o, 0, sizeof o); o.limit = NEWS_FIRST_LIMIT;
        out->first = 1;
        snprintf(out->first_why, sizeof out->first_why, "%s",
                 old ? "The previous catalogue could not be read, so nothing is called new."
                     : "This is the first catalogue on this machine, so nothing is called new.");
        rc = recent_from(&b, &o, PKG_NEWS_RECENT, out);
        cat_free(&b);
        if (rc) { pkg_news_free(out); return JS_ENOMEM; }
        return 0;
    }
    while (i < b.n) {
        int j = i, k;
        while (j < b.n && !strcmp(b.r[j].id, b.r[i].id)) j++;
        if (b.r[i].native) {
            pkg_news_row r;
            k = cat_find(&a, b.r[i].id);
            if (k < 0) {
                fill_row(&b, b.r[i].el, PKG_NEWS_APP, &r);
                /* Back after a removal: its first publication is older than
                   the catalogue it is compared with. */
                r.returned = r.app_first_added[0] && prev_day && news_date_ok(prev_day)
                             && strcmp(r.app_first_added, prev_day) < 0;
                if (push(out, &r)) goto oom;
            } else if (!a.r[k].native) {
                fill_row(&b, b.r[i].el, PKG_NEWS_BUILD, &r);
                if (push(out, &r)) goto oom;
            } else {
                char ov[64]; long orev;
                fill_row(&b, b.r[i].el, PKG_NEWS_UPDATED, &r);
                mstr(a.json, a.t, a.ntok, a.r[k].el, "version", ov, sizeof ov);
                plain(ov);
                orev = js_long(a.json, a.t, js_member(a.json, a.t, a.ntok, a.r[k].el, "revision"), 0);
                if (strcmp(ov, r.version) || orev != r.revision) {
                    snprintf(r.old_version, sizeof r.old_version, "%s", ov);
                    r.old_revision = orev;
                    if (push(out, &r)) goto oom;
                }
            }
        }
        i = j;
    }
    if (out->n > 1) qsort(out->rows, (size_t)out->n, sizeof *out->rows, cmp_kind_id);
    cat_free(&a); cat_free(&b);
    return 0;
oom:
    cat_free(&a); cat_free(&b); pkg_news_free(out);
    return JS_ENOMEM;
}

/* ------------------------------------------------------------------ JSON */

static void jk(js_out *o, const char *k) { js_vstr(o, k); js_raw(o, ": "); }

char *pkg_news_json(const pkg_news *nw, const char *mode, int limit)
{
    size_t cap = 2048 + (size_t)nw->n * 900;
    char *buf = (char *)malloc(cap);
    js_out o; int i, shown[4] = { 0, 0, 0, 0 }, first = 1;
    if (!buf) return NULL;
    js_out_init(&o, buf, cap);
    js_raw(&o, "{ "); jk(&o, "schema"); js_vlong(&o, 1);
    js_raw(&o, ", "); jk(&o, "mode"); js_vstr(&o, mode);
    js_raw(&o, ",\n  "); jk(&o, "catalogue"); js_vstr(&o, nw->catalogue);
    js_raw(&o, ", "); jk(&o, "refreshed"); js_vstr(&o, nw->when);
    js_raw(&o, ", "); jk(&o, "compared_with"); js_vstr(&o, nw->previous);
    js_raw(&o, ",\n  "); jk(&o, "catalogue_sha256"); js_vstr(&o, nw->catalogue_sha256);
    if (!strcmp(mode, "last-update")) {
        js_raw(&o, ",\n  "); jk(&o, "recorded"); js_raw(&o, nw->recorded ? "true" : "false");
        js_raw(&o, ", "); jk(&o, "current"); js_raw(&o, nw->current ? "true" : "false");
    }
    js_raw(&o, ",\n  "); jk(&o, "first_catalogue"); js_raw(&o, nw->first ? "true" : "false");
    js_raw(&o, ", "); jk(&o, "first_reason"); js_vstr(&o, nw->first_why);
    js_raw(&o, ",\n  "); jk(&o, "counts"); js_raw(&o, "{ ");
    jk(&o, "new_applications"); js_vlong(&o, nw->apps);
    js_raw(&o, ", "); jk(&o, "new_builds"); js_vlong(&o, nw->builds);
    js_raw(&o, ", "); jk(&o, "updated"); js_vlong(&o, nw->updated);
    js_raw(&o, ", "); jk(&o, "recently_added"); js_vlong(&o, nw->recent);
    js_raw(&o, ", "); jk(&o, "unknown_dates"); js_vlong(&o, nw->unknown_dates);
    js_raw(&o, " },\n  "); jk(&o, "items"); js_raw(&o, "[");
    for (i = 0; i < nw->n; i++) {
        const pkg_news_row *r = &nw->rows[i];
        if (limit > 0 && shown[r->kind]++ >= limit) continue;
        js_raw(&o, first ? "\n    { " : ",\n    { ");
        first = 0;
        jk(&o, "id"); js_vstr(&o, r->id);
        js_raw(&o, ", "); jk(&o, "kind"); js_vstr(&o, pkg_news_word(r->kind));
        js_raw(&o, ", "); jk(&o, "returned"); js_raw(&o, r->returned ? "true" : "false");
        js_raw(&o, ", "); jk(&o, "version"); js_vstr(&o, r->version);
        js_raw(&o, ", "); jk(&o, "revision"); js_vlong(&o, r->revision);
        js_raw(&o, ", "); jk(&o, "old_version"); js_vstr(&o, r->old_version);
        js_raw(&o, ", "); jk(&o, "old_revision"); js_vlong(&o, r->old_revision);
        js_raw(&o, ",\n      "); jk(&o, "arch"); js_vstr(&o, r->arch);
        js_raw(&o, ", "); jk(&o, "abi"); js_vstr(&o, r->abi);
        js_raw(&o, ", "); jk(&o, "first_added"); js_vstr(&o, r->first_added);
        js_raw(&o, ", "); jk(&o, "app_first_added"); js_vstr(&o, r->app_first_added);
        js_raw(&o, ", "); jk(&o, "installed"); js_raw(&o, r->installed ? "true" : "false");
        js_raw(&o, ",\n      "); jk(&o, "summary"); js_vstr(&o, r->summary);
        js_raw(&o, " }");
    }
    js_raw(&o, first ? "]\n}\n" : "\n  ]\n}\n");
    if (o.fail) { free(buf); return NULL; }
    return buf;
}

static pkg_news_kind kind_of(const char *w)
{
    return !strcmp(w, "new-build") ? PKG_NEWS_BUILD : !strcmp(w, "updated") ? PKG_NEWS_UPDATED :
           !strcmp(w, "recently-added") ? PKG_NEWS_RECENT : PKG_NEWS_APP;
}

int news_from_json(const char *json, size_t len, pkg_news *out)
{
    js_tok *t; int n, arr, el;
    char w[24];
    memset(out, 0, sizeof *out);
    n = js_parse_alloc(json, len, &t, PKG_JSON_MAXTOK);
    if (n < 0) return n;
    if (t[0].type != JS_OBJ || js_long(json, t, js_member(json, t, n, 0, "schema"), 0) != 1
        || (arr = js_member(json, t, n, 0, "items")) < 0) { free(t); return JS_EINVAL; }
    mstr(json, t, n, 0, "catalogue", out->catalogue, sizeof out->catalogue);
    mstr(json, t, n, 0, "refreshed", out->when, sizeof out->when);
    mstr(json, t, n, 0, "compared_with", out->previous, sizeof out->previous);
    mstr(json, t, n, 0, "catalogue_sha256", out->catalogue_sha256, sizeof out->catalogue_sha256);
    mstr(json, t, n, 0, "first_reason", out->first_why, sizeof out->first_why);
    mstr(json, t, n, 0, "first_catalogue", w, sizeof w);
    out->first = !strcmp(w, "true");
    for (el = js_child(t, n, arr); el >= 0; el = js_sibling(t, n, el)) {
        pkg_news_row r;
        memset(&r, 0, sizeof r);
        mstr(json, t, n, el, "kind", w, sizeof w);
        r.kind = kind_of(w);
        mstr(json, t, n, el, "returned", w, sizeof w);
        r.returned = !strcmp(w, "true");
        mstr(json, t, n, el, "id", r.id, sizeof r.id);
        mstr(json, t, n, el, "version", r.version, sizeof r.version);
        r.revision = js_long(json, t, js_member(json, t, n, el, "revision"), 0);
        mstr(json, t, n, el, "old_version", r.old_version, sizeof r.old_version);
        r.old_revision = js_long(json, t, js_member(json, t, n, el, "old_revision"), 0);
        mstr(json, t, n, el, "arch", r.arch, sizeof r.arch);
        mstr(json, t, n, el, "abi", r.abi, sizeof r.abi);
        mstr(json, t, n, el, "summary", r.summary, sizeof r.summary);
        dates_from_element(json, t, n, el, r.first_added, r.app_first_added);
        if (r.id[0] && push(out, &r)) { free(t); pkg_news_free(out); return JS_ENOMEM; }
    }
    free(t);
    return 0;
}
