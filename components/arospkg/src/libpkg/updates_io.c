/* The AROS half of pkg_updates: read the registry and the catalogue, date
 * the catalogue, and hand both to updates_compute(). Read-only: no lock, no
 * recovery, nothing written, so the ScreenBar widget can call it while apkg
 * or PkgManager is working. A registry entry being rewritten at that moment
 * is read as it is on disk; the next scan sees the settled one. */
#include "internal.h"
#include "updates.h"
#include "json.h"
#include "listing.h"
#include "util.h"

#include <proto/dos.h>
#include <dos/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The catalogue file's date, from the file system: the moment apkg update
   last published a catalogue that parsed. AROS dates count from 1978-01-01
   in the machine's local time; the 2922 days to 1970 make it comparable
   with time(), which on AROS is local time too. */
static void catalogue_date(const char *path, pkg_updates *u)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    struct FileInfoBlock *fib;
    if (!l) return;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(l, fib)) {
        const struct DateStamp *d = &fib->fib_Date;
        long secs = (d->ds_Days + 2922L) * 86400L + d->ds_Minute * 60L + d->ds_Tick / TICKS_PER_SECOND;
        time_t t = (time_t)secs;
        struct tm *g = gmtime(&t);    /* already local: no further shift */
        u->catalogue_time = secs;
        if (g) strftime(u->catalogue_when, sizeof u->catalogue_when, "%Y-%m-%d %H:%M", g);
        {
            long now = (long)time(NULL);
            u->catalogue_stale = now > secs && now - secs > PKG_CATALOGUE_STALE_DAYS * 86400L;
        }
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    UnLock(l);
}

static void member(const char *js, const js_tok *t, int n, const char *name, char *out, size_t len)
{
    out[0] = 0;
    if (js_str(js, t, js_member(js, t, n, 0, name), out, len) != 0) out[0] = 0;
}

pkg_status pkg_updates_scan(const char *root, const char *index_path, int probe,
                            pkg_updates *out, pkg_err *e)
{
    char dir[PKG_MAXPATH], **names = NULL, *idx;
    upd_installed *inst = NULL;
    int nnames = 0, ninst = 0, i, rc;
    size_t len = 0;

    memset(out, 0, sizeof *out);
    snprintf(dir, sizeof dir, "%s/db/installed", root);
    /* No registry directory is a root nothing was ever installed into. */
    if (u_listdir(dir, &names, &nnames) == 0 && nnames > 0) {
        pkg_sort_names(names, nnames);
        inst = (upd_installed *)calloc((size_t)nnames, sizeof *inst);
        if (!inst) { u_freelist(names, nnames); return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", ""); }
        for (i = 0; i < nnames; i++) {
            char p[PKG_MAXPATH], *txt; size_t l = strlen(names[i]); js_tok *t; int n;
            /* <id>.json only: a .json.new is a registry write in progress. */
            if (l < 6 || strcmp(names[i] + l - 5, ".json") != 0) continue;
            snprintf(p, sizeof p, "%s/%s", dir, names[i]);
            if (!(txt = u_read_all(p, &len))) continue;
            n = js_parse_alloc(txt, len, &t, PKG_JSON_MAXTOK);
            if (n > 0 && t[0].type == JS_OBJ) {
                upd_installed *r = &inst[ninst];
                member(txt, t, n, "name", r->id, sizeof r->id);
                member(txt, t, n, "version", r->version, sizeof r->version);
                member(txt, t, n, "arch", r->arch, sizeof r->arch);
                member(txt, t, n, "abi", r->abi, sizeof r->abi);
                r->revision = js_long(txt, t, js_member(txt, t, n, 0, "revision"), 0);
                if (r->id[0]) ninst++;
            }
            if (n > 0) free(t);
            free(txt);
        }
        u_freelist(names, nnames);
    } else if (names) u_freelist(names, nnames);

    idx = u_read_all(index_path, &len);
    if (idx) catalogue_date(index_path, out);
    rc = updates_compute(idx, len, inst, ninst, pkg_arch(), pkg_abi(), pkg_abi_override(), probe, out);
    free(idx); free(inst);
    if (rc != 0) { pkg_updates_free(out); return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", ""); }
    return PKG_OK;
}

pkg_status pkg_updates_get(pkg_ctx *c, const char *index_path, int probe,
                           pkg_updates *out, pkg_err *e)
{
    pkg_status st = pkg_updates_scan(c->root, index_path, probe, out, e);
    if (st == PKG_OK) out->pending = c->pending;
    return st;
}
