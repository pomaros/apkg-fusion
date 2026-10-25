/* Upgrade and rollback: one transaction, two directions.
 *
 * Both replace the package's drawer by a tree built in staging from the
 * target revision's archive plus every file the user changed or added, then
 * swap the trees with two renames. The plan (upgrade.h) is computed from the
 * three inventories and printed before anything moves; a single conflict
 * refuses the whole operation with nothing touched. This is deliberately not
 * remove-then-install: that pair loses the user's files at the removal and
 * refuses at the install when it finds them kept.
 *
 * Journal states, in order, each one recoverable from the plan and the disk:
 *   1. staging built, drawer untouched            -> undo: delete staging
 *   2. drawer renamed aside (tmp/<tid>/old)       -> undo: rename it back
 *   3. staging renamed into place                 -> undo: remove the
 *      published tree by the plan's hashes, then rename the old tree back
 *   4. icon, previous-revision bookkeeping, registry written
 *   5. 005-commit                                 -> forward: delete old tree
 *
 * What is saved for a later rollback lives OUTSIDE the transaction so it
 * survives its cleanup: db/previous/<id>.json and cache/previous/<id>.<ext>.
 */
#include "limits.h"
#include "internal.h"
#include "app.h"
#include "upgrade.h"
#include "util.h"
#include "json.h"
#include "sha256.h"
#include "arc.h"
#include "icon.h"
#include "listing.h"
#include "entries.h"
#include "verify.h"
#include "release.h"

#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STOP(want) do { if (stop == (want)) \
    return pkg_fail(e, PKG_E_INTERRUPTED, "stopped on purpose", #want, id); } while (0)


static void member(const char *js, js_tok *t, int n, int obj, const char *k, char *out, size_t sz)
{
    int m = js_member(js, t, n, obj, k);
    out[0] = 0;
    if (m >= 0) js_str(js, t, m, out, sz);
}

/* The installed revision, from a registry entry. */
int swap_read_registry(const char *path, rev_info *r, inv_ent *inv, int *ninv, char **text_out)
{
    size_t len; char *js = u_read_all(path, &len);
    js_tok *t; int n, ic, pv;
    if (!js) return -1;
    n = js_parse_alloc(js, len, &t, PKG_JSON_MAXTOK);
    if (n <= 0) { free(t); free(js); return -1; }
    memset(r, 0, sizeof *r);
    member(js, t, n, 0, "version", r->version, sizeof r->version);
    r->revision = js_long(js, t, js_member(js, t, n, 0, "revision"), 0);
    member(js, t, n, 0, "arch",    r->arch, sizeof r->arch);
    member(js, t, n, 0, "abi",     r->abi,  sizeof r->abi);
    member(js, t, n, 0, "subdir",  r->subdir, sizeof r->subdir);
    member(js, t, n, 0, "origin",  r->url,  sizeof r->url);
    member(js, t, n, 0, "archive_sha256", r->sha, sizeof r->sha);
    r->size = js_long(js, t, js_member(js, t, n, 0, "archive_size"), -1);
    /* A rollback writes the previous revision's record again: its notes too. */
    if (notes_from_element(js, t, n, 0, r->notes, sizeof r->notes) < 0) r->notes[0] = 0;
    member(js, t, n, 0, "reason", r->reason, sizeof r->reason);
    {
        /* Recorded since 0.6; an older entry's relations are unknown, not
           none, and stay so when it is written again. */
        char why[200];
        r->rel_known = js_member(js, t, n, 0, "relations_schema") >= 0
                       && rel_fragment(js, t, n, 0, r->rel_json, sizeof r->rel_json, why, sizeof why) == 0;
        if (!r->rel_known) r->rel_json[0] = 0;
    }
    {
        char why[200];
        if (launch_from_object(js, t, n, js_member(js, t, n, 0, "launch"), &r->launch, why, sizeof why) != 1)
            memset(&r->launch, 0, sizeof r->launch);
        /* Only the declarations, for the drawer's description; the verdicts
           in the registry belong to the install that made them. */
        if (req_parse(js, t, n, 0, r->app_reqs, MAXREQ, &r->app_nreq, why, sizeof why) != 0) r->app_nreq = 0;
    }
    ic = js_member(js, t, n, 0, "icon");
    if (ic >= 0) {
        member(js, t, n, ic, "state",  r->icon_state, sizeof r->icon_state);
        member(js, t, n, ic, "sha256", r->icon_sha,   sizeof r->icon_sha);
        member(js, t, n, ic, "member", r->icon_member, sizeof r->icon_member);
    }
    pv = js_member(js, t, n, 0, "previous");
    if (pv >= 0) {
        member(js, t, n, pv, "registry", r->prev_registry, sizeof r->prev_registry);
        member(js, t, n, pv, "archive",  r->prev_archive,  sizeof r->prev_archive);
    }
    free(t);
    if (inv_from_registry(js, len, inv, MAXINV, ninv) != 0) { free(js); return -1; }
    if (text_out) *text_out = js; else free(js);
    return 0;
}

/* The index's revision of `id`. */
pkg_status swap_read_index(const char *index_path, const char *id,
                           const char *want_arch, const char *want_abi, rev_info *r,
                           req_ent *reqs, int *nreq, pkg_err *e)
{
    size_t len; char *js = u_read_all(index_path, &len);
    js_tok *t; int n, arr, found = -1; char why[200];
    if (!js) return pkg_fail(e, PKG_E_NOT_FOUND, "cannot read the index", "Run apkg update to fetch one.", index_path);
    n = js_parse_alloc(js, len, &t, PKG_JSON_MAXTOK);
    if (n < 0) { free(js); return index_unreadable(e, n, index_path); }
    arr = js_member(js, t, n, 0, "packages");
    {
        int how, same;
        found = index_select_variant(js, t, n, arr, id, want_arch, want_abi, &how, &same);
        if (found >= 0 && how == VARIANT_SEVERAL) {
            snprintf(why, sizeof why, "The index lists %d entries of it for %s/%s. Refusing rather "
                     "than picking one.", same, want_arch, want_abi);
            free(t); free(js);
            return pkg_fail(e, PKG_E_CONFLICT, "more than one build of this package for this system", why, id);
        }
    }
    if (found < 0) { free(t); free(js); return pkg_fail(e, PKG_E_NOT_FOUND, "no such package in the index", "", id); }
    memset(r, 0, sizeof *r);
    member(js, t, n, found, "version", r->version, sizeof r->version);
    r->revision = js_long(js, t, js_member(js, t, n, found, "revision"), 0);
    member(js, t, n, found, "arch",   r->arch, sizeof r->arch);
    member(js, t, n, found, "abi",    r->abi,  sizeof r->abi);
    member(js, t, n, found, "url",    r->url,  sizeof r->url);
    member(js, t, n, found, "sha256", r->sha,  sizeof r->sha);
    r->size = js_long(js, t, js_member(js, t, n, found, "size"), -1);
    member(js, t, n, found, "subdir", r->subdir, sizeof r->subdir);
    member(js, t, n, found, "icon",   r->icon_member, sizeof r->icon_member);
    if (notes_from_element(js, t, n, found, r->notes, sizeof r->notes) < 0) r->notes[0] = 0;
    if (launch_from_object(js, t, n, js_member(js, t, n, found, "launch"), &r->launch, why, sizeof why) != 1)
        memset(&r->launch, 0, sizeof r->launch);
    if (req_parse(js, t, n, found, r->app_reqs, MAXREQ, &r->app_nreq, why, sizeof why) != 0) r->app_nreq = 0;
    if (rel_fragment(js, t, n, found, r->rel_json, sizeof r->rel_json, why, sizeof why) != 0) {
        free(t); free(js);
        return pkg_fail(e, PKG_E_REQUIRES, "this package's relations are not readable by this apkg", why, id);
    }
    r->rel_known = 1;
    if (req_parse(js, t, n, found, reqs, MAXREQ, nreq, why, sizeof why) != 0) {
        free(t); free(js);
        return pkg_fail(e, PKG_E_REQUIRES, "this package's system requirements are not readable", why, id);
    }
    free(t); free(js);
    return PKG_OK;
}

/* cache/<id>.<ext>, the extension taken from the URL as pkg_install does. */
void swap_archive_ext(const char *url, char *ext, size_t n)
{
    const char *dot = strrchr(url, '.');
    size_t k = 0;
    snprintf(ext, n, "bin");
    if (!dot || strlen(dot + 1) >= n) return;
    for (k = 0; dot[1 + k]; k++) {
        char ch = dot[1 + k];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) return;
        ext[k] = ch;
    }
    ext[k] = 0;
}

static int copy_into(const char *from_root, const char *to_root, const char *rel)
{
    char src[PKG_MAXPATH], dst[PKG_MAXPATH], parent[PKG_MAXPATH], *slash;
    u_join(src, sizeof src, from_root, rel);
    u_join(dst, sizeof dst, to_root, rel);
    snprintf(parent, sizeof parent, "%s", dst);
    slash = strrchr(parent, '/');
    if (slash) { *slash = 0; u_mkpath(parent); }
    if (u_exists(dst)) u_delete(dst);
    return u_copy(src, dst);
}

static void print_plan(const up_step *steps, int n, const char *dest)
{
    int i, a = 0, r = 0, d = 0, k = 0, u = 0;
    for (i = 0; i < n; i++) switch (steps[i].act) {
        case UP_ADD: a++; break; case UP_REPLACE: r++; break; case UP_REMOVE: d++; break;
        case UP_KEEP_USER: case UP_KEEP_UNKNOWN: k++; break; case UP_UNCHANGED: u++; break;
        default: break; }
    printf("plan for %s: %d add, %d replace, %d remove, %d kept, %d unchanged\n", dest, a, r, d, k, u);
    for (i = 0; i < n; i++)
        if (steps[i].act != UP_UNCHANGED)
            printf("  %-10s %s\n", upgrade_actname(steps[i].act), steps[i].rel);
}

/* The transaction. `to` describes the target revision and `to_archive` is
 * its verified archive; `from` is the installed one. On an upgrade the
 * installed revision is saved for a later rollback; on a rollback the saved
 * revision is consumed. */
static pkg_status swap(pkg_ctx *c, const char *id, int is_rollback,
                       const rev_info *from, const char *from_text,
                       const rev_info *to, const char *to_archive,
                       inv_ent *old, int nold, req_ent *reqs, int nreq,
                       pkg_stop stop, pkg_plan *preview, int keep_icons, pkg_err *e)
{
    char tid[64], sroot[PKG_MAXPATH], staging[PKG_MAXPATH], oldtree[PKG_MAXPATH];
    char dest[PKG_MAXPATH], regpath[PKG_MAXPATH], regbefore[PKG_MAXPATH];
    char icon_dst[PKG_MAXPATH], icon_stage[PKG_MAXPATH] = "", icon_before[PKG_MAXPATH] = "";
    char icon_sha[65] = "", why[200], ext[8];
    char prev_reg[PKG_MAXPATH] = "", prev_arc[PKG_MAXPATH] = "", cur_arc[PKG_MAXPATH];
    char arc_before[PKG_MAXPATH] = "", preg_before[PKG_MAXPATH] = "";
    inv_ent *newinv = NULL, *live = NULL, *result = NULL;
    up_step *steps = NULL;
    int nnew = 0, nlive = 0, nresult = 0, nsteps = 0, conflicts = 0, i;
    int icon_replace = 0, icon_managed_after = 0;
    long icon_size = -1;
    char *plan = NULL, *reg = NULL;
    pkg_status st = PKG_E_IO;

    snprintf(dest, sizeof dest, "%s/%s", c->root, id);
    registry_path(c, id, regpath, sizeof regpath);
    snprintf(icon_dst, sizeof icon_dst, "%s/%s.info", c->root, id);
    swap_archive_ext(to->url, ext, sizeof ext);
    snprintf(cur_arc, sizeof cur_arc, "%s/%s.%s", c->cache, id, ext);

    txn_id(tid, id);
    snprintf(sroot, sizeof sroot, "%s/%s", c->tmp, tid);
    snprintf(staging, sizeof staging, "%s/%s", sroot, id);
    snprintf(oldtree, sizeof oldtree, "%s/old", sroot);
    snprintf(regbefore, sizeof regbefore, "%s/registry-before.json", sroot);
    if (u_exists(sroot))
        return pkg_fail(e, PKG_E_CONFLICT, "the staging path is already in use", "", sroot);
    if (u_mkdir(sroot) != 0 || u_mkdir(staging) != 0)
        return pkg_fail(e, PKG_E_IO, "cannot create staging", "", staging);

    /* 1. The target tree, and the three inventories. */
    prog(c, "extract", 0, to->size > 0 ? (unsigned long)to->size : 0, 0);
    if (pkg_slow_ms) Delay((ULONG)pkg_slow_ms);
    if (arc_extract(to_archive, staging, to->subdir, PKG_MAX_MEMBER, PKG_MAX_TOTAL, NULL, NULL, why) != 0) {
        st = pkg_fail(e, PKG_E_ARCHIVE, "the archive was rejected", why, to_archive); goto fail_staging;
    }
    prog(c, "extract", 1, 1, 0);
    if (app_stage(staging, id, to->version, to->revision, to->arch[0] ? to->arch : from->arch,
                  to->abi[0] ? to->abi : from->abi, to->app_reqs, to->app_nreq, &to->launch) != 0) {
        st = pkg_fail(e, PKG_E_IO, "cannot write the application description", "", staging); goto fail_staging;
    }
    newinv = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    live   = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    result = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    steps  = (up_step *)malloc(sizeof(up_step) * MAXINV * 2);
    if (!newinv || !live || !result || !steps) { st = pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id); goto fail_staging; }
    if (inv_of_tree(staging, "", newinv, &nnew, MAXINV) != 0) { st = pkg_fail(e, PKG_E_IO, "cannot inventory the staged tree", "", staging); goto fail_staging; }
    if (u_exists(dest) && inv_of_tree(dest, "", live, &nlive, MAXINV) != 0) { st = pkg_fail(e, PKG_E_IO, "cannot inventory the drawer", "", dest); goto fail_staging; }

    /* 2. The plan, printed; a conflict stops here with nothing touched. */
    nsteps = upgrade_plan(old, nold, newinv, nnew, live, nlive, steps, MAXINV * 2, &conflicts);
    if (nsteps < 0) { st = pkg_fail(e, PKG_E_NOMEM, "too many files to plan", "", id); goto fail_staging; }
    if (keep_icons) {
        /* A self-update keeps the icons a user changed (their tool types and
           positions are settings), where an upgrade would stop and ask. */
        for (i = 0; i < nsteps; i++) {
            size_t l = strlen(steps[i].rel);
            if (steps[i].act == UP_CONFLICT && l > 5 && !strcmp(steps[i].rel + l - 5, ".info")) {
                steps[i].act = UP_KEEP_USER;
                conflicts--;
            }
        }
    }
    print_plan(steps, nsteps, dest);
    if (preview) {
        /* Information, not an operation: the counts and the conflicts go
           to the caller and the staging goes away. */
        size_t k = 0;
        memset(preview, 0, sizeof *preview);
        snprintf(preview->from_version, sizeof preview->from_version, "%s", from->version);
        snprintf(preview->to_version,   sizeof preview->to_version,   "%s", to->version);
        preview->from_revision = from->revision; preview->to_revision = to->revision;
        preview->allowed = 1;
        for (i = 0; i < nsteps; i++) switch (steps[i].act) {
            case UP_ADD: preview->add++; break; case UP_REPLACE: preview->replace++; break;
            case UP_REMOVE: preview->remove++; break; case UP_UNCHANGED: preview->unchanged++; break;
            case UP_KEEP_USER: case UP_KEEP_UNKNOWN: preview->kept++; break;
            case UP_CONFLICT:
                preview->conflicts++;
                if (k + 40 < sizeof preview->conflict_list)
                    k += (size_t)snprintf(preview->conflict_list + k, sizeof preview->conflict_list - k,
                                          "%s%s", k ? ", " : "", steps[i].rel);
                break; }
        if (conflicts)
            snprintf(preview->why, sizeof preview->why, "%s: %s",
                     is_rollback ? "a file you changed after the upgrade would be overwritten"
                                 : "a file you changed would be overwritten by the new revision",
                     preview->conflict_list);
        st = PKG_OK;
        goto fail_staging;
    }
    if (conflicts) {
        char det[320]; size_t k = 0;
        det[0] = 0;
        for (i = 0; i < nsteps && k + 40 < sizeof det; i++)
            if (steps[i].act == UP_CONFLICT)
                k += (size_t)snprintf(det + k, sizeof det - k, "%s%s", k ? ", " : "", steps[i].rel);
        st = pkg_fail(e, PKG_E_CONFLICT,
                      is_rollback ? "a file you changed after the upgrade would be overwritten"
                                  : "a file you changed would be overwritten by the new revision",
                      det, id);
        printf("nothing was changed. Move the file(s) away, or remove and install if you\n"
               "do not want them, and try again.\n");
        goto fail_staging;
    }

    /* 3. The result tree: the target's files, plus the user's, in staging. */
    for (i = 0; i < nsteps; i++) {
        const up_step *s = &steps[i];
        if (s->act == UP_UNCHANGED || s->act == UP_KEEP_USER || s->act == UP_KEEP_UNKNOWN) {
            char lp[PKG_MAXPATH];
            u_join(lp, sizeof lp, dest, s->rel);
            if (u_exists(lp) && copy_into(dest, staging, s->rel) != 0) {
                st = pkg_fail(e, PKG_E_IO, "cannot carry a file over into the new tree", "", s->rel);
                goto fail_staging;
            }
        }
    }
    if (inv_of_tree(staging, "", result, &nresult, MAXINV) != 0) { st = pkg_fail(e, PKG_E_IO, "cannot inventory the result", "", staging); goto fail_staging; }

    /* The drawer icon: replaced only if it is ours and unchanged, and the
       target has one. A user's icon, or one we never installed, stays. */
    icon_managed_after = strcmp(from->icon_state, "installed") == 0;
    if (to->icon_member[0] && strcmp(from->icon_state, "installed") == 0 && u_exists(icon_dst)) {
        char sha[65] = "";
        if (sha256_file(icon_dst, sha) == 0 && strcmp(sha, from->icon_sha) == 0) {
            snprintf(icon_stage, sizeof icon_stage, "%s/%s.info", sroot, id);
            snprintf(icon_before, sizeof icon_before, "%s/icon-before.info", sroot);
            if (arc_extract_member(to_archive, to->icon_member, icon_stage, 1L*1024*1024, why) != 0) {
                st = pkg_fail(e, PKG_E_ARCHIVE, "cannot extract the drawer icon", why, to_archive); goto fail_staging;
            }
            if (sha256_file(icon_stage, icon_sha) != 0 || u_copy(icon_dst, icon_before) != 0) {
                st = pkg_fail(e, PKG_E_IO, "cannot stage the drawer icon", "", icon_stage); goto fail_staging;
            }
            icon_size = u_size(icon_stage);
            icon_replace = 1;
        } else {
            printf("note: %s has changed since it was installed and is kept.\n", icon_dst);
        }
    }

    /* Bookkeeping paths for the previous revision. */
    if (is_rollback) {
        snprintf(arc_before, sizeof arc_before, "%s/archive-before.%s", sroot, ext);
        snprintf(preg_before, sizeof preg_before, "%s/prev-registry-before.json", sroot);
    }
    if (!is_rollback) {
        char pdir[PKG_MAXPATH];
        snprintf(pdir, sizeof pdir, "%s/previous", c->db); u_mkdir(pdir);
        snprintf(prev_reg, sizeof prev_reg, "%s/%s.json", pdir, id);
        snprintf(pdir, sizeof pdir, "%s/previous", c->cache); u_mkdir(pdir);
        snprintf(prev_arc, sizeof prev_arc, "%s/%s.%s", pdir, id, ext);
    }

    /* 4. The plan on disk, with a copy of the registry it will replace. */
    if (u_write_atomicish(regbefore, from_text) != 0) { st = pkg_fail(e, PKG_E_IO, "cannot save the registry entry", "", regbefore); goto fail_staging; }
    {
        char dir[PKG_MAXPATH];
        snprintf(dir, sizeof dir, "%s/%s", c->txns, tid);
        if (u_mkdir(dir) != 0) { st = pkg_fail(e, PKG_E_IO, "cannot create the transaction", "", dir); goto fail_staging; }
    }
    {
        size_t cap = 8192 + (size_t)nresult * 400;
        js_out o;
        plan = (char *)malloc(cap);
        if (!plan) { st = pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id); goto fail_staging; }
        js_out_init(&o, plan, cap);
        js_raw(&o, "{\n  "); js_key(&o, "op");       js_vstr(&o, is_rollback ? "rollback" : "upgrade");
        js_raw(&o, ",\n  "); js_key(&o, "package");  js_vstr(&o, id);
        js_raw(&o, ",\n  "); js_key(&o, "dir");      js_vstr(&o, dest);
        js_raw(&o, ",\n  "); js_key(&o, "staging");  js_vstr(&o, staging);
        js_raw(&o, ",\n  "); js_key(&o, "oldtree");  js_vstr(&o, oldtree);
        js_raw(&o, ",\n  "); js_key(&o, "registry"); js_vstr(&o, regpath);
        js_raw(&o, ",\n  "); js_key(&o, "registry_before"); js_vstr(&o, regbefore);
        js_raw(&o, ",\n  "); js_key(&o, "prev_registry"); js_vstr(&o, is_rollback ? from->prev_registry : prev_reg);
        js_raw(&o, ",\n  "); js_key(&o, "prev_archive");  js_vstr(&o, is_rollback ? from->prev_archive : prev_arc);
        js_raw(&o, ",\n  "); js_key(&o, "current_archive"); js_vstr(&o, cur_arc);
        js_raw(&o, ",\n  "); js_key(&o, "target_archive");  js_vstr(&o, to_archive);
        js_raw(&o, ",\n  "); js_key(&o, "archive_before");  js_vstr(&o, arc_before);
        js_raw(&o, ",\n  "); js_key(&o, "prev_registry_before"); js_vstr(&o, preg_before);
        js_raw(&o, ",\n  "); js_key(&o, "icon"); js_raw(&o, " { ");
        js_key(&o, "path");    js_vstr(&o, icon_dst);    js_raw(&o, ", ");
        js_key(&o, "staged");  js_vstr(&o, icon_stage);  js_raw(&o, ", ");
        js_key(&o, "before");  js_vstr(&o, icon_before); js_raw(&o, ", ");
        js_key(&o, "sha256");  js_vstr(&o, icon_sha);    js_raw(&o, ", ");
        js_key(&o, "managed"); js_vlong(&o, icon_replace);
        js_raw(&o, " }");
        js_raw(&o, ",\n  "); js_key(&o, "contents"); js_raw(&o, " [");
        for (i = 0; i < nresult; i++) {
            js_raw(&o, i ? ",\n    { " : "\n    { ");
            js_key(&o, "path");   js_vstr(&o, result[i].rel); js_raw(&o, ", ");
            js_key(&o, "sha256"); js_vstr(&o, result[i].sha); js_raw(&o, ", ");
            js_key(&o, "size");   js_vlong(&o, result[i].size);
            js_raw(&o, " }");
        }
        js_raw(&o, "\n  ]\n}\n");
        if (o.fail || txn_write_plan(c, tid, plan) != 0) {
            st = pkg_fail(e, PKG_E_IO, "cannot write or re-read the plan", "Nothing outside staging has been touched.", tid);
            goto fail_staging;
        }
    }
    STOP(PKG_STOP_AFTER_PLAN);

    /* 5. The swap: two renames. */
    prog(c, "publish", 0, 2, 0);
    if (u_exists(dest) && u_publish(dest, oldtree) != 0) {
        st = pkg_fail(e, PKG_E_IO, "cannot move the drawer aside", "The transaction is kept; the next run rolls it back.", dest); goto out;
    }
    STOP(PKG_STOP_AFTER_OLD_MOVED);
    if (u_publish(staging, dest) != 0) {
        st = pkg_fail(e, PKG_E_IO, "cannot put the new tree in place", "The transaction is kept; the next run rolls it back.", dest); goto out;
    }
    prog(c, "publish", 2, 2, 0);
    STOP(PKG_STOP_AFTER_NEW_PUBLISHED);
    txn_marker(c, tid, "001-dir-done");

    if (icon_replace) {
        u_delete(icon_dst);
        if (u_publish(icon_stage, icon_dst) != 0) {
            st = pkg_fail(e, PKG_E_IO, "cannot put the drawer icon in place", "The transaction is kept; the next run rolls it back.", icon_dst); goto out;
        }
        STOP(PKG_STOP_AFTER_ICON);
    }

    /* 6. The previous-revision bookkeeping, then the registry. */
    if (!is_rollback) {
        if (u_write_atomicish(prev_reg, from_text) != 0) { st = pkg_fail(e, PKG_E_IO, "cannot save the previous registry entry", "", prev_reg); goto out; }
        if (u_exists(cur_arc)) { u_delete(prev_arc); if (u_publish(cur_arc, prev_arc) != 0) prev_arc[0] = 0; }
        else prev_arc[0] = 0;
        if (u_publish(to_archive, cur_arc) != 0) { st = pkg_fail(e, PKG_E_IO, "cannot file the new archive in the cache", "", cur_arc); goto out; }
        if (!prev_arc[0]) printf("note: the previous archive was not in the cache; rollback will not be available.\n");
    } else {
        /* Going back: the saved archive becomes the current one, and the
           entry we came from is not kept -- there is no rollback of a
           rollback. Neither is deleted here, though: what this displaces is
           moved under the staging root, where it survives until commit and
           an undo can put it back. A deleted archive would have to be
           downloaded again, and "again" depends on a network and on the
           exact artefact still being served. */
        if (u_exists(cur_arc) && u_publish(cur_arc, arc_before) != 0) {
            st = pkg_fail(e, PKG_E_IO, "cannot set the current archive aside", "", cur_arc); goto out;
        }
        if (u_exists(from->prev_archive) && u_publish(from->prev_archive, cur_arc) != 0) {
            st = pkg_fail(e, PKG_E_IO, "cannot restore the archive in the cache", "", cur_arc); goto out;
        }
        if (u_exists(from->prev_registry) && u_publish(from->prev_registry, preg_before) != 0) {
            st = pkg_fail(e, PKG_E_IO, "cannot set the previous registry entry aside", "", from->prev_registry); goto out;
        }
    }
    /* The registry records what the PACKAGE delivered -- the target tree's
       own hashes -- not the result tree. A user's edited copy carried over
       as "unchanged" must read as changed to the next remove or upgrade,
       and a file kept for the user is not the package's at all. Recording
       the result once made a rollback treat the user's config as the
       revision's own and replace it. The plan, by contrast, lists the result,
       because that is what an undo has to take back out of the drawer. */
    reg = registry_text(id, to->version, to->revision, to->arch[0] ? to->arch : from->arch,
                        to->abi[0] ? to->abi : from->abi,
                        to->subdir, to->url, to->sha, to->size, newinv, nnew, reqs, nreq,
                        icon_managed_after ? "installed" : (to->icon_member[0] ? "kept-existing" : "none"),
                        icon_managed_after ? (icon_replace ? icon_sha : from->icon_sha) : "",
                        icon_managed_after ? (icon_replace ? icon_size : -1) : -1,
                        to->icon_member,
                        is_rollback ? "" : prev_reg,
                        is_rollback ? "" : prev_arc,
                        to->notes, &to->launch,
                        from->reason, to->rel_known ? to->rel_json : NULL);
    if (!reg || u_write_atomicish(regpath, reg) != 0) { st = pkg_fail(e, PKG_E_IO, "cannot write the registry entry", "", regpath); goto out; }
    STOP(PKG_STOP_AFTER_REGISTRY);
    STOP(PKG_STOP_BEFORE_COMMIT);
    if (txn_marker(c, tid, "005-commit") != 0) {
        st = pkg_fail(e, PKG_E_IO, "cannot record the commit", "The transaction is kept; the next run will roll it back.", tid); goto out;
    }
    /* Committed: the old tree and the transaction go. */
    u_deltree(sroot);
    {
        char dir[PKG_MAXPATH];
        snprintf(dir, sizeof dir, "%s/%s", c->txns, tid);
        u_deltree(dir);
    }
    st = PKG_OK;
    goto out;

fail_staging:
    /* Nothing outside tmp/ was touched: take the staging root away. */
    u_deltree(sroot);
out:
    free(newinv); free(live); free(result); free(steps); free(plan); free(reg);
    return st;
}

static pkg_status do_upgrade(pkg_ctx *c, const char *index_path, const char *id,
                             pkg_stop stop, pkg_plan *preview, int allow_fetch, pkg_err *e)
{
    rev_info from, to; inv_ent *old; int nold = 0, nreq = 0; req_ent reqs[MAXREQ];
    char regpath[PKG_MAXPATH], why[240], ext[8], newarc[PKG_MAXPATH], *from_text = NULL;
    pkg_status st;

    c->progress_id = id;
    registry_path(c, id, regpath, sizeof regpath);
    if (!u_exists(regpath)) return pkg_fail(e, PKG_E_NOT_FOUND, "not installed", "", id);
    old = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    if (!old) return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id);
    if (swap_read_registry(regpath, &from, old, &nold, &from_text) != 0) { free(old); return pkg_fail(e, PKG_E_IO, "the registry entry is not readable", "", regpath); }

    /* The installed target, not whatever the index lists first: an upgrade
       replaces this build with a newer version or revision of the same build
       (same CPU and ABI); upgrade_allowed() decides which is newer. */
    st = swap_read_index(index_path, id, from.arch[0] ? from.arch : pkg_arch(),
                         pkg_abi_override() && *pkg_abi_override() ? pkg_abi_override()
                                                                   : (from.abi[0] ? from.abi : pkg_abi()),
                         &to, reqs, &nreq, e);
    if (st != PKG_OK) goto out;
    if (upgrade_allowed(from.version, from.revision, to.version, to.revision, why, sizeof why) != 0) {
        if (preview) {
            memset(preview, 0, sizeof *preview);
            snprintf(preview->from_version, sizeof preview->from_version, "%s", from.version);
            snprintf(preview->to_version,   sizeof preview->to_version,   "%s", to.version);
            preview->from_revision = from.revision; preview->to_revision = to.revision;
            snprintf(preview->why, sizeof preview->why, "%s", why);
            st = PKG_OK; goto out;
        }
        st = pkg_fail(e, PKG_E_CONFLICT, "nothing to upgrade to", why, id); goto out;
    }
    st = target_check(id, to.arch, to.abi, pkg_abi_override(), e);
    if (st != PKG_OK) goto out;
    st = req_check(id, reqs, nreq, e);
    if (st != PKG_OK) goto out;
    /* The new build's depends must already be here and it must not conflict
       with anything installed: an upgrade installs nothing else. Judged for
       the installed build's CPU and ABI, which the upgrade keeps. */
    st = rel_check_alone(c, index_path, id, 1, to.arch, to.abi, e);
    if (st != PKG_OK) goto out;
    if (strlen(to.sha) != 64) { st = pkg_fail(e, PKG_E_VERIFY, "the index has no hash for this package", "", id); goto out; }

    /* The new archive is fetched beside the current one, not over it: the
       current one is what a rollback needs, and it is filed as previous
       only once the swap has happened. */
    swap_archive_ext(to.url, ext, sizeof ext);
    snprintf(newarc, sizeof newarc, "%s/%s.new.%s", c->cache, id, ext);
    if (preview && !allow_fetch) {
        /* A dry run says whether it may download. Without permission and
           without a cached copy, the plan cannot be made, and saying so is
           the answer -- not a plan guessed from the index alone. */
        char got[65];
        if (!u_exists(newarc) || sha256_file(newarc, got) != 0 || strcmp(got, to.sha) != 0) {
            memset(preview, 0, sizeof *preview);
            snprintf(preview->from_version, sizeof preview->from_version, "%s", from.version);
            snprintf(preview->to_version,   sizeof preview->to_version,   "%s", to.version);
            preview->from_revision = from.revision; preview->to_revision = to.revision;
            preview->allowed = 1; preview->incomplete = 1;
            snprintf(preview->why, sizeof preview->why,
                     "the new revision's archive is not in the cache and this run may not "
                     "download it (%ld bytes from %s); allow fetching to see the file plan",
                     to.size, to.url);
            st = PKG_OK; goto out;
        }
    }
    st = fetch_verified(c, id, to.url, to.sha, to.size, newarc, e);
    if (st != PKG_OK) goto out;
    if (prog(c, "verify", 1, 1, 1)) { st = pkg_fail(e, PKG_E_CANCELLED, "cancelled after verification", "", id); goto out; }

    if (!preview) {
        char a[80], b[80];
        rev_label(a, sizeof a, from.version, from.revision); rev_label(b, sizeof b, to.version, to.revision);
        printf("upgrading %s %s -> %s\n", id, a, b);
    }
    st = swap(c, id, 0, &from, from_text, &to, newarc, old, nold, reqs, nreq, stop, preview, 0, e);
out:
    free(old); free(from_text);
    return st;
}

pkg_status pkg_upgrade(pkg_ctx *c, const char *index_path, const char *id, pkg_stop stop, pkg_err *e)
{ return do_upgrade(c, index_path, id, stop, NULL, 1, e); }

/* arospkg updating itself where apkg installed it as a package: the same
 * transaction as an upgrade, to the release's archive instead of the
 * catalogue's entry (selfup_io.c checked it against the release's
 * SHA256SUMS and put it in this root's cache). Everything the catalogue
 * would have said about the package -- how it starts, its requirements and
 * relations, its notes -- is kept from the installed record, so the
 * registry describes exactly what is in the drawer afterwards. */
pkg_status pkg_selfupdate_swap(pkg_ctx *c, const char *id, const char *archive, const char *version,
                               const char *url, const char *sha, long size, const char *subdir,
                               const char *expect_version, long expect_revision,
                               pkg_stop stop, pkg_err *e)
{
    rev_info from, to; inv_ent *old; int nold = 0, nreq, i;
    req_ent reqs[MAXREQ];
    char regpath[PKG_MAXPATH], *from_text = NULL;
    pkg_status st;

    c->progress_id = id;
    registry_path(c, id, regpath, sizeof regpath);
    if (!u_exists(regpath)) return pkg_fail(e, PKG_E_NOT_FOUND, "not installed", "", id);
    if (!(old = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV))) return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id);
    if (swap_read_registry(regpath, &from, old, &nold, &from_text) != 0) {
        free(old); return pkg_fail(e, PKG_E_IO, "the registry entry is not readable", "", regpath);
    }
    /* Decided here, under this root's lock: the installed revision is still
       the one the update was planned for, and the release is newer. A
       self-update never moves a package backwards or sideways. */
    if (strcmp(from.version, expect_version) || from.revision != expect_revision
        || pkg_release_vercmp(from.version, version) >= 0) {
        char a[80];
        rev_label(a, sizeof a, from.version, from.revision);
        free(old); free(from_text);
        return pkg_fail(e, PKG_E_CONFLICT, "the installed package is not the one the update was planned for", a, id);
    }
    to = from;
    snprintf(to.version, sizeof to.version, "%s", version);
    to.revision = 0;
    snprintf(to.url, sizeof to.url, "%s", url);
    snprintf(to.sha, sizeof to.sha, "%s", sha);
    to.size = size;
    snprintf(to.subdir, sizeof to.subdir, "%s", subdir);
    to.prev_registry[0] = to.prev_archive[0] = 0;
    nreq = from.app_nreq;
    for (i = 0; i < nreq; i++) reqs[i] = from.app_reqs[i];
    req_probe_all(reqs, nreq);
    {
        char a[80];
        rev_label(a, sizeof a, from.version, from.revision);
        printf("updating %s %s -> %s\n", id, a, version);
    }
    st = swap(c, id, 0, &from, from_text, &to, archive, old, nold, reqs, nreq, stop, NULL, 1, e);
    free(old); free(from_text);
    return st;
}

pkg_status pkg_upgrade_preview(pkg_ctx *c, const char *index_path, const char *id,
                               int allow_fetch, pkg_plan *out, pkg_err *e)
{ return do_upgrade(c, index_path, id, PKG_STOP_NONE, out, allow_fetch, e); }

static pkg_status do_rollback(pkg_ctx *c, const char *id, pkg_stop stop, pkg_plan *preview, pkg_err *e)
{
    rev_info from, to; inv_ent *old, *dummy; int nold = 0, ndummy = 0, nreq = 0; req_ent reqs[MAXREQ];
    char regpath[PKG_MAXPATH], *from_text = NULL, *to_text = NULL;
    pkg_status st;

    c->progress_id = id;
    registry_path(c, id, regpath, sizeof regpath);
    if (!u_exists(regpath)) return pkg_fail(e, PKG_E_NOT_FOUND, "not installed", "", id);
    old = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    dummy = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    if (!old || !dummy) { free(old); free(dummy); return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id); }
    if (swap_read_registry(regpath, &from, old, &nold, &from_text) != 0) { st = pkg_fail(e, PKG_E_IO, "the registry entry is not readable", "", regpath); goto out; }
    if (preview) {
        memset(preview, 0, sizeof *preview);
        snprintf(preview->from_version, sizeof preview->from_version, "%s", from.version);
        preview->from_revision = from.revision;
        if (!pkg_can_rollback(c, id, preview->why, sizeof preview->why)) { st = PKG_OK; goto out; }
    }
    if (!from.prev_registry[0] || !u_exists(from.prev_registry)) {
        st = pkg_fail(e, PKG_E_NOT_FOUND, "nothing to roll back to",
                      "No upgrade of this package is recorded, or its record is gone.", id); goto out;
    }
    if (!from.prev_archive[0] || !u_exists(from.prev_archive)) {
        st = pkg_fail(e, PKG_E_NOT_FOUND, "the previous revision's archive is not available",
                      "It was not in the cache when the upgrade ran, or has been deleted since.", from.prev_archive); goto out;
    }
    if (swap_read_registry(from.prev_registry, &to, dummy, &ndummy, &to_text) != 0) { st = pkg_fail(e, PKG_E_IO, "the previous registry entry is not readable", "", from.prev_registry); goto out; }
    {
        /* The archive on disk must still be the one that entry describes. */
        char got[65];
        if (sha256_file(from.prev_archive, got) != 0 || strcmp(got, to.sha) != 0) {
            st = pkg_fail(e, PKG_E_VERIFY, "the previous archive does not match its record", got, from.prev_archive); goto out;
        }
    }
    /* The previous entry records its own subdir (entries written before
       that field exists read as "", the whole archive). The requirements are
       not re-checked on the way back: they were checked when that revision
       was installed, and the machine is the same one. */
    nreq = 0;
    if (!preview) { char b[80]; rev_label(b, sizeof b, to.version, to.revision); printf("rolling %s back to %s\n", id, b); }
    /* Going back must not undo a relation: the earlier revision's depends
       must be here, it must not conflict, and nothing installed may need a
       newer one. Its relations are the ones its record kept; an entry from
       before 0.6 kept none, and is judged by what depends on it. */
    {
        pkg_status rs = rel_check_rollback(c, id, to.version, to.rel_known ? to.rel_json : "",
                                           from.arch[0] ? from.arch : pkg_arch(),
                                           from.abi[0] ? from.abi : pkg_abi(), e);
        if (rs != PKG_OK) { st = rs; goto out; }
    }
    st = swap(c, id, 1, &from, from_text, &to, from.prev_archive, old, nold, reqs, nreq, stop, preview, 0, e);
out:
    free(old); free(dummy); free(from_text); free(to_text);
    return st;
}

pkg_status pkg_rollback(pkg_ctx *c, const char *id, pkg_stop stop, pkg_err *e)
{ return do_rollback(c, id, stop, NULL, e); }

pkg_status pkg_rollback_preview(pkg_ctx *c, const char *id, pkg_plan *out, pkg_err *e)
{ return do_rollback(c, id, PKG_STOP_NONE, out, e); }

int pkg_can_upgrade(const pkg_entry *e, char *why, size_t n)
{
    if (!e->installed) { snprintf(why, n, "not installed"); return 0; }
    return upgrade_allowed(e->installed_version, e->installed_revision, e->version, e->revision, why, n) == 0;
}

int pkg_can_rollback(pkg_ctx *c, const char *id, char *why, size_t n)
{
    rev_info r; inv_ent *inv; int ninv = 0; char regpath[PKG_MAXPATH];
    registry_path(c, id, regpath, sizeof regpath);
    if (!u_exists(regpath)) { snprintf(why, n, "not installed"); return 0; }
    inv = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    if (!inv) { snprintf(why, n, "out of memory"); return 0; }
    if (swap_read_registry(regpath, &r, inv, &ninv, NULL) != 0) { free(inv); snprintf(why, n, "the registry entry is not readable"); return 0; }
    free(inv);
    if (!r.prev_registry[0] || !u_exists(r.prev_registry)) {
        snprintf(why, n, "no upgrade of this package is recorded, or its record is gone"); return 0;
    }
    if (!r.prev_archive[0] || !u_exists(r.prev_archive)) {
        snprintf(why, n, "the previous revision's archive is not in the cache (%s)", r.prev_archive[0] ? r.prev_archive : "none recorded"); return 0;
    }
    return 1;
}
