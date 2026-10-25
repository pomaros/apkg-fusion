/* The AROS half of pkg_news: the record of what the last successful
 * catalogue refresh brought, and the readers behind `apkg recent`.
 *
 * <root>/db/news.json is written by pkg_update after the new catalogue is in
 * place, never before: a refresh that fails or is cancelled leaves the old
 * catalogue and the old record as they were. The record carries the SHA-256
 * of the catalogue it describes, so a catalogue replaced by other means
 * (an older apkg, a copy by hand) is not described by an old record. */
#include "internal.h"
#include "news.h"
#include "sha256.h"
#include "util.h"

#include <proto/dos.h>
#include <dos/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* AROS dates count from 1978-01-01 in the machine's local time, as time()
   does on AROS (see updates_io.c catalogue_date). */
static int file_when(const char *path, char when[24], char day[11])
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    struct FileInfoBlock *fib;
    int ok = 0;
    when[0] = day[0] = 0;
    if (!l) return 0;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(l, fib)) {
        const struct DateStamp *d = &fib->fib_Date;
        time_t t = (time_t)((d->ds_Days + 2922L) * 86400L + d->ds_Minute * 60L + d->ds_Tick / TICKS_PER_SECOND);
        struct tm *g = gmtime(&t);
        if (g) {
            strftime(when, 24, "%Y-%m-%d %H:%M", g);
            strftime(day, 11, "%Y-%m-%d", g);
            ok = 1;
        }
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    UnLock(l);
    return ok;
}

void pkg_days_ago(int days, char out[11])
{
    time_t t = time(NULL) - (time_t)days * 86400;
    struct tm *g = gmtime(&t);
    out[0] = 0;
    if (g) strftime(out, 11, "%Y-%m-%d", g);
}

static const char *my_abi(void)
{
    const char *o = pkg_abi_override();
    return o && *o ? o : pkg_abi();
}

static void mark_installed(struct pkg_ctx *c, pkg_news *nw)
{
    int i;
    for (i = 0; i < nw->n; i++) {
        char p[PKG_MAXPATH];
        registry_path(c, nw->rows[i].id, p, sizeof p);
        nw->rows[i].installed = u_exists(p);
    }
}

static void news_path(struct pkg_ctx *c, char *out, size_t n) { snprintf(out, n, "%s/db/news.json", c->root); }

/* The catalogue address recorded by the last refresh, "" when none; and,
   if wanted, when that refresh was. */
static void recorded(struct pkg_ctx *c, char *url, size_t n, char when[24])
{
    char p[PKG_MAXPATH], *txt; size_t len; pkg_news nw;
    url[0] = 0;
    if (when) when[0] = 0;
    news_path(c, p, sizeof p);
    if (!(txt = u_read_all(p, &len))) return;
    if (news_from_json(txt, len, &nw) == 0) {
        snprintf(url, n, "%s", nw.catalogue);
        if (when) snprintf(when, 24, "%s", nw.when);
    }
    pkg_news_free(&nw);
    free(txt);
}

static void recorded_url(struct pkg_ctx *c, char *out, size_t n) { recorded(c, out, n, NULL); }

void news_before_update(struct pkg_ctx *c, news_prev *prev)
{
    char p[PKG_MAXPATH], when[24];
    memset(prev, 0, sizeof *prev);
    snprintf(p, sizeof p, "%s/db/index.json", c->root);
    prev->text = u_read_all(p, &prev->len);
    recorded(c, prev->url, sizeof prev->url, when);
    /* When the catalogue was fetched: the record of that refresh says it;
       the file's date is the fallback, for a catalogue fetched by an apkg
       before 0.6 (a copy by hand would change it). */
    if (prev->text && when[0] && prev->url[0]) {
        snprintf(prev->when, sizeof prev->when, "%s", when);
        snprintf(prev->day, sizeof prev->day, "%.10s", when);
    } else if (prev->text) file_when(p, prev->when, prev->day);
}

/* The address as the record keeps it: a longer one is stored cut, and must
   still be recognised as the same. */
static int same_url(const char *recorded, const char *url)
{
    size_t n = strlen(recorded);
    const char *a = strrchr(recorded, '/'), *b = strrchr(url, '/');
    if (n >= 511) return strncmp(recorded, url, n) == 0;
    if (!strcmp(recorded, url)) return 1;
    /* index-v2.json and index-v3.json of one place are one catalogue in
       two generations: moving from one to the other (apkg 0.6 does) is not
       another catalogue, and is compared as usual. */
    return a && b && a - recorded == b - url && !strncmp(recorded, url, (size_t)(a - recorded))
           && !strncmp(a, "/index-v", 8) && !strncmp(b, "/index-v", 8);
}

int news_after_update(struct pkg_ctx *c, const char *url, news_prev *prev)
{
    char p[PKG_MAXPATH], day[11];
    char *nwtext, *js; size_t nlen;
    pkg_news nw; int rc;

    snprintf(p, sizeof p, "%s/db/index.json", c->root);
    if (!(nwtext = u_read_all(p, &nlen))) return -1;
    if (prev->text && prev->url[0] && !same_url(prev->url, url)) {
        /* Another catalogue altogether: comparing with it would announce
           everything the two do not share. */
        free(prev->text); prev->text = NULL;
        rc = news_compare(NULL, 0, nwtext, nlen, pkg_arch(), my_abi(), NULL, &nw);
        if (rc == 0)
            snprintf(nw.first_why, sizeof nw.first_why,
                     "The catalogue now comes from another address, so nothing is called new.");
    } else rc = news_compare(prev->text, prev->len, nwtext, nlen, pkg_arch(), my_abi(), prev->day, &nw);
    if (rc != 0) { free(nwtext); return -1; }
    {
        sha256_ctx h; sha256_init(&h); sha256_update(&h, nwtext, nlen); sha256_final(&h, nw.catalogue_sha256);
    }
    free(nwtext);
    snprintf(nw.catalogue, sizeof nw.catalogue, "%s", url);
    if (!nw.first) snprintf(nw.previous, sizeof nw.previous, "%s", prev->when);
    file_when(p, nw.when, day);
    js = pkg_news_json(&nw, "last-update", 0);
    pkg_news_free(&nw);
    if (!js) return -1;
    news_path(c, p, sizeof p);
    rc = u_write_atomicish(p, js);
    free(js);
    return rc;
}

void news_prev_free(news_prev *prev) { free(prev->text); prev->text = NULL; }

static int current_sha(const char *index_path, char out[65])
{
    return sha256_file(index_path, out);
}

pkg_status pkg_news_last(pkg_ctx *c, const char *index_path, pkg_news *out, pkg_err *e)
{
    char p[PKG_MAXPATH], sha[65], *txt; size_t len;
    memset(out, 0, sizeof *out);
    news_path(c, p, sizeof p);
    if (!(txt = u_read_all(p, &len))) return PKG_OK;      /* nothing recorded */
    if (news_from_json(txt, len, out) != 0) {
        free(txt);
        memset(out, 0, sizeof *out);
        return pkg_fail(e, PKG_E_IO, "the record of the last update cannot be read",
                        "apkg update writes it again.", p);
    }
    free(txt);
    out->recorded = 1;
    out->current = current_sha(index_path, sha) == 0 && !strcmp(sha, out->catalogue_sha256);
    mark_installed(c, out);
    return PKG_OK;
}

pkg_status pkg_recent(pkg_ctx *c, const char *index_path, const pkg_recent_opts *o,
                      pkg_news *out, pkg_err *e)
{
    char *idx; size_t len; int rc;
    memset(out, 0, sizeof *out);
    if (!(idx = u_read_all(index_path, &len)))
        return pkg_fail(e, PKG_E_NOT_FOUND, "no catalogue -- run apkg update first", "", index_path);
    rc = news_recent(idx, len, pkg_arch(), my_abi(), o, out);
    free(idx);
    if (rc != 0) return index_unreadable(e, rc, index_path);
    {
        char day[11];
        file_when(index_path, out->when, day);
    }
    recorded_url(c, out->catalogue, sizeof out->catalogue);
    mark_installed(c, out);
    return PKG_OK;
}

void details_catalogue(struct pkg_ctx *c, pkg_details *d)
{
    recorded_url(c, d->catalogue, sizeof d->catalogue);
}
