#include "internal.h"
#include "net.h"
#include "util.h"
#include "json.h"
#include "sha256.h"
#include "zip.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <exec/tasks.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- boot id */

/* A random token in ENV:, which resolves to RAM Disk:ENV and so cannot outlive
 * the boot. Created with the same create-if-absent primitive as the lock, so
 * two processes starting together cannot each mint one. */
static void boot_id(char out[40])
{
    char dir[] = "ENV:pkg", path[] = "ENV:pkg/boot-id", tmp[64];
    char *have;
    FILE *f;
    u_mkdir(dir);
    have = u_read_all(path, NULL);
    if (have) {
        have[strcspn(have, "\r\n")] = 0;
        snprintf(out, 40, "%s", have);
        free(have);
        if (out[0]) return;
    }
    snprintf(tmp, sizeof tmp, "ENV:pkg/boot-id.%lx.new",
             (unsigned long)(IPTR)FindTask(NULL) ^ (unsigned long)time(NULL));
    f = fopen(tmp, "wb");
    if (f) {
        fprintf(f, "%lx%lx\n", (unsigned long)time(NULL),
                (unsigned long)(IPTR)FindTask(NULL));
        fclose(f);
        if (Rename((CONST_STRPTR)tmp, (CONST_STRPTR)path) == 0)
            u_delete(tmp);              /* someone else won; read theirs */
    }
    have = u_read_all(path, NULL);
    if (have) { have[strcspn(have, "\r\n")] = 0; snprintf(out, 40, "%s", have); free(have); }
    else snprintf(out, 40, "unknown");
}

/* ------------------------------------------------------------------- lock */

static pkg_status lock_take(pkg_ctx *c, pkg_err *e)
{
    char boot[40], tmp[PKG_MAXPATH], body[512];
    struct Task *me = FindTask(NULL);
    FILE *f;

    boot_id(boot);
    snprintf(c->nonce, sizeof c->nonce, "%lx%lx",
             (unsigned long)(IPTR)me, (unsigned long)time(NULL));
    snprintf(tmp, sizeof tmp, "%s/lock.%s.new", c->db, c->nonce);
    snprintf(body, sizeof body,
             "{\"nonce\":\"%s\",\"boot_id\":\"%s\",\"task\":\"%s\",\"proc\":\"%lx\"}\n",
             c->nonce, boot, me->tc_Node.ln_Name ? me->tc_Node.ln_Name : "?",
             (unsigned long)(IPTR)me);

    f = fopen(tmp, "wb");
    if (!f) return pkg_fail(e, PKG_E_IO, "cannot write the lock file", tmp, c->db);
    fputs(body, f);
    if (fclose(f) != 0) return pkg_fail(e, PKG_E_IO, "cannot write the lock file", tmp, c->db);

    /* Rename refuses to overwrite, which makes this an atomic create-if-absent.
     * Measured: two processes, 150 attempts each, zero violations. */
    if (Rename((CONST_STRPTR)tmp, (CONST_STRPTR)c->lock_owner)) {
        c->holds_lock = 1;
        return PKG_OK;
    }
    u_delete(tmp);

    {   /* Someone holds it. Same boot means live; a different boot means the
         * holder is gone. Reclaiming is deliberately NOT automatic. */
        char *rec = u_read_all(c->lock_owner, NULL);
        js_tok t[64]; int n; char theirs[40] = "", task[64] = "";
        if (rec && (n = js_parse(rec, strlen(rec), t, 64)) > 0) {
            js_str(rec, t, js_member(rec, t, n, 0, "boot_id"), theirs, sizeof theirs);
            js_str(rec, t, js_member(rec, t, n, 0, "task"), task, sizeof task);
        }
        free(rec);
        if (theirs[0] && strcmp(theirs, boot) != 0)
            return pkg_fail(e, PKG_E_STALE_LOCK,
                        "the lock was left by a run from a previous boot",
                        "Stop every other pkg and PkgManager, then run: pkg unlock",
                        c->lock_owner);
        return pkg_fail(e, PKG_E_LOCKED, "another instance is running",
                    task[0] ? task : "unknown task", c->lock_owner);
    }
}

static void lock_release(pkg_ctx *c)
{
    char *rec;
    js_tok t[64]; int n; char mine[40] = "";
    if (!c->holds_lock) return;
    rec = u_read_all(c->lock_owner, NULL);
    if (rec && (n = js_parse(rec, strlen(rec), t, 64)) > 0)
        js_str(rec, t, js_member(rec, t, n, 0, "nonce"), mine, sizeof mine);
    free(rec);
    /* Only remove the record if it is still ours. */
    if (!mine[0] || strcmp(mine, c->nonce) == 0) u_delete(c->lock_owner);
    c->holds_lock = 0;
}

pkg_status pkg_unlock(const char *root, pkg_err *e)
{
    char path[PKG_MAXPATH];
    snprintf(path, sizeof path, "%s/db/lock.owner", root);
    if (!u_exists(path)) return pkg_fail(e, PKG_E_NOT_FOUND, "no lock is held", "", path);
    if (u_delete(path) != 0) return pkg_fail(e, PKG_E_IO, "cannot remove the lock", "", path);
    return PKG_OK;
}

/* -------------------------------------------------------------- inventory */

int inv_of_tree(const char *base, const char *prefix,
                       inv_ent *inv, int *n, int max)
{
    char **names; int cnt, i, bad = 0;
    char path[PKG_MAXPATH], rel[256];
    if (u_listdir(base, &names, &cnt) != 0) return -1;
    for (i = 0; i < cnt; i++) {
        u_join(path, sizeof path, base, names[i]);
        if (prefix && *prefix) snprintf(rel, sizeof rel, "%s/%s", prefix, names[i]);
        else                   snprintf(rel, sizeof rel, "%s", names[i]);
        if (u_isdir(path)) {
            if (inv_of_tree(path, rel, inv, n, max) != 0) bad = 1;
        } else {
            if (*n >= max) { bad = 1; break; }
            snprintf(inv[*n].rel, sizeof inv[*n].rel, "%s", rel);
            inv[*n].size = u_size(path);
            if (sha256_file(path, inv[*n].sha) != 0) { bad = 1; break; }
            (*n)++;
        }
    }
    u_freelist(names, cnt);
    return bad ? -1 : 0;
}

/* ------------------------------------------------------------ transaction */

/* Returns 0 on success. The caller decides how much a failure matters: for the
 * audit markers very little, but 005-commit records the DECISION recovery
 * reads, so a marker that silently failed to appear would turn a finished
 * install into one that gets rolled back. */
int txn_marker(pkg_ctx *c, const char *id, const char *name)
{
    char p[PKG_MAXPATH];
    FILE *f;
    /* Injected failure: the only way to reach the code that handles a marker
       that could not be written, since stopping early never enters it. */
    if (pkg_injected_fault == PKG_FAIL_COMMIT && strcmp(name, "005-commit") == 0)
        return -1;
    snprintf(p, sizeof p, "%s/%s/%s", c->txns, id, name);
    f = fopen(p, "wb");
    if (!f) return -1;
    if (fclose(f) != 0) return -1;
    return u_exists(p) ? 0 : -1;
}

int txn_has(pkg_ctx *c, const char *id, const char *name)
{
    char p[PKG_MAXPATH];
    snprintf(p, sizeof p, "%s/%s/%s", c->txns, id, name);
    return u_exists(p);
}

/* Write the plan, then close, reopen and re-parse it. That proves it is
 * readable NOW, which covers a killed process; it does not prove a plan found
 * unreadable after a restart means nothing happened, and recovery treats that
 * case differently. */
int txn_write_plan(pkg_ctx *c, const char *id, const char *text)
{
    char p[PKG_MAXPATH];
    char *back; size_t len; js_tok *t; int ok;
    snprintf(p, sizeof p, "%s/%s/000-plan.json", c->txns, id);
    {
        FILE *f = fopen(p, "wb");
        if (!f) return -1;
        fputs(text, f);
        if (fclose(f) != 0) return -1;
    }
    back = u_read_all(p, &len);
    if (!back) return -1;
    /* The plan carries one object per installed file, so a fixed 64-token
       buffer overflows on any real package -- ZapHod alone has hundreds. That
       failed safely (nothing outside staging had been touched) but it failed. */
    ok = js_parse_alloc(back, len, &t, PKG_JSON_MAXTOK) > 0;
    free(t);
    free(back);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------- inventory io */

int inv_from_registry(const char *json, size_t len,
                             inv_ent *inv, int max, int *n)
{
    js_tok *t = NULL;
    int ntok, arr, el;
    *n = 0;
    ntok = js_parse_alloc(json, len, &t, PKG_JSON_MAXTOK);
    if (ntok <= 0) { free(t); return -1; }
    arr = js_member(json, t, ntok, 0, "contents");
    for (el = js_child(t, ntok, arr); el >= 0 && *n < max; el = js_sibling(t, ntok, el)) {
        js_str(json, t, js_member(json, t, ntok, el, "path"), inv[*n].rel, sizeof inv[*n].rel);
        js_str(json, t, js_member(json, t, ntok, el, "sha256"), inv[*n].sha, sizeof inv[*n].sha);
        inv[*n].size = js_long(json, t, js_member(json, t, ntok, el, "size"), -1);
        (*n)++;
    }
    free(t);
    return 0;
}

/* Delete the files an inventory lists, but only where the content still matches.
 * Returns counts: removed, kept because modified, and (via scan) unknown files
 * left behind. This is section 2's uninstall rule and recovery reuses it. */
void inv_remove(const char *dir, inv_ent *inv, int n,
                       int *removed, int *kept)
{
    int i;
    *removed = *kept = 0;
    for (i = 0; i < n; i++) {
        char p[PKG_MAXPATH], sha[65];
        u_join(p, sizeof p, dir, inv[i].rel);
        if (!u_exists(p)) continue;
        if (sha256_file(p, sha) == 0 && strcmp(sha, inv[i].sha) == 0) {
            if (u_delete(p) == 0) (*removed)++;
        } else {
            (*kept)++;
        }
    }
}

/* Remove directories that are now empty, deepest first. Returns 1 if `dir`
 * itself went away. */
int prune_empty(const char *dir)
{
    char **names; int cnt, i, left = 0;
    if (!u_isdir(dir)) return 0;
    if (u_listdir(dir, &names, &cnt) != 0) return 0;
    for (i = 0; i < cnt; i++) {
        char child[PKG_MAXPATH];
        u_join(child, sizeof child, dir, names[i]);
        if (u_isdir(child)) { if (!prune_empty(child)) left++; }
        else left++;
    }
    u_freelist(names, cnt);
    if (left == 0) return u_delete(dir) == 0;
    return 0;
}

/* ------------------------------------------------------------------ open */

pkg_status pkg_open(const char *root, pkg_mode mode, pkg_ctx **out, pkg_err *e)
{
    pkg_ctx *c = (pkg_ctx *)calloc(1, sizeof *c);
    pkg_status st;
    if (!c) return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", "");
    snprintf(c->root, sizeof c->root, "%s", root);
    snprintf(c->db,        sizeof c->db,        "%s/db", root);
    snprintf(c->installed, sizeof c->installed, "%s/db/installed", root);
    snprintf(c->txns,      sizeof c->txns,      "%s/db/transactions", root);
    snprintf(c->doctor,    sizeof c->doctor,    "%s/db/doctor", root);
    snprintf(c->cache,     sizeof c->cache,     "%s/cache", root);
    snprintf(c->tmp,       sizeof c->tmp,       "%s/tmp", root);
    snprintf(c->lock_owner,sizeof c->lock_owner,"%s/db/lock.owner", root);

    u_mkpath(c->root); u_mkdir(c->db); u_mkdir(c->installed);
    u_mkdir(c->txns);  u_mkdir(c->doctor); u_mkdir(c->cache); u_mkdir(c->tmp);

    if (mode == PKG_OPEN_READ) {
        /* Read-only never recovers and never locks, but it must not present a
         * half-finished transaction as a settled state. Count them so the
         * front end can say so. */
        char **names; int n, i;
        if (u_listdir(c->txns, &names, &n) == 0) {
            for (i = 0; i < n; i++) {
                char d[PKG_MAXPATH], m[PKG_MAXPATH];
                u_join(d, sizeof d, c->txns, names[i]);
                if (!u_isdir(d)) continue;
                snprintf(m, sizeof m, "%s/005-commit", d);
                if (!u_exists(m)) c->pending++;
            }
            u_freelist(names, n);
        }
        *out = c;
        return PKG_OK;
    }

    st = lock_take(c, e);
    if (st != PKG_OK) { free(c); return st; }

    /* Order is the requirement: lock, then recover, and only then clean tmp.
     * Cleaning first would discard the staged tree a roll-forward needs. */
    pkg_recover_all(c);
    if (c->unresolved && mode == PKG_OPEN_DOCTOR) {
        /* doctor must stay open over exactly the state that stops everything
           else, and must not clean tmp -- the staged tree is evidence. */
        *out = c;
        return PKG_OK;
    }
    if (c->unresolved) {
        char det[320];
        snprintf(det, sizeof det,
                 "%d transaction(s) could not be resolved. Nothing was cleaned up. "
                 "Inspect db/transactions and run pkg doctor.", c->unresolved);
        lock_release(c);
        free(c);
        return pkg_fail(e, PKG_E_UNRESOLVED_TXN, "recovery stopped short", det, root);
    }
    if (mode != PKG_OPEN_DOCTOR) {
        u_deltree(c->tmp);
        u_mkdir(c->tmp);
        /* A replacement interrupted after its new package was installed:
           the old one is removed now, as the plan said. */
        rel_finish_replacements(c);
    }
    *out = c;
    return PKG_OK;
}

void pkg_close(pkg_ctx *c)
{
    if (!c) return;
    lock_release(c);
    free(c);
}

void pkg_recovery_counts(const pkg_ctx *c, int *r, int *u)
{ if (r) *r = c->recovered; if (u) *u = c->unresolved; }

void pkgctx_resolved(pkg_ctx *c)   { c->recovered++; }
void pkgctx_unresolved(pkg_ctx *c) { c->unresolved++; }

int pkg_pending_txns(const pkg_ctx *c) { return c->pending; }

int pkg_break_recovery_after = 0;
void pkg_test_break_recovery(int n) { pkg_break_recovery_after = n; }

pkg_fault pkg_injected_fault = PKG_FAIL_NONE;
void pkg_test_fault(pkg_fault f) { pkg_injected_fault = f; }
static int verbose_s;
void pkg_set_verbose(int v) { verbose_s = v; }
int  pkg_verbose(void) { return verbose_s; }
static int keep_rejected_s;
void pkg_set_keep_rejected(int v) { keep_rejected_s = v; }
int  pkg_keep_rejected(void) { return keep_rejected_s; }
void pkg_last_removal(pkg_ctx *c, pkg_removal *out) { *out = c->removal; }

int  pkg_slow_ms;
void pkg_test_slow(int ms) { pkg_slow_ms = ms < 0 ? 0 : ms; net_set_slow(pkg_slow_ms); }
void pkg_test_break_download(long body_bytes, int how) { net_set_break(body_bytes, how); }
