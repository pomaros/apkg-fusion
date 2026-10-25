/* verify, and the dry run of each operation.
 *
 * Both are read-only in every sense that matters: nothing in the drawer or
 * the registry changes, and no transaction is recovered on the way -- the
 * caller opens the root PKG_OPEN_READ, which never recovers, and the report
 * repeats what pkg_pending_txns says. The one thing a dry run may write is
 * a staging tree under tmp/, which it removes again, and -- only when asked
 * -- an archive into the cache, which is not the installation.
 */
#include "limits.h"
#include "internal.h"
#include "app.h"
#include "verify.h"
#include "util.h"
#include "sha256.h"
#include "arc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *p; size_t len, cap; } rep;
static void rep_add(rep *r, const char *fmt, ...)
{
    va_list ap; int n;
    if (!r->p) { r->cap = 8192; r->p = (char *)malloc(r->cap); if (!r->p) return; r->p[0] = 0; }
    for (;;) {
        va_start(ap, fmt); n = vsnprintf(r->p + r->len, r->cap - r->len, fmt, ap); va_end(ap);
        if (n < 0) return;
        if ((size_t)n < r->cap - r->len) { r->len += (size_t)n; return; }
        { char *q = (char *)realloc(r->p, r->cap * 2); if (!q) return; r->p = q; r->cap *= 2; }
    }
}

/* Registry inventory and icon record of an installed package, or NULL. */
static char *load_installed(pkg_ctx *c, const char *id, rev_info *r, inv_ent *inv, int *ninv)
{
    char regpath[PKG_MAXPATH], *txt = NULL;
    registry_path(c, id, regpath, sizeof regpath);
    if (!u_exists(regpath)) return NULL;
    if (swap_read_registry(regpath, r, inv, ninv, &txt) != 0) return NULL;
    return txt;
}

static void icon_verdict(pkg_ctx *c, const char *id, const rev_info *r, char *out, size_t n)
{
    char icon_dst[PKG_MAXPATH], sha[65] = "";
    snprintf(icon_dst, sizeof icon_dst, "%s/%s.info", c->root, id);
    if (strcmp(r->icon_state, "installed") != 0) { snprintf(out, n, "%s", r->icon_state[0] ? r->icon_state : "none"); return; }
    if (!u_exists(icon_dst)) { snprintf(out, n, "missing"); return; }
    if (sha256_file(icon_dst, sha) == 0 && strcmp(sha, r->icon_sha) == 0) snprintf(out, n, "ours");
    else snprintf(out, n, "changed locally");
}

pkg_status pkg_verify(pkg_ctx *c, const char *id, pkg_verify_result *out, char **listing, pkg_err *e)
{
    rev_info r; inv_ent *reg, *live; int nreg = 0, nlive = 0, n, i;
    vf_item *items; char dest[PKG_MAXPATH], *txt; rep rp = {0};

    memset(out, 0, sizeof *out);
    if (listing) *listing = NULL;
    reg  = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    live = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    items = (vf_item *)malloc(sizeof(vf_item) * MAXINV * 2);
    if (!reg || !live || !items) { free(reg); free(live); free(items); return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id); }
    txt = load_installed(c, id, &r, reg, &nreg);
    if (!txt) { free(reg); free(live); free(items); return pkg_fail(e, PKG_E_NOT_FOUND, "not installed", "", id); }
    free(txt);
    snprintf(dest, sizeof dest, "%s/%s", c->root, id);
    if (u_exists(dest) && inv_of_tree(dest, "", live, &nlive, MAXINV) != 0) {
        free(reg); free(live); free(items);
        return pkg_fail(e, PKG_E_IO, "cannot read the drawer", "", dest);
    }
    n = verify_classify(reg, nreg, live, nlive, items, MAXINV * 2, &out->ok, &out->changed, &out->missing, &out->unknown);
    if (n < 0) { free(reg); free(live); free(items); return pkg_fail(e, PKG_E_NOMEM, "too many files to list", "", id); }
    icon_verdict(c, id, &r, out->icon, sizeof out->icon);
    if (listing) {
        for (i = 0; i < n; i++) rep_add(&rp, "%-18s %s\n", verify_statename(items[i].state), items[i].rel);
        *listing = rp.p ? rp.p : strdup("");
    }
    free(reg); free(live); free(items);
    return PKG_OK;
}

static void preface(pkg_ctx *c, rep *r, const char *op, const char *id)
{
    rep_add(r, "dry run: %s %s -- nothing will be changed\n", op, id);
    if (pkg_pending_txns(c))
        rep_add(r, "note: %d uncommitted transaction(s) exist and are NOT recovered by a dry run;\n"
                   "      run apkg doctor --retry first, and read this plan as provisional\n",
                pkg_pending_txns(c));
}

static pkg_status dry_install(pkg_ctx *c, const char *index_path, const char *id, int allow_fetch, rep *r, pkg_err *e)
{
    rev_info to; req_ent reqs[MAXREQ]; int nreq = 0, ninv = 0, i;
    char regpath[PKG_MAXPATH], dest[PKG_MAXPATH], ext[8], archive[PKG_MAXPATH], got[65], why[200], lbl[80];
    char sroot[PKG_MAXPATH], staging[PKG_MAXPATH], tid[64];
    inv_ent *inv; long total = 0; pkg_status st;

    registry_path(c, id, regpath, sizeof regpath);
    if (u_exists(regpath)) { rep_add(r, "already installed; an install would be refused\n"); return PKG_OK; }
    snprintf(dest, sizeof dest, "%s/%s", c->root, id);
    st = swap_read_index(index_path, id, pkg_arch(),
                         pkg_abi_override() && *pkg_abi_override() ? pkg_abi_override() : pkg_abi(),
                         &to, reqs, &nreq, e);
    if (st != PKG_OK) return st;
    rev_label(lbl, sizeof lbl, to.version, to.revision);
    rep_add(r, "would install %s %s from %s (%ld bytes)\n", id, lbl, to.url, to.size);
    st = target_check(id, to.arch, to.abi, pkg_abi_override(), e);
    if (st != PKG_OK) { rep_add(r, "would be refused: %s -- %s\n", e->summary, e->detail); return PKG_OK; }
    req_probe_all(reqs, nreq);
    for (i = 0; i < nreq; i++)
        rep_add(r, "  requires %s: %s (%s)\n", reqs[i].id, req_statename(reqs[i].state), reqs[i].why);
    for (i = 0; i < nreq; i++)
        if (reqs[i].state == REQ_MISSING) { rep_add(r, "would be refused: this system does not provide %s\n", reqs[i].id); return PKG_OK; }
    if (u_exists(dest)) {
        rep_add(r, "would be refused: %s already exists and is not the package's\n", dest);
        return PKG_OK;
    }
    swap_archive_ext(to.url, ext, sizeof ext);
    snprintf(archive, sizeof archive, "%s/%s.%s", c->cache, id, ext);
    if (u_exists(archive) && sha256_file(archive, got) == 0 && strcmp(got, to.sha) == 0)
        rep_add(r, "archive: cached and verified (%s)\n", archive);
    else if (allow_fetch) {
        rep_add(r, "archive: not cached; fetching into the cache (allowed for this run)\n");
        st = fetch_verified(c, id, to.url, to.sha, to.size, archive, e);
        if (st != PKG_OK) return st;
    } else {
        rep_add(r, "archive: not cached, and this run may not download it\n"
                   "plan incomplete: the file list needs the archive; allow fetching (into the cache only) to see it\n");
        return PKG_OK;
    }
    /* The file plan, from a staging tree that goes straight back. */
    txn_id(tid, id);
    snprintf(sroot, sizeof sroot, "%s/dry-%s", c->tmp, tid);
    snprintf(staging, sizeof staging, "%s/%s", sroot, id);
    if (u_mkdir(sroot) != 0 || u_mkdir(staging) != 0) return pkg_fail(e, PKG_E_IO, "cannot create staging", "", staging);
    if (arc_extract(archive, staging, to.subdir, PKG_MAX_MEMBER, PKG_MAX_TOTAL, NULL, NULL, why) != 0) {
        u_deltree(sroot);
        rep_add(r, "would be refused: the archive was rejected -- %s\n", why); return PKG_OK;
    }
    /* The plan counts the description the install would write. */
    if (app_stage(staging, id, to.version, to.revision, to.arch, to.abi, to.app_reqs, to.app_nreq, &to.launch) != 0) {
        u_deltree(sroot); return pkg_fail(e, PKG_E_IO, "cannot write the application description", "", staging);
    }
    inv = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    if (!inv || inv_of_tree(staging, "", inv, &ninv, MAXINV) != 0) { free(inv); u_deltree(sroot); return pkg_fail(e, PKG_E_IO, "cannot inventory the staged tree", "", staging); }
    for (i = 0; i < ninv; i++) total += inv[i].size > 0 ? inv[i].size : 0;
    rep_add(r, "would add %d file(s), %ld bytes, into %s\n", ninv, total, dest);
    for (i = 0; i < ninv && i < 40; i++) rep_add(r, "  add        %s\n", inv[i].rel);
    if (ninv > 40) rep_add(r, "  ... %d more\n", ninv - 40);
    if (to.icon_member[0]) {
        char icon_dst[PKG_MAXPATH];
        snprintf(icon_dst, sizeof icon_dst, "%s/%s.info", c->root, id);
        rep_add(r, u_exists(icon_dst) ? "drawer icon: %s already exists and would be left as it is\n"
                                      : "drawer icon: would put %s in place\n", icon_dst);
    }
    free(inv); u_deltree(sroot);
    return PKG_OK;
}

static pkg_status dry_remove(pkg_ctx *c, const char *id, rep *r, pkg_err *e)
{
    pkg_verify_result v; char *listing = NULL; pkg_status st; char verdict[24];
    rev_info rr; inv_ent *inv; int ninv = 0; char *txt;
    st = pkg_verify(c, id, &v, &listing, e);
    if (st != PKG_OK) return st;
    inv = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    txt = inv ? load_installed(c, id, &rr, inv, &ninv) : NULL;
    if (txt) { char lbl[80]; rev_label(lbl, sizeof lbl, rr.version, rr.revision); rep_add(r, "installed: %s %s\n", id, lbl); free(txt); }
    free(inv);
    rep_add(r, "would remove %d file(s) that are as installed\n", v.ok);
    rep_add(r, "would keep %d file(s) changed locally, and report them\n", v.changed);
    rep_add(r, "would leave %d file(s) that are not the package's\n", v.unknown);
    if (v.missing) rep_add(r, "%d file(s) already missing\n", v.missing);
    icon_verdict(c, id, &rr, verdict, sizeof verdict);
    if (!strcmp(v.icon, "ours")) rep_add(r, "drawer icon: would delete (ours, unchanged)\n");
    else if (!strcmp(v.icon, "changed locally")) rep_add(r, "drawer icon: changed locally, would keep\n");
    else if (!strcmp(v.icon, "kept-existing")) rep_add(r, "drawer icon: was there before the install, would not touch\n");
    if (listing) { rep_add(r, "%s", listing); free(listing); }
    (void)verdict;
    return PKG_OK;
}

static void plan_lines(rep *r, const char *op, const char *id, const pkg_plan *p)
{
    char a[80], b[80];
    rev_label(a, sizeof a, p->from_version, p->from_revision);
    rev_label(b, sizeof b, p->to_version, p->to_revision);
    if (!p->allowed) { rep_add(r, "%s %s: not possible -- %s\n", op, id, p->why); return; }
    rep_add(r, "would %s %s: %s -> %s\n", op, id, a, b);
    if (p->incomplete) { rep_add(r, "plan incomplete: %s\n", p->why); return; }
    rep_add(r, "  %d add, %d replace, %d remove, %d kept as changed locally or not the package's, %d unchanged\n",
            p->add, p->replace, p->remove, p->kept, p->unchanged);
    if (p->conflicts) rep_add(r, "would be refused: %s\n", p->why);
}

pkg_status pkg_dry_run(pkg_ctx *c, const char *index_path, const char *op, const char *id,
                       int allow_fetch, char **report, pkg_err *e)
{
    rep r = {0}; pkg_status st = PKG_OK; pkg_plan p;
    *report = NULL;
    preface(c, &r, op, id);
    if (!strcmp(op, "install"))       st = dry_install(c, index_path, id, allow_fetch, &r, e);
    else if (!strcmp(op, "remove"))   st = dry_remove(c, id, &r, e);
    else if (!strcmp(op, "upgrade"))  { st = pkg_upgrade_preview(c, index_path, id, allow_fetch, &p, e); if (st == PKG_OK) plan_lines(&r, "upgrade", id, &p); }
    else if (!strcmp(op, "rollback")) { st = pkg_rollback_preview(c, id, &p, e); if (st == PKG_OK) plan_lines(&r, "roll back", id, &p); }
    else st = pkg_fail(e, PKG_E_NOT_FOUND, "no such operation to dry-run", "install, remove, upgrade, rollback", op);
    if (st != PKG_OK) { free(r.p); return st; }
    rep_add(&r, "dry run: nothing was changed\n");
    *report = r.p ? r.p : strdup("");
    return PKG_OK;
}
