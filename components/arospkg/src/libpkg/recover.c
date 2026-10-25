/* Recovery.
 *
 * Driven by the plan and by what is actually on disk -- never by the markers.
 * A crash between performing a mutation and creating its marker is the most
 * likely crash there is, because the two are adjacent, so a marker-driven
 * rollback would miss exactly the case it exists for. 005-commit is consulted,
 * but only because it records a DECISION rather than an action.
 */
#include "internal.h"
#include "util.h"
#include "json.h"
#include "sha256.h"
#include "icon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void txn_dir(pkg_ctx *c, const char *id, char *out, size_t n)
{ snprintf(out, n, "%s/%s", c->txns, id); }

static void recover_one(pkg_ctx *c, const char *tid)
{
    char dir[PKG_MAXPATH], planp[PKG_MAXPATH];
    char *plan = NULL;
    js_tok *t = NULL;
    int ntok, arr, el, ninv = 0;
    char op[16] = "", dest[PKG_MAXPATH] = "", regpath[PKG_MAXPATH] = "";
    char regbefore[16] = "", dirbefore[16] = "", staging[PKG_MAXPATH] = "";
    inv_ent *inv = NULL;
    size_t len;
    int committed;
    char icon_path[PKG_MAXPATH] = "", icon_staged[PKG_MAXPATH] = "", icon_sha[65] = "";
    int icon_managed = 0;

    txn_dir(c, tid, dir, sizeof dir);
    snprintf(planp, sizeof planp, "%s/000-plan.json", dir);
    committed = txn_has(c, tid, "005-commit");

    plan = u_read_all(planp, &len);
    if (!plan) {
        /* Unreadable after a restart is NOT "nothing happened". Closing and
         * re-reading the plan when it was written proves it was readable then,
         * which covers a killed process; it says nothing about a power loss
         * that left the file never fully committed. Stop, keep everything. */
        printf("  %s: plan unreadable -- stopping, nothing cleaned up\n", tid);
        pkgctx_unresolved(c);
        return;
    }
    ntok = js_parse_alloc(plan, len, &t, PKG_JSON_MAXTOK);
    if (ntok <= 0) {
        printf("  %s: plan is not valid JSON -- stopping\n", tid);
        free(plan); free(t); pkgctx_unresolved(c);
        return;
    }

    js_str(plan, t, js_member(plan, t, ntok, 0, "op"),       op,        sizeof op);
    js_str(plan, t, js_member(plan, t, ntok, 0, "dir"),      dest,      sizeof dest);
    js_str(plan, t, js_member(plan, t, ntok, 0, "registry"), regpath,   sizeof regpath);
    js_str(plan, t, js_member(plan, t, ntok, 0, "registry_before"), regbefore, sizeof regbefore);
    js_str(plan, t, js_member(plan, t, ntok, 0, "dir_before"), dirbefore, sizeof dirbefore);
    js_str(plan, t, js_member(plan, t, ntok, 0, "staging"),    staging,   sizeof staging);
    {   /* Plans written before icons existed have no "icon" and read as unmanaged. */
        int ic = js_member(plan, t, ntok, 0, "icon");
        if (ic >= 0) {
            js_str(plan, t, js_member(plan, t, ntok, ic, "path"),   icon_path,   sizeof icon_path);
            js_str(plan, t, js_member(plan, t, ntok, ic, "staged"), icon_staged, sizeof icon_staged);
            js_str(plan, t, js_member(plan, t, ntok, ic, "sha256"), icon_sha,    sizeof icon_sha);
            icon_managed = (int)js_long(plan, t, js_member(plan, t, ntok, ic, "managed"), 0);
        }
    }

    inv = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    if (!inv) { free(plan); free(t); pkgctx_unresolved(c); return; }
    arr = js_member(plan, t, ntok, 0, "contents");
    for (el = js_child(t, ntok, arr); el >= 0 && ninv < MAXINV; el = js_sibling(t, ntok, el)) {
        js_str(plan, t, js_member(plan, t, ntok, el, "path"),   inv[ninv].rel, sizeof inv[ninv].rel);
        js_str(plan, t, js_member(plan, t, ntok, el, "sha256"), inv[ninv].sha, sizeof inv[ninv].sha);
        inv[ninv].size = js_long(plan, t, js_member(plan, t, ntok, el, "size"), -1);
        ninv++;
    }

    if (committed) {
        printf("  %s: committed -- cleaning up\n", tid);
        if (staging[0]) {
            /* A swap keeps its old tree under the staging root until commit;
               a crash between the two leaves it for here. */
            char sroot[PKG_MAXPATH], *slash;
            snprintf(sroot, sizeof sroot, "%s", staging);
            slash = strrchr(sroot, '/'); if (slash) *slash = 0;
            if (strcmp(op, "install") != 0 && u_exists(sroot)) u_deltree(sroot);
        }
        u_deltree(dir);
        pkgctx_resolved(c);
    } else if (strcmp(op, "install") == 0) {
        int removed = 0, kept = 0;
        printf("  %s: install without commit -- rolling back\n", tid);
        if (u_exists(regpath) && strcmp(regbefore, "absent") == 0) u_delete(regpath);

        /* Ownership, not content.
         *
         * The inventory records the hashes of the files we STAGED. Matching a
         * file in the destination against it proves the content is the same;
         * it proves nothing about who put it there. A previous install of the
         * same version leaves byte-identical files, and an earlier version of
         * this rollback deleted 50 of them believing they were its own -- in a
         * transaction that had stopped BEFORE publishing anything, so it could
         * not have created a single one.
         *
         * The publish is a rename of the whole staged tree, so the staged tree
         * still being there is a disk-observable proof that the rename never
         * happened, and therefore that nothing in the destination is ours. */
        if (staging[0] && u_exists(staging)) {
            char sroot[PKG_MAXPATH], *slash;
            printf("      staged tree still present: publish never happened,\n");
            printf("      so nothing in %s belongs to this transaction\n", dest);
            /* Remove the transaction's whole staging root, and never put one
               back at this path: a recreated staging tree would make a later
               run conclude that a publish which HAD happened never did. */
            snprintf(sroot, sizeof sroot, "%s", staging);
            slash = strrchr(sroot, '/');
            if (slash) *slash = 0;
            u_deltree(u_exists(sroot) ? sroot : staging);
            u_deltree(dir);
            free(plan); free(t); free(inv);
            pkgctx_resolved(c);
            return;
        }
        if (pkg_break_recovery_after > 0) {
            /* Interrupt the rollback itself. Whatever this leaves must be
               finishable by a plain re-run, with no special case. */
            int done = 0, k2 = 0, j;
            for (j = 0; j < ninv && done < pkg_break_recovery_after; j++) {
                inv_ent one = inv[j];
                int r1 = 0;
                inv_remove(dest, &one, 1, &r1, &k2);
                done += r1;
            }
            printf("      recovery interrupted on purpose after %d file(s)\n", done);
            free(plan); free(t); free(inv);
            pkgctx_unresolved(c);
            return;
        }
        inv_remove(dest, inv, ninv, &removed, &kept);
        prune_empty(dest);
        printf("      %d file(s) removed, %d left because changed\n", removed, kept);
        if (icon_path[0]) {
            char sha[65] = "";
            int present = u_exists(icon_path);
            if (present && sha256_file(icon_path, sha) != 0) sha[0] = 0;
            switch (icon_on_rollback(icon_managed,
                                     icon_staged[0] && u_exists(icon_staged),
                                     present, sha, icon_sha)) {
            case ICON_DELETE:
                u_delete(icon_path);
                printf("      drawer icon removed\n");
                break;
            case ICON_UNKNOWN:
                /* Ours by the plan, not ours by content: nothing here can say
                   whose it is, so it stays and so does the transaction. */
                printf("      %s is not the icon this install wrote -- left in place,\n"
                       "      transaction kept\n", icon_path);
                pkgctx_unresolved(c);
                free(plan); free(t); free(inv);
                return;
            default: break;
            }
        }
        if (u_exists(dest)) {
            /* Something we did not install, or something edited since. Section 2
             * forbids destroying it to tidy up, so the transaction stays. */
            printf("      %s still exists -- left in place, transaction kept\n", dest);
            pkgctx_unresolved(c);
        } else {
            u_deltree(dir);
            pkgctx_resolved(c);
        }
    } else if (strcmp(op, "remove") == 0) {
        int removed = 0, kept = 0;
        /* Roll forward: a deleted file cannot be restored, so a partial removal
         * can only be finished. */
        printf("  %s: remove without commit -- rolling forward\n", tid);
        inv_remove(dest, inv, ninv, &removed, &kept);
        prune_empty(dest);
        {
            int icon_kept = 0;
            if (icon_path[0]) {
                char sha[65] = "";
                int present = u_exists(icon_path);
                if (present && sha256_file(icon_path, sha) != 0) sha[0] = 0;
                if (icon_on_remove(icon_managed, present, sha, icon_sha) == ICON_DELETE)
                    u_delete(icon_path);
                else if (icon_managed && present) {
                    icon_kept = 1;
                    printf("      %s changed since install -- kept\n", icon_path);
                }
            }
            /* The same rule as an uninterrupted removal: whatever is kept is
               recorded before the registry that attributes it is deleted. If
               the record cannot be written, the registry and the transaction
               both stay, and a later run tries again. */
            if ((kept || icon_kept || u_exists(dest)) && u_exists(regpath)) {
                const char *slash = strrchr(dest, '/');
                if (leftovers_report(c, slash ? slash + 1 : dest, dest, inv, ninv,
                                     removed, kept, icon_kept ? icon_path : "") != 0) {
                    printf("      cannot record what is being kept -- registry and\n"
                           "      transaction left in place\n");
                    pkgctx_unresolved(c);
                    free(plan); free(t); free(inv);
                    return;
                }
            }
        }
        if (u_exists(regpath)) u_delete(regpath);
        printf("      %d file(s) removed, %d left because changed\n", removed, kept);
        u_deltree(dir);
        pkgctx_resolved(c);
    } else if (strcmp(op, "upgrade") == 0 || strcmp(op, "rollback") == 0) {
        /* Undo a swap. Which of its states was reached is read off the disk:
           the staged tree still present means the second rename never
           happened; the old tree still aside means the first one did. */
        char oldtree[PKG_MAXPATH] = "", regbefore[PKG_MAXPATH] = "", sroot[PKG_MAXPATH], *slash;
        char prev_reg[PKG_MAXPATH] = "", prev_arc[PKG_MAXPATH] = "", cur_arc[PKG_MAXPATH] = "", tgt_arc[PKG_MAXPATH] = "";
        char icon_before[PKG_MAXPATH] = "";
        int removed = 0, kept = 0, is_rollback = strcmp(op, "rollback") == 0;
        js_str(plan, t, js_member(plan, t, ntok, 0, "oldtree"),         oldtree,   sizeof oldtree);
        js_str(plan, t, js_member(plan, t, ntok, 0, "registry_before"), regbefore, sizeof regbefore);
        js_str(plan, t, js_member(plan, t, ntok, 0, "prev_registry"),   prev_reg,  sizeof prev_reg);
        js_str(plan, t, js_member(plan, t, ntok, 0, "prev_archive"),    prev_arc,  sizeof prev_arc);
        js_str(plan, t, js_member(plan, t, ntok, 0, "current_archive"), cur_arc,   sizeof cur_arc);
        js_str(plan, t, js_member(plan, t, ntok, 0, "target_archive"),  tgt_arc,   sizeof tgt_arc);
        {
            int ic = js_member(plan, t, ntok, 0, "icon");
            if (ic >= 0) js_str(plan, t, js_member(plan, t, ntok, ic, "before"), icon_before, sizeof icon_before);
        }
        snprintf(sroot, sizeof sroot, "%s", staging);
        slash = strrchr(sroot, '/'); if (slash) *slash = 0;
        printf("  %s: %s without commit -- rolling back\n", tid, op);

        if (staging[0] && u_exists(staging)) {
            /* State 1 or 2: nothing of ours is in the drawer. */
            if (!u_exists(dest) && oldtree[0] && u_exists(oldtree)) {
                if (u_publish(oldtree, dest) != 0) {
                    printf("      cannot move the drawer back from %s -- transaction kept\n", oldtree);
                    pkgctx_unresolved(c); free(plan); free(t); free(inv); return;
                }
                printf("      drawer moved back into place\n");
            } else printf("      staged tree still present: the drawer was never replaced\n");
            u_deltree(sroot); u_deltree(dir);
            pkgctx_resolved(c); free(plan); free(t); free(inv); return;
        }

        /* State 3 or later: the published tree is ours by the plan's hashes;
           a file that no longer matches was changed since and is kept. */
        if (u_exists(dest)) {
            inv_remove(dest, inv, ninv, &removed, &kept);
            prune_empty(dest);
            printf("      %d file(s) of the new tree removed, %d left because changed\n", removed, kept);
        }
        if (u_exists(dest)) {
            printf("      %s is not empty after removing the new tree -- left in place,\n"
                   "      the old tree stays at %s, transaction kept\n", dest, oldtree);
            pkgctx_unresolved(c); free(plan); free(t); free(inv); return;
        }
        if (oldtree[0] && u_exists(oldtree) && u_publish(oldtree, dest) != 0) {
            printf("      cannot move the old drawer back -- transaction kept\n");
            pkgctx_unresolved(c); free(plan); free(t); free(inv); return;
        }
        if (icon_managed && icon_before[0] && u_exists(icon_before)) {
            char sha[65] = "";
            if (u_exists(icon_path) && sha256_file(icon_path, sha) == 0 && strcmp(sha, icon_sha) == 0)
                u_delete(icon_path);
            if (!u_exists(icon_path)) u_copy(icon_before, icon_path);
        }
        if (regbefore[0] && u_exists(regbefore)) u_copy(regbefore, regpath);
        /* The archive bookkeeping, reversed by what is on disk. */
        if (!is_rollback) {
            if (tgt_arc[0] && !u_exists(tgt_arc) && cur_arc[0] && u_exists(cur_arc)) u_publish(cur_arc, tgt_arc);
            if (prev_arc[0] && u_exists(prev_arc) && !u_exists(cur_arc)) u_publish(prev_arc, cur_arc);
            if (prev_reg[0] && u_exists(prev_reg)) {
                /* Ours only if it holds what we saved; an older upgrade's
                   record is not deleted on the strength of a later, undone one. */
                size_t la, lb; char *a = u_read_all(prev_reg, &la), *b = u_read_all(regbefore, &lb);
                if (a && b && la == lb && memcmp(a, b, la) == 0) u_delete(prev_reg);
                free(a); free(b);
            }
        } else {
            char arc_before[PKG_MAXPATH] = "", preg_before[PKG_MAXPATH] = "";
            js_str(plan, t, js_member(plan, t, ntok, 0, "archive_before"),       arc_before,  sizeof arc_before);
            js_str(plan, t, js_member(plan, t, ntok, 0, "prev_registry_before"), preg_before, sizeof preg_before);
            /* The previous archive went back to the current slot; the one it
               displaced waited under the staging root. Both return. */
            if (prev_arc[0] && !u_exists(prev_arc) && cur_arc[0] && u_exists(cur_arc)) u_publish(cur_arc, prev_arc);
            if (arc_before[0] && u_exists(arc_before) && cur_arc[0] && !u_exists(cur_arc)) u_publish(arc_before, cur_arc);
            if (preg_before[0] && u_exists(preg_before) && prev_reg[0] && !u_exists(prev_reg)) u_publish(preg_before, prev_reg);
        }
        printf("      drawer, icon and registry restored\n");
        u_deltree(sroot); u_deltree(dir);
        pkgctx_resolved(c);
    } else {
        printf("  %s: unknown op '%s' -- stopping\n", tid, op);
        pkgctx_unresolved(c);
    }

    free(plan); free(t); free(inv);
}

void pkg_recover_all(pkg_ctx *c)
{
    char **names; int n, i;
    if (u_listdir(c->txns, &names, &n) != 0) return;
    if (n) printf("recovery: %d transaction(s) to resolve\n", n);
    for (i = 0; i < n; i++) {
        char d[PKG_MAXPATH];
        u_join(d, sizeof d, c->txns, names[i]);
        if (u_isdir(d)) recover_one(c, names[i]);
    }
    u_freelist(names, n);
}
