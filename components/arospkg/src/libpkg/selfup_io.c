/* The AROS half of selfup.h: reading an installation from the disk,
 * staging a release, replacing the files and settling an interruption.
 *
 * Measured on AROS 2026.09 (pool v11-3, tests/selfupdate/dirren.c): a
 * running program holds no lock on its file, so its file can be renamed
 * and replaced under it; and a drawer can be renamed while a running
 * program's PROGDIR: is a lock on it, on SFS and on RAM:, with PROGDIR:
 * following it. So apkg replaces its own installation in its own process,
 * including a managed home it was started from. PkgManager cannot do the
 * same for itself, because it has to be started again afterwards, by a
 * process that outlives it: it stages the release, hands the rest to its
 * installation's apkg (su_handoff_save, `apkg self-update --apply`) and
 * quits; apkg waits for it to be gone, replaces the files and starts it
 * again. */
#include "internal.h"
#include "selfup.h"
#include "net.h"
#include "util.h"
#include "sha256.h"
#include "zip.h"
#include "json.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/workbench.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <workbench/workbench.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char *base_s;
void        su_set_base(const char *u) { base_s = u; }
/* SU_TEST_BASE is set only in the labelled test builds of
   tests/selfupdate/make-test-release.sh, so that a PkgManager started from
   its icon, which takes no arguments, can be tested against a controlled
   release; a release build never sets it. */
const char *su_base(void)
{
    if (base_s && *base_s) return base_s;
#ifdef SU_TEST_BASE
    return SU_TEST_BASE;
#else
    return PKG_RELEASE_LATEST;
#endif
}

static char fail_s[PKG_MAXPATH];
void su_test_fail(const char *how) { snprintf(fail_s, sizeof fail_s, "%s", how ? how : ""); }
static int fail_at(const char *kind)
{
    size_t k = strlen(kind);
    if (strncmp(fail_s, kind, k) || fail_s[k] != ':') return 0;
    return atoi(fail_s + k + 1);
}

/* ------------------------------------------------------------- paths */

/* `path` as DOS names it, so two spellings of one file compare equal
   ("C:apkg" and "System:C/apkg"). Returns 0, or -1 when it is not there. */
static int canon(const char *path, char *out, size_t n)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    int ok;
    if (!l) return -1;
    ok = NameFromLock(l, (STRPTR)out, (LONG)n) ? 0 : -1;
    UnLock(l);
    return ok;
}

static void parent_of(const char *path, char *out, size_t n)
{
    const char *fp = (const char *)FilePart((STRPTR)path);
    size_t k = (size_t)(fp - path);
    if (k > 0 && path[k - 1] == '/') k--;
    if (k >= n) k = n - 1;
    memcpy(out, path, k); out[k] = 0;
}

static int same_path(const char *a, const char *b) { return a[0] && !strcasecmp(a, b); }

/* An arospkg drawer: PkgManager in it, or the BUILD.txt of an arospkg
   archive (the builds without PkgManager). */
static int is_home(const char *dir)
{
    char p[PKG_MAXPATH], *b;
    int yes;
    if (!dir[0] || !u_isdir(dir)) return 0;
    u_join(p, sizeof p, dir, "PkgManager");
    if (u_exists(p)) return 1;
    u_join(p, sizeof p, dir, "BUILD.txt");
    b = u_read_all(p, NULL);
    yes = b && !strncmp(b, "arospkg ", 8);
    free(b);
    return yes;
}

/* <root>/arospkg with a registry entry for arospkg in <root>. */
static int is_managed(const char *dir, char *root, size_t n)
{
    char reg[PKG_MAXPATH];
    if (strcasecmp((const char *)FilePart((STRPTR)dir), "arospkg") != 0) return 0;
    parent_of(dir, root, n);
    if (!root[0]) return 0;
    u_join(reg, sizeof reg, root, "db/installed/arospkg.json");
    return u_exists(reg);
}

/* The CPU of an ELF file, and whether it carries this build's ABI tag. */
static int same_target(const char *file, char *why, size_t wn)
{
    size_t len, i, tl;
    char tag[40];
    unsigned char *d = (unsigned char *)u_read_all(file, &len);
    unsigned m, want = !strcmp(pkg_arch(), "x86_64") ? 62 : !strcmp(pkg_arch(), "i386") ? 3 : 183;
    int found = 0;
    if (!d || len < 64 || memcmp(d, "\177ELF", 4)) {
        free(d); snprintf(why, wn, "%s is not an AROS program", file); return -1;
    }
    m = d[18] | (d[19] << 8);
    snprintf(tag, sizeof tag, "arospkg-abi=%s", pkg_abi());
    tl = strlen(tag) + 1;                       /* with its terminating zero */
    for (i = 0; i + tl <= len && !found; i++) if (!memcmp(d + i, tag, tl)) found = 1;
    free(d);
    if (m != want) { snprintf(why, wn, "%s is for another CPU", file); return -1; }
    if (!found) { snprintf(why, wn, "%s is not built for ABI %s", file, pkg_abi()); return -1; }
    return 0;
}

static void file_facts(su_file *f)
{
    size_t len;
    char *d;
    f->sha[0] = f->version[0] = 0;
    if (sha256_file(f->path, f->sha) != 0) f->sha[0] = 0;
    if (f->role != SU_APKG && f->role != SU_PKGMANAGER) return;
    if (!(d = u_read_all(f->path, &len))) return;
    su_version_in(d, len, su_member(f->role), f->version, sizeof f->version);
    free(d);
}

static int add_file(su_install *in, su_role role, const char *path, int copy)
{
    int i;
    char c[PKG_MAXPATH];
    if (canon(path, c, sizeof c) != 0) return -1;
    for (i = 0; i < in->n; i++) if (same_path(in->f[i].path, c)) return 0;
    if (in->n >= SU_MAXFILES) return -1;
    memset(&in->f[in->n], 0, sizeof in->f[0]);
    in->f[in->n].role = role;
    in->f[in->n].copy = copy;
    snprintf(in->f[in->n].path, sizeof in->f[0].path, "%s", c);
    file_facts(&in->f[in->n]);
    in->n++;
    return 0;
}

int su_add_copy(su_install *in, const char *path) { return add_file(in, SU_APKG, path, 1); }

/* ------------------------------------------------------------- record */

/* ENVARC: is the record; the ENV: copy is a courtesy that may lag behind
   a failed write, so it is never read first. */
static char *record_text(void)
{
    char *t = u_read_all(SU_COPIES_FILE, NULL);
    return t ? t : u_read_all("ENV:arospkg/copies.json", NULL);
}

pkg_status su_record(const char *copy, const char *drawer, pkg_err *e)
{
    char *old = record_text(), *now;
    char c[PKG_MAXPATH];
    if (canon(copy, c, sizeof c) != 0) snprintf(c, sizeof c, "%s", copy);
    now = su_copies_set(old, c, drawer);
    free(old);
    if (!now) return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", SU_COPIES_FILE);
    u_mkdir("ENVARC:arospkg"); u_mkdir("ENV:arospkg");
    if (u_write_atomicish(SU_COPIES_FILE, now) != 0) {
        u_delete("ENVARC:arospkg/copies.json.new");
        free(now);
        return pkg_fail(e, PKG_E_IO, "cannot record which installation this apkg belongs to", "", SU_COPIES_FILE);
    }
    if (u_write_atomicish("ENV:arospkg/copies.json", now) != 0) u_delete("ENV:arospkg/copies.json");
    free(now);
    {
        /* Said only when it can be read back. */
        char *back = u_read_all(SU_COPIES_FILE, NULL), d[PKG_MAXPATH];
        int ok = back && su_copies_lookup(back, c, d, sizeof d) && !strcmp(d, drawer);
        free(back);
        if (!ok) return pkg_fail(e, PKG_E_IO, "the choice could not be recorded", "", SU_COPIES_FILE);
    }
    return PKG_OK;
}

/* ------------------------------------------------------- running windows */

/* What a PkgManager puts in the public port list while it runs: its
   drawer, so a self-update can tell whether it is the one being replaced.
   The port takes no messages (PA_IGNORE); only its name and data matter. */
struct su_announce {
    struct MsgPort port;
    char magic[8];
    unsigned long task;            /* the announcing process */
    char nonce[24];
    char drawer[PKG_MAXPATH];
};
static struct su_announce *announced;

static int task_alive(unsigned long addr);

void su_announce_start(const char *drawer, char *nonce, size_t n)
{
    struct su_announce *a = (struct su_announce *)AllocMem(sizeof *a, MEMF_PUBLIC | MEMF_CLEAR);
    if (!a) return;
    a->port.mp_Node.ln_Type = NT_MSGPORT;
    a->port.mp_Node.ln_Name = (char *)SU_ANNOUNCE;
    a->port.mp_Flags = PA_IGNORE;
    NEWLIST(&a->port.mp_MsgList);
    memcpy(a->magic, "arospkg", 8);
    a->task = (unsigned long)(IPTR)FindTask(NULL);
    snprintf(a->nonce, sizeof a->nonce, "%lx-%lx", a->task, (unsigned long)time(NULL));
    snprintf(a->drawer, sizeof a->drawer, "%s", drawer);
    if (nonce) snprintf(nonce, n, "%s", a->nonce);
    AddPort(&a->port);
    announced = a;
}

void su_announce_stop(void)
{
    if (!announced) return;
    RemPort(&announced->port);
    FreeMem(announced, sizeof *announced);
    announced = NULL;
}

/* Under Forbid only copies; no DOS call is made while it holds. An
   announcement whose process is gone (a PkgManager that crashed) is not
   counted: it would refuse every update until a reboot. */
static int announcements(char drawers[][PKG_MAXPATH], char nonces[][24], unsigned long *tasks, int max)
{
    struct Node *nd;
    int k = 0;
    Forbid();
    for (nd = SysBase->PortList.lh_Head; nd->ln_Succ; nd = nd->ln_Succ) {
        struct su_announce *a = (struct su_announce *)nd;
        if (!nd->ln_Name || strcmp(nd->ln_Name, SU_ANNOUNCE) || memcmp(a->magic, "arospkg", 8)) continue;
        if (!task_alive(a->task)) continue;
        if (k < max) {
            if (drawers) snprintf(drawers[k], PKG_MAXPATH, "%s", a->drawer);
            if (nonces) snprintf(nonces[k], 24, "%s", a->nonce);
            if (tasks) tasks[k] = a->task;
        }
        k++;
    }
    Permit();
    return k;
}

/* The last part of a name, without a DOS call: used under Forbid. */
static const char *name_part(const char *s)
{
    const char *p = s + strlen(s);
    while (p > s && p[-1] != '/' && p[-1] != ':') p--;
    return p;
}

#define SU_MAXPROC 16
static void add_task(unsigned long *t, int *n, unsigned long a)
{
    int i;
    for (i = 0; i < *n; i++) if (t[i] == a) return;
    if (*n < SU_MAXPROC) t[(*n)++] = a;
}

/* Processes running PkgManager, each once: started from Workbench a
   process is named after its file (with or without the path); from a
   Shell, the Shell process is named as the command was typed and runs it
   as its command. */
static int pkgmanager_processes(unsigned long *t)
{
    struct Node *n;
    struct Task *me;
    int k = 0;
    LONG i, max;
    Disable();
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (n->ln_Name && !strcasecmp(name_part(n->ln_Name), "PkgManager")) add_task(t, &k, (unsigned long)(IPTR)n);
    for (n = SysBase->TaskWait.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (n->ln_Name && !strcasecmp(name_part(n->ln_Name), "PkgManager")) add_task(t, &k, (unsigned long)(IPTR)n);
    Enable();
    me = FindTask(NULL);
    if (me->tc_Node.ln_Name && !strcasecmp(name_part(me->tc_Node.ln_Name), "PkgManager")) add_task(t, &k, (unsigned long)(IPTR)me);
    Forbid();
    max = MaxCli();
    for (i = 1; i <= max; i++) {
        struct Process *p = FindCliProc((ULONG)i);
        struct CommandLineInterface *cli = p ? (struct CommandLineInterface *)BADDR(p->pr_CLI) : NULL;
        if (cli && cli->cli_Module && cli->cli_CommandName) {
            char name[256];
            CONST_STRPTR b = AROS_BSTR_ADDR(cli->cli_CommandName);
            LONG l = AROS_BSTR_strlen(cli->cli_CommandName);
            if (l > 0 && l < (LONG)sizeof name) {
                memcpy(name, b, (size_t)l); name[l] = 0;
                if (!strcasecmp(name_part(name), "PkgManager")) add_task(t, &k, (unsigned long)(IPTR)p);
            }
        }
    }
    Permit();
    return k;
}

int su_running_pkgmanagers(char drawers[][PKG_MAXPATH], int max, int *unannounced)
{
    unsigned long at[SU_MAXCAND * 2], pt[SU_MAXPROC];
    int a = announcements(drawers, NULL, at, max), p = pkgmanager_processes(pt), i, j, un = 0;
    for (i = 0; i < p; i++) {
        for (j = 0; j < a && j < max && at[j] != pt[i]; j++) ;
        if (j == a || j == max) un++;
    }
    if (unannounced) *unannounced = un;
    return a < max ? a : max;
}

/* ------------------------------------------------------------- locate */

static void read_managed(su_install *in)
{
    char reg[PKG_MAXPATH], *js;
    js_tok *t = NULL; int n;
    u_join(reg, sizeof reg, in->root, "db/installed/arospkg.json");
    if (!(js = u_read_all(reg, NULL))) return;
    n = js_parse_alloc(js, strlen(js), &t, PKG_JSON_MAXTOK);
    if (n > 0) {
        su_jstr(js, t, js_member(js, t, n, 0, "version"), in->managed_version, sizeof in->managed_version);
        in->managed_revision = js_long(js, t, js_member(js, t, n, 0, "revision"), 0);
    }
    free(t); free(js);
}

static void home_files(su_install *in)
{
    static const su_role roles[] = { SU_APKG, SU_PKGMANAGER, SU_BUILD, SU_APPJSON };
    unsigned i;
    for (i = 0; i < sizeof roles / sizeof roles[0]; i++) {
        char p[PKG_MAXPATH];
        u_join(p, sizeof p, in->drawer, su_member(roles[i]));
        if (u_exists(p)) add_file(in, roles[i], p, 0);
    }
}

/* Copies the record ties to this home. */
static void recorded_copies(su_install *in)
{
    char *js = record_text();
    js_tok *t = NULL; int n, arr, el;
    if (!js) return;
    n = js_parse_alloc(js, strlen(js), &t, 4096);
    arr = n > 0 ? js_member(js, t, n, 0, "copies") : -1;
    for (el = arr >= 0 ? js_child(t, n, arr) : -1; el >= 0; el = js_sibling(t, n, el)) {
        char p[PKG_MAXPATH] = "", d[PKG_MAXPATH] = "";
        su_jstr(js, t, js_member(js, t, n, el, "path"), p, sizeof p);
        su_jstr(js, t, js_member(js, t, n, el, "drawer"), d, sizeof d);
        if (same_path(d, in->drawer) && u_exists(p)) add_file(in, SU_APKG, p, 1);
    }
    free(t); free(js);
}

pkg_status su_locate(const char *started, int started_is_apkg, const char *root,
                     const char *chosen, su_install *in, su_where *w, su_facts *f, pkg_err *e)
{
    char *rec;
    memset(in, 0, sizeof *in);
    memset(f, 0, sizeof *f);
    f->started_is_apkg = started_is_apkg;
    if (canon(started, f->started, sizeof f->started) != 0)
        return pkg_fail(e, PKG_E_NOT_FOUND, "cannot find the file this program was started from",
                        "A resident apkg is not updated; start it from its file and try again.", started);
    parent_of(f->started, f->dir, sizeof f->dir);
    f->dir_managed = is_managed(f->dir, f->dir_root, sizeof f->dir_root);
    f->dir_is_home = f->dir_managed || is_home(f->dir);
    if ((rec = record_text())) {
        f->recorded = su_copies_lookup(rec, f->started, f->recorded_drawer, sizeof f->recorded_drawer);
        free(rec);
        if (f->recorded && f->recorded_drawer[0]) {
            f->recorded_ok = is_home(f->recorded_drawer) || is_managed(f->recorded_drawer, f->recorded_root, sizeof f->recorded_root);
            f->recorded_managed = is_managed(f->recorded_drawer, f->recorded_root, sizeof f->recorded_root);
        }
    }
    if (chosen) {
        f->chose = 1;
        if (chosen[0]) {
            if (canon(chosen, f->chosen, sizeof f->chosen) != 0) snprintf(f->chosen, sizeof f->chosen, "%s", chosen);
            f->chosen_managed = is_managed(f->chosen, f->chosen_root, sizeof f->chosen_root);
            f->chosen_ok = f->chosen_managed || is_home(f->chosen);
        }
    }
    if (!f->dir_is_home) {
        /* The candidates a choice is offered between: the package arospkg
           in the root in use, and the drawers running PkgManagers name. */
        char d[PKG_MAXPATH], c[PKG_MAXPATH], run[SU_MAXCAND][PKG_MAXPATH], r2[PKG_MAXPATH];
        int k, i, j;
        u_join(d, sizeof d, root, "arospkg");
        if (u_isdir(d) && canon(d, c, sizeof c) == 0 && is_managed(c, r2, sizeof r2)) {
            snprintf(f->cand[f->ncand], PKG_MAXPATH, "%s", c);
            snprintf(f->cand_how[f->ncand++], 80, "the package arospkg in %s", root);
        }
        k = su_running_pkgmanagers(run, SU_MAXCAND, NULL);
        for (i = 0; i < k && f->ncand < SU_MAXCAND; i++) {
            for (j = 0; j < f->ncand && !same_path(f->cand[j], run[i]); j++) ;
            if (j < f->ncand || !run[i][0]) continue;
            snprintf(f->cand[f->ncand], PKG_MAXPATH, "%s", run[i]);
            snprintf(f->cand_how[f->ncand++], 80, "PkgManager is running from it");
        }
    }
    su_decide_where(f, w);

    snprintf(in->started, sizeof in->started, "%s", f->started);
    snprintf(in->note, sizeof in->note, "%s", w->why);
    if (w->kind == SU_WHERE_HOME || w->kind == SU_WHERE_COPY) {
        in->home = w->home;
        snprintf(in->drawer, sizeof in->drawer, "%s", w->drawer);
        snprintf(in->root, sizeof in->root, "%s", w->root);
        if (in->home == SU_HOME_MANAGED) read_managed(in);
        home_files(in);
        if (w->kind == SU_WHERE_COPY && started_is_apkg) add_file(in, SU_APKG, f->started, 1);
        recorded_copies(in);
    } else if (w->kind == SU_WHERE_ALONE && started_is_apkg) {
        add_file(in, SU_APKG, f->started, 1);
    }
    return PKG_OK;
}

int su_unrecorded_c_apkg(const su_install *in, char *path, size_t n, char *version, size_t vn)
{
    char c[PKG_MAXPATH], why[200], d[PKG_MAXPATH], *rec;
    su_file f;
    int i, known = 0;
    if (canon("C:apkg", c, sizeof c) != 0) return 0;
    for (i = 0; i < in->n; i++) if (same_path(in->f[i].path, c)) return 0;
    if ((rec = record_text())) { known = su_copies_lookup(rec, c, d, sizeof d); free(rec); }
    if (known || same_target(c, why, sizeof why) != 0) return 0;
    memset(&f, 0, sizeof f);
    f.role = SU_APKG;
    snprintf(f.path, sizeof f.path, "%s", c);
    file_facts(&f);
    snprintf(path, n, "%s", c);
    snprintf(version, vn, "%s", f.version[0] ? f.version : "unknown version");
    return 1;
}

/* ------------------------------------------------------------- release */

pkg_status su_release(const char *base, const char *sums_file, pkg_release *r, pkg_err *e)
{
    return pkg_release_check(base ? base : su_base(), sums_file, "", r, e);
}

/* ------------------------------------------------------------- staging */

typedef struct { pkg_progress cb; void *user; int cancelled; } prog_shim;

static void net_shim(void *user, unsigned long done, unsigned long total, int *cancel)
{
    prog_shim *s = (prog_shim *)user;
    pkg_progress_ev ev;
    int c = 0;
    if (!s->cb) return;
    ev.phase = "download"; ev.id = "arospkg"; ev.done = done; ev.total = total; ev.can_cancel = 1;
    s->cb(s->user, &ev, &c);
    if (c) { s->cancelled = 1; *cancel = 1; }
}

static int ask_cancel(prog_shim *s, const char *phase, int can)
{
    pkg_progress_ev ev;
    int c = 0;
    if (!s->cb) return 0;
    ev.phase = phase; ev.id = "arospkg"; ev.can_cancel = can;
    ev.done = ev.total = strcmp(phase, "download") ? 1 : 0;     /* nothing received yet */
    s->cb(s->user, &ev, &c);
    return can && c;
}

/* A new file can be made in `dir` and renamed there: what the swap does. */
static int dir_takes_files(const char *dir, char *why, size_t wn)
{
    char a[PKG_MAXPATH], b[PKG_MAXPATH];
    FILE *fp;
    u_join(a, sizeof a, dir, ".arospkg-write-test");
    u_join(b, sizeof b, dir, ".arospkg-write-test.old");
    u_delete(a); u_delete(b);
    if (!(fp = fopen(a, "wb")) || fputs("arospkg\n", fp) == EOF) {
        if (fp) fclose(fp);
        u_delete(a);
        snprintf(why, wn, "cannot write in %s", dir); return -1;
    }
    if (fclose(fp) != 0) { u_delete(a); snprintf(why, wn, "cannot write in %s", dir); return -1; }
    if (!Rename((CONST_STRPTR)a, (CONST_STRPTR)b)) { u_delete(a); snprintf(why, wn, "cannot rename files in %s", dir); return -1; }
    if (u_delete(b) != 0) { snprintf(why, wn, "cannot delete files in %s", dir); return -1; }
    return 0;
}

/* An existing file the swap has to delete (an older .old or .new). The
   protection bits are active-low: FIBF_DELETE set forbids deleting. */
static int deletable(const char *path)
{
    struct FileInfoBlock *fib;
    BPTR l;
    int ok = 1;
    if (!u_exists(path)) return 1;
    if (!(fib = AllocDosObject(DOS_FIB, NULL))) return 0;
    if ((l = Lock((CONST_STRPTR)path, SHARED_LOCK))) {
        if (Examine(l, fib) && (fib->fib_Protection & FIBF_DELETE)) ok = 0;
        UnLock(l);
    }
    FreeDosObject(DOS_FIB, fib);
    return ok;
}

static long free_bytes(const char *dir)
{
    struct InfoData *id = (struct InfoData *)AllocVec(sizeof *id, MEMF_PUBLIC | MEMF_CLEAR);
    BPTR l;
    long b = -1;
    if (!id) return -1;
    if ((l = Lock((CONST_STRPTR)dir, SHARED_LOCK))) {
        if (Info(l, id)) {
            double f = (double)(id->id_NumBlocks - id->id_NumBlocksUsed) * (double)id->id_BytesPerBlock;
            b = f > 2e9 ? 2000000000L : (long)f;
        }
        UnLock(l);
    }
    FreeVec(id);
    return b;
}

static void staged_path(const su_stage *st, su_role r, char *out, size_t n)
{
    char d[PKG_MAXPATH];
    u_join(d, sizeof d, st->dir, "new");
    u_join(out, n, d, su_member(r));
}

pkg_status su_stage_release(su_install *in, const pkg_release *r, const char *base,
                            const char *stage_dir, pkg_progress cb, void *user,
                            su_stage *st, pkg_err *e)
{
    char why[240], zwhy[160], member[300], build[PKG_MAXPATH], newdir[PKG_MAXPATH];
    char want_apkg[65] = "", want_pm[65] = "", got[65];
    prog_shim shim;
    int i, have[4] = { 0, 0, 0, 0 };
    long need = 0;

    memset(st, 0, sizeof *st);
    memset(&shim, 0, sizeof shim);
    shim.cb = cb; shim.user = user;
    if (r->found != 1)
        return pkg_fail(e, PKG_E_NOT_FOUND, r->found ? "the release lists more than one archive for this system"
                                                     : "the latest stable release has no archive for this CPU and ABI",
                        "", r->target);
    snprintf(st->dir, sizeof st->dir, "%s", stage_dir);
    snprintf(st->version, sizeof st->version, "%s", r->version);
    snprintf(st->target, sizeof st->target, "%s", r->target);
    snprintf(st->archive, sizeof st->archive, "%s", r->archive);
    snprintf(st->sha, sizeof st->sha, "%s", r->sha256);
    snprintf(st->url, sizeof st->url, "%s%s", base ? base : su_base(), r->archive);
    u_deltree(stage_dir);
    u_join(newdir, sizeof newdir, stage_dir, "new");
    if (u_mkpath(newdir) != 0 || !u_isdir(newdir))
        return pkg_fail(e, PKG_E_IO, "cannot make a directory for the download", "", stage_dir);
    u_join(st->zip, sizeof st->zip, stage_dir, st->archive);

    /* 1. The archive, complete, and the sum the release names for it. */
    if (ask_cancel(&shim, "download", 1)) return pkg_fail(e, PKG_E_CANCELLED, "cancelled before the download", "Nothing was changed.", "");
    if (net_open(why) != 0) return pkg_fail(e, PKG_E_NETWORK, "no network", why, st->url);
    net_set_progress(cb ? net_shim : NULL, &shim);
    i = net_fetch(st->url, st->zip, 32L * 1024 * 1024, why);
    net_set_progress(NULL, NULL);
    net_close();
    {
        char part[PKG_MAXPATH + 8];
        snprintf(part, sizeof part, "%s.part", st->zip);
        if (i != 0) {
            u_delete(part);
            if (shim.cancelled || !strcmp(why, "cancelled"))
                return pkg_fail(e, PKG_E_CANCELLED, "download cancelled", "Nothing was changed.", st->url);
            return pkg_fail(e, PKG_E_NETWORK, "cannot fetch the release archive", why, st->url);
        }
        if (!Rename((CONST_STRPTR)part, (CONST_STRPTR)st->zip)) { u_delete(part); return pkg_fail(e, PKG_E_IO, "cannot write the download", "", st->zip); }
    }
    st->size = u_size(st->zip);
    if (sha256_file(st->zip, got) != 0 || strcmp(got, st->sha) != 0)
        return pkg_fail(e, PKG_E_VERIFY, "the archive does not match the release's checksum",
                        "Nothing was changed. The download may be damaged; try again later.", st->zip);
    if (ask_cancel(&shim, "verify", 1)) return pkg_fail(e, PKG_E_CANCELLED, "cancelled after verification", "Nothing was changed.", "");

    /* 2. The programs' own sums, from the archive's BUILD.txt. */
    snprintf(member, sizeof member, "arospkg-%s.%s/BUILD.txt", st->version, st->target);
    staged_path(st, SU_BUILD, build, sizeof build);
    if (zip_extract_member(st->zip, member, build, 65536, zwhy) != 0)
        return pkg_fail(e, PKG_E_ARCHIVE, "the archive has no BUILD.txt", zwhy, st->zip);
    {
        char *bt = u_read_all(build, NULL), *l;
        for (l = bt; l && *l; ) {
            char sha[80], fn[64];
            if (sscanf(l, "%79s %63s", sha, fn) == 2 && strlen(sha) == 64) {
                if (!strcmp(fn, "apkg")) snprintf(want_apkg, sizeof want_apkg, "%s", sha);
                if (!strcmp(fn, "PkgManager")) snprintf(want_pm, sizeof want_pm, "%s", sha);
            }
            l = strchr(l, '\n'); if (l) l++;
        }
        free(bt);
    }
    if (!want_apkg[0]) return pkg_fail(e, PKG_E_ARCHIVE, "BUILD.txt in the archive does not name apkg's checksum", "", st->zip);

    /* 3. Each file this installation has, unpacked and checked. */
    for (i = 0; i < in->n; i++) have[in->f[i].role] = 1;
    have[SU_BUILD] = 1;
    for (i = 0; i < 4; i++) {
        char dest[PKG_MAXPATH];
        if (!have[i]) continue;
        staged_path(st, (su_role)i, dest, sizeof dest);
        if (i != SU_BUILD) {
            snprintf(member, sizeof member, "arospkg-%s.%s/%s", st->version, st->target, su_member((su_role)i));
            u_delete(dest);
            zwhy[0] = 0;
            if (zip_extract_member(st->zip, member, dest, 32L * 1024 * 1024, zwhy) != 0 || !u_exists(dest)) {
                /* Only a file the release really lacks is left as it is: a
                   full T: or a damaged member is a failure, and a program
                   BUILD.txt names must be there. */
                int absent = !zwhy[0];
                u_delete(dest);
                if (!absent || i == SU_APKG || (i == SU_PKGMANAGER && want_pm[0]))
                    return pkg_fail(e, PKG_E_ARCHIVE, "cannot unpack a file of the release",
                                    zwhy[0] ? zwhy : su_member((su_role)i), st->zip);
                have[i] = 0; continue;
            }
        }
        if (sha256_file(dest, got) != 0)
            return pkg_fail(e, PKG_E_IO, "cannot read an unpacked file", "", dest);
        if ((i == SU_APKG && strcmp(got, want_apkg)) || (i == SU_PKGMANAGER && (!want_pm[0] || strcmp(got, want_pm))))
            return pkg_fail(e, PKG_E_VERIFY, "an unpacked program does not match BUILD.txt", su_member((su_role)i), st->zip);
        if ((i == SU_APKG || i == SU_PKGMANAGER) && same_target(dest, why, sizeof why) != 0)
            return pkg_fail(e, PKG_E_VERIFY, "the release's program is not for this machine", why, st->zip);
        {
            int k;
            for (k = 0; k < in->n; k++)
                if ((int)in->f[k].role == i) snprintf(in->f[k].want_sha, sizeof in->f[k].want_sha, "%s", got);
        }
    }
    st->nreplace = su_plan(in, st->version, why, sizeof why);
    if (st->nreplace < 0) return pkg_fail(e, PKG_E_CONFLICT, "not updated", why, "");
    st->managed = su_managed_behind(in, st->version);

    /* 4. Before anything is closed or replaced: every place a file goes
       takes a new file and a rename, nothing in the way is protected, and
       there is room. */
    for (i = 0; i < in->n; i++) {
        su_file *f = &in->f[i];
        char dir[PKG_MAXPATH], p[PKG_MAXPATH + 8], dest[PKG_MAXPATH];
        if (f->act != SU_REPLACE || (in->home == SU_HOME_MANAGED && !f->copy)) continue;
        parent_of(f->path, dir, sizeof dir);
        if (dir_takes_files(dir, why, sizeof why) != 0)
            return pkg_fail(e, PKG_E_IO, "a file of this installation cannot be replaced", why, f->path);
        snprintf(p, sizeof p, "%s.old", f->path);
        if (!deletable(p)) return pkg_fail(e, PKG_E_IO, "the previous backup is protected from deletion",
                                           "Unprotect it (Protect <file> +d) or delete it, then try again.", p);
        snprintf(p, sizeof p, "%s.new", f->path);
        if (!deletable(p)) return pkg_fail(e, PKG_E_IO, "a file in the way is protected from deletion", "", p);
        staged_path(st, f->role, dest, sizeof dest);
        need = u_size(dest) + 65536;
        if (free_bytes(dir) >= 0 && free_bytes(dir) < need)
            return pkg_fail(e, PKG_E_IO, "not enough room for the new files", "", dir);
    }
    if (in->home == SU_HOME_MANAGED) {
        char c[PKG_MAXPATH];
        u_join(c, sizeof c, in->root, "cache");
        if (dir_takes_files(c, why, sizeof why) != 0)
            return pkg_fail(e, PKG_E_IO, "the package root cannot take the new archive", why, in->root);
        if (free_bytes(c) >= 0 && free_bytes(c) < 3 * st->size + 65536)
            return pkg_fail(e, PKG_E_IO, "not enough room in the package root", "", in->root);
    }
    return PKG_OK;
}

/* ------------------------------------------------------------- locks */

/* One self-update at a time: a file in ENV:, which a reboot clears, made
   with Rename's create-if-absent as the root's lock is. A holder that is
   no longer in the system left it behind; that one is taken over. */
static char lock_nonce[40];

static int task_alive(unsigned long addr)
{
    struct Node *n;
    int yes = 0;
    if (addr == (unsigned long)(IPTR)FindTask(NULL)) return 1;
    Disable();
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ && !yes; n = n->ln_Succ) if ((unsigned long)(IPTR)n == addr) yes = 1;
    for (n = SysBase->TaskWait.lh_Head; n->ln_Succ && !yes; n = n->ln_Succ) if ((unsigned long)(IPTR)n == addr) yes = 1;
    Enable();
    return yes;
}

static pkg_status su_lock(pkg_err *e)
{
    char tmp[80], body[120];
    int tries;
    FILE *f;
    u_mkdir("ENV:arospkg");
    snprintf(lock_nonce, sizeof lock_nonce, "%lx%lx", (unsigned long)(IPTR)FindTask(NULL), (unsigned long)time(NULL));
    snprintf(tmp, sizeof tmp, "ENV:arospkg/self-update.%s", lock_nonce);
    snprintf(body, sizeof body, "%lx %s\n", (unsigned long)(IPTR)FindTask(NULL), lock_nonce);
    for (tries = 0; tries < 2; tries++) {
        char *have;
        unsigned long holder = 0;
        if (!(f = fopen(tmp, "wb"))) return pkg_fail(e, PKG_E_IO, "cannot write the self-update lock", "", tmp);
        fputs(body, f); fclose(f);
        if (Rename((CONST_STRPTR)tmp, (CONST_STRPTR)SU_LOCK_FILE)) return PKG_OK;
        u_delete(tmp);
        have = u_read_all(SU_LOCK_FILE, NULL);
        if (have) holder = strtoul(have, NULL, 16);
        free(have);
        if (holder && task_alive(holder))
            return pkg_fail(e, PKG_E_LOCKED, "another update of arospkg is running",
                            "Wait for it to finish, then try again.", SU_LOCK_FILE);
        {
            /* Its holder is gone. Taken aside by rename, which only one
               process can win; if what was taken aside is a live holder's
               (it was taken over meanwhile), it is put back. */
            char aside[80];
            snprintf(aside, sizeof aside, "ENV:arospkg/self-update.stale.%s", lock_nonce);
            if (Rename((CONST_STRPTR)SU_LOCK_FILE, (CONST_STRPTR)aside)) {
                unsigned long now = 0;
                have = u_read_all(aside, NULL);
                if (have) now = strtoul(have, NULL, 16);
                free(have);
                if (now && task_alive(now)) {
                    Rename((CONST_STRPTR)aside, (CONST_STRPTR)SU_LOCK_FILE);
                    return pkg_fail(e, PKG_E_LOCKED, "another update of arospkg is running",
                                    "Wait for it to finish, then try again.", SU_LOCK_FILE);
                }
                u_delete(aside);
            }
        }
    }
    return pkg_fail(e, PKG_E_LOCKED, "cannot take the self-update lock", "", SU_LOCK_FILE);
}

static void su_unlock(void)
{
    char *have = u_read_all(SU_LOCK_FILE, NULL);
    if (have && strstr(have, lock_nonce)) u_delete(SU_LOCK_FILE);
    free(have);
}

/* The roots an update must hold: the one in use, and a managed home's
   when it is another. An operation holding either refuses the update. */
typedef struct { pkg_ctx *c[2]; int n; } roots;

static pkg_status take_roots(roots *rs, const char *root, const char *managed_root, pkg_err *e)
{
    const char *want[2];
    char ca[PKG_MAXPATH] = "", cb[PKG_MAXPATH] = "";
    int i, n = 0;
    memset(rs, 0, sizeof *rs);
    if (managed_root && managed_root[0]) want[n++] = managed_root;
    if (root && root[0] && u_isdir(root)) {
        if (!(n && canon(root, ca, sizeof ca) == 0 && canon(managed_root, cb, sizeof cb) == 0 && same_path(ca, cb)))
            want[n++] = root;
    }
    for (i = 0; i < n; i++) {
        pkg_status st = pkg_open(want[i], PKG_OPEN_WRITE, &rs->c[rs->n], e);
        if (st != PKG_OK) {
            while (rs->n) pkg_close(rs->c[--rs->n]);
            if (st == PKG_E_LOCKED)
                snprintf(e->detail, sizeof e->detail, "An apkg or PkgManager operation is running in %s. "
                         "Try again when it has finished. Nothing was changed.", want[i]);
            return st;
        }
        rs->n++;
    }
    return PKG_OK;
}

static void drop_roots(roots *rs) { while (rs->n) pkg_close(rs->c[--rs->n]); }

/* ------------------------------------------------------------- apply */

static long protection_of(const char *path)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR l;
    long p = 0;
    if (!fib) return 0;
    if ((l = Lock((CONST_STRPTR)path, SHARED_LOCK))) { if (Examine(l, fib)) p = fib->fib_Protection; UnLock(l); }
    FreeDosObject(DOS_FIB, fib);
    return p;
}

static int file_is(const char *path, const char *sha)
{
    char got[65];
    return sha && sha[0] && u_exists(path) && sha256_file(path, got) == 0 && !strcmp(got, sha);
}

/* <path> back to what it was, from <path>.old: the new file goes back to
   <path>.new, so nothing is lost if the old one cannot be had. */
static int undo_file(const su_jfile *f)
{
    char o[PKG_MAXPATH + 8], nw[PKG_MAXPATH + 8];
    snprintf(o, sizeof o, "%s.old", f->path);
    snprintf(nw, sizeof nw, "%s.new", f->path);
    if (file_is(f->path, f->from_sha)) return 0;
    if (!f->from_sha[0]) return u_exists(f->path) ? Rename((CONST_STRPTR)f->path, (CONST_STRPTR)nw) ? 0 : -1 : 0;
    if (!file_is(o, f->from_sha)) return -1;
    if (u_exists(f->path)) {
        u_delete(nw);
        if (!Rename((CONST_STRPTR)f->path, (CONST_STRPTR)nw)) return -1;
    }
    return Rename((CONST_STRPTR)o, (CONST_STRPTR)f->path) ? 0 : -1;
}

static int do_file(const su_jfile *f)
{
    char o[PKG_MAXPATH + 8], nw[PKG_MAXPATH + 8];
    snprintf(o, sizeof o, "%s.old", f->path);
    snprintf(nw, sizeof nw, "%s.new", f->path);
    if (file_is(f->path, f->to_sha)) return 0;
    if (!file_is(nw, f->to_sha)) return -1;
    if (u_exists(f->path)) {
        if (u_exists(o) && u_delete(o) != 0) return -1;
        if (!Rename((CONST_STRPTR)f->path, (CONST_STRPTR)o)) return -1;
    }
    return Rename((CONST_STRPTR)nw, (CONST_STRPTR)f->path) ? 0 : -1;
}

static void scan_journal(su_journal *j)
{
    int i;
    for (i = 0; i < j->n; i++) {
        su_jfile *f = &j->f[i];
        char p[PKG_MAXPATH + 8];
        f->now = !u_exists(f->path) ? SU_J_ABSENT : file_is(f->path, f->to_sha) ? SU_J_TO
               : file_is(f->path, f->from_sha) ? SU_J_FROM : SU_J_OTHER;
        snprintf(p, sizeof p, "%s.new", f->path); f->have_new = file_is(p, f->to_sha);
        snprintf(p, sizeof p, "%s.old", f->path); f->have_old = file_is(p, f->from_sha);
    }
}

static void clean_news(const su_journal *j)
{
    int i;
    for (i = 0; i < j->n; i++) {
        char p[PKG_MAXPATH + 8];
        snprintf(p, sizeof p, "%s.new", j->f[i].path);
        u_delete(p);
    }
}

static int write_journal(const su_journal *j)
{
    char *t = su_journal_text(j);
    int rc;
    if (!t) return -1;
    u_mkdir("ENVARC:arospkg");
    rc = u_write_atomicish(SU_JOURNAL_FILE, t);
    free(t);
    return rc;
}

int su_journal_pending(void) { return u_exists(SU_JOURNAL_FILE); }

/* The registry's version of a managed home now. */
static int managed_at(const char *root, const char *version)
{
    su_install tmp;
    memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.root, sizeof tmp.root, "%s", root);
    read_managed(&tmp);
    return tmp.managed_version[0] && !strcmp(tmp.managed_version, version);
}

static int running_from(const su_install *in, int *unannounced)
{
    char run[SU_MAXCAND][PKG_MAXPATH];
    int n = su_running_pkgmanagers(run, SU_MAXCAND, unannounced), i, k = 0;
    for (i = 0; i < n; i++) if (in->drawer[0] && same_path(run[i], in->drawer)) k++;
    return k;
}

pkg_status su_precheck(const su_install *in, const char *root, int from_window, pkg_err *e)
{
    const char *roots[2];
    int i, un = 0, mine;
    char *have;
    roots[0] = root;
    roots[1] = in->home == SU_HOME_MANAGED ? in->root : "";
    for (i = 0; i < 2; i++) {
        char lk[PKG_MAXPATH + 16];
        if (!roots[i] || !roots[i][0]) continue;
        u_join(lk, sizeof lk, roots[i], "db/lock.owner");
        if (u_exists(lk))
            return pkg_fail(e, PKG_E_LOCKED, "an apkg or PkgManager operation is running",
                            "Try again when it has finished. Nothing was changed.", roots[i]);
    }
    if ((have = u_read_all(SU_LOCK_FILE, NULL))) {
        unsigned long holder = strtoul(have, NULL, 16);
        free(have);
        if (holder && task_alive(holder))
            return pkg_fail(e, PKG_E_LOCKED, "another update of arospkg is running", "Wait for it to finish.", "");
    }
    mine = running_from(in, &un);
    if (mine > (from_window ? 1 : 0))
        return pkg_fail(e, PKG_E_LOCKED, from_window ? "another PkgManager runs from this installation"
                                                     : "PkgManager is running from this installation",
                        from_window ? "Close the other PkgManager windows first. Nothing was changed."
                                    : "Close it first, or use Update arospkg in its About window. Nothing was changed.",
                        in->drawer);
    if (un && in->home != SU_HOME_NONE)
        return pkg_fail(e, PKG_E_LOCKED, "a PkgManager is running",
                        "Close it first: an older PkgManager does not say where it runs from. Nothing was changed.", "");
    return PKG_OK;
}

pkg_status su_apply(su_install *in, const su_stage *st, const char *root, pkg_err *e)
{
    su_journal j;
    roots rs;
    pkg_status s;
    int i, k, managed_swap, done = 0;
    char why[300];

    memset(&j, 0, sizeof j);
    managed_swap = st->managed;
    if (!st->nreplace && !managed_swap) return PKG_OK;

    if (!strncmp(fail_s, "hold:", 5)) {
        /* Testing only: wait, staged, until the file exists, so another
           operation can be made to happen between the plan and the swap. */
        int t;
        for (t = 0; t < 600 * 50 && !u_exists(fail_s + 5); t += 25) Delay(25);
    }
    if ((s = su_lock(e)) != PKG_OK) return s;
    if (su_journal_pending()) {
        /* Another update stopped halfway since this one was planned: its
           journal is all that can settle it, and is never written over. */
        su_unlock();
        return pkg_fail(e, PKG_E_UNRESOLVED_TXN, "an earlier update of arospkg did not finish",
                        "Nothing was changed. Settle it first: apkg self-update --recover, or Repair now "
                        "in PkgManager; then update again.", SU_JOURNAL_FILE);
    }
    if ((s = take_roots(&rs, root, in->home == SU_HOME_MANAGED ? in->root : NULL, e)) != PKG_OK) { su_unlock(); return s; }
    if (in->home == SU_HOME_MANAGED) {
        /* The plan was made before the download. Under the root's lock now:
           the package must still be what was planned for, or an upgrade
           that ran meanwhile would be undone. */
        su_install now;
        memset(&now, 0, sizeof now);
        snprintf(now.root, sizeof now.root, "%s", in->root);
        read_managed(&now);
        for (i = 0; i < in->n; i++)
            if (!in->f[i].copy && in->f[i].sha[0] && !file_is(in->f[i].path, in->f[i].sha)) break;
        if (strcmp(now.managed_version, in->managed_version) || now.managed_revision != in->managed_revision || i < in->n) {
            char now_v[80];
            snprintf(now_v, sizeof now_v, "%s%s", now.managed_version[0] ? now.managed_version : "not installed",
                     now.managed_revision ? " (another revision)" : "");
            s = pkg_fail(e, PKG_E_CONFLICT, "the package arospkg changed since the update was planned",
                         "Nothing was changed. Check for a newer version again.", now_v);
            goto out;
        }
    }
    {
        /* A window running from this installation would keep its old code
           and later find new files under it: it is closed first. Asked
           again under the locks; su_precheck asked before the download. */
        int un = 0;
        if (running_from(in, &un) || (un && in->home != SU_HOME_NONE)) {
            s = pkg_fail(e, PKG_E_LOCKED, "PkgManager is running from this installation",
                         "Close it first, or use Update arospkg in its About window. Nothing was changed.", in->drawer);
            goto out;
        }
    }

    /* 1. Every new file beside its target, checked. Nothing is renamed yet. */
    snprintf(j.from_version, sizeof j.from_version, "%s", in->managed_version);
    snprintf(j.to_version, sizeof j.to_version, "%s", st->version);
    if (managed_swap) { snprintf(j.root, sizeof j.root, "%s", in->root); snprintf(j.managed_to, sizeof j.managed_to, "%s", st->version); }
    for (i = 0, k = 0; i < in->n; i++) {
        su_file *f = &in->f[i];
        char src[PKG_MAXPATH], nw[PKG_MAXPATH + 8];
        if (f->act != SU_REPLACE || (in->home == SU_HOME_MANAGED && !f->copy)) continue;
        if (!j.from_version[0] && f->version[0]) snprintf(j.from_version, sizeof j.from_version, "%s", f->version);
        if (!file_is(f->path, f->sha)) {
            /* Checked before the download, and in a window minutes ago:
               a file that changed since is not what the plan was made for. */
            for (; i >= 0; i--) { char p[PKG_MAXPATH + 8]; snprintf(p, sizeof p, "%s.new", in->f[i].path); if (in->f[i].act == SU_REPLACE) u_delete(p); }
            s = pkg_fail(e, PKG_E_CONFLICT, "a file changed since it was checked", "Nothing was changed. Try again.", f->path);
            goto out;
        }
        staged_path(st, f->role, src, sizeof src);
        snprintf(nw, sizeof nw, "%s.new", f->path);
        u_delete(nw);
        if (++k == fail_at("write") || u_copy(src, nw) != 0 || !file_is(nw, f->want_sha)) {
            u_delete(nw);
            for (; i >= 0; i--) { char p[PKG_MAXPATH + 8]; snprintf(p, sizeof p, "%s.new", in->f[i].path); if (in->f[i].act == SU_REPLACE) u_delete(p); }
            s = pkg_fail(e, PKG_E_IO, "cannot write the new file", "Nothing was changed.", nw);
            goto out;
        }
        /* The new file gets the old one's protection bits, so it stays runnable. */
        SetProtection((CONST_STRPTR)nw, protection_of(f->path));
        snprintf(j.f[j.n].path, sizeof j.f[0].path, "%s", f->path);
        snprintf(j.f[j.n].from_sha, sizeof j.f[0].from_sha, "%s", f->sha);
        snprintf(j.f[j.n].to_sha, sizeof j.f[0].to_sha, "%s", f->want_sha);
        j.n++;
    }
    if (write_journal(&j) != 0) {
        clean_news(&j);
        s = pkg_fail(e, PKG_E_IO, "cannot write the update's journal", "Nothing was changed.", SU_JOURNAL_FILE);
        goto out;
    }

    /* 2. The renames. A failure puts back what was done; a stop leaves the
       journal for recovery, as a machine that stopped here would. */
    for (i = 0; i < j.n; i++) {
        if (i + 1 == fail_at("rename") || do_file(&j.f[i]) != 0) {
            snprintf(why, sizeof why, "cannot put the new %s in place", j.f[i].path);
            goto undo;
        }
        done = i + 1;
        if (done == fail_at("stop")) {
            /* As a machine that stopped here and was started again: the
               journal and the files stay; the locks are what a reboot and
               apkg unlock would clear. */
            printf("(test) stopped after replacing %s\n", j.f[i].path);
            fflush(stdout);
            drop_roots(&rs); su_unlock();
            exit(20);
        }
    }

    /* 3. A managed home: its own transaction, which decides. */
    if (managed_swap) {
        char arc[PKG_MAXPATH], got[65];
        pkg_ctx *mc = rs.c[0];
        u_join(arc, sizeof arc, in->root, "cache/arospkg.new.zip");
        u_delete(arc);
        if (u_copy(st->zip, arc) != 0 || sha256_file(arc, got) != 0 || strcmp(got, st->sha) != 0) {
            u_delete(arc);
            snprintf(why, sizeof why, "cannot put the release archive in %s/cache", in->root);
            goto undo;
        }
        {
            char subdir[200];
            snprintf(subdir, sizeof subdir, "arospkg-%s.%s", st->version, st->target);
            s = pkg_selfupdate_swap(mc, "arospkg", arc, st->version, st->url, st->sha, st->size, subdir,
                                    in->managed_version, in->managed_revision, (pkg_stop)fail_at("managed"), e);
        }
        if (s != PKG_OK) {
            if (s == PKG_E_INTERRUPTED) {      /* testing: as if the machine stopped */
                printf("(test) stopped inside the package transaction\n");
                fflush(stdout);
                drop_roots(&rs); su_unlock();
                exit(20);
            }
            u_delete(arc);
            snprintf(why, sizeof why, "the package arospkg could not be replaced: %s", e->summary);
            {
                /* Its transaction may still be open (swap.c leaves it for the
                   next run). Opening the root again runs that recovery now;
                   the copies go back only once the package is back. */
                pkg_err e2;
                memset(&e2, 0, sizeof e2);
                pkg_close(rs.c[0]); rs.c[0] = NULL;
                if (pkg_open(in->root, PKG_OPEN_WRITE, &rs.c[0], &e2) != PKG_OK) rs.c[0] = NULL;
                if (!rs.c[0] || !managed_at(in->root, in->managed_version) || !u_isdir(in->drawer)) {
                    s = pkg_fail(e, PKG_E_UNRESOLVED_TXN, why,
                                 "The update's journal is kept. Run apkg self-update again: it finishes "
                                 "the package's recovery and then puts the other files back.", in->root);
                    goto out;
                }
            }
            goto undo;
        }
    }

    /* 4. Only now is it done: every file, and a managed home's registry. */
    for (i = 0; i < j.n; i++)
        if (!file_is(j.f[i].path, j.f[i].to_sha)) { snprintf(why, sizeof why, "%s does not match after the update", j.f[i].path); goto undo; }
    if (managed_swap && !managed_at(in->root, st->version)) {
        s = pkg_fail(e, PKG_E_UNRESOLVED_TXN, "the package record does not say the new version",
                     "Run apkg self-update again; it finishes or undoes the update.", in->root);
        goto out;                       /* the journal stays for recovery */
    }
    clean_news(&j);
    u_delete(SU_JOURNAL_FILE);
    s = PKG_OK;
    goto out;

undo:
    {
        int bad = 0;
        for (i = done; i >= 0; i--) if (i < j.n && undo_file(&j.f[i]) != 0) bad = 1;
        if (bad) {
            char steps[900];
            scan_journal(&j);
            su_manual_steps(&j, steps, sizeof steps);
            s = pkg_fail(e, PKG_E_UNRESOLVED_TXN, why, steps, SU_JOURNAL_FILE);
        } else {
            clean_news(&j);
            u_delete(SU_JOURNAL_FILE);
            s = pkg_fail(e, PKG_E_IO, why, "The previous version is back; nothing was changed.", "");
        }
    }
out:
    drop_roots(&rs);
    su_unlock();
    return s;
}

/* ------------------------------------------------------------- recover */

pkg_status su_recover(int *found, char *steps, size_t n, pkg_err *e)
{
    su_journal j;
    char *text;
    roots rs;
    pkg_status s;
    int managed = -1, d, i, bad = 0;

    *found = 0;
    steps[0] = 0;
    /* Read and decided under the lock: an update that starts meanwhile
       refuses while the journal is there (su_apply). */
    if ((s = su_lock(e)) != PKG_OK) return s;
    if (!(text = u_read_all(SU_JOURNAL_FILE, NULL))) { su_unlock(); return PKG_OK; }
    *found = 1;
    if (su_journal_parse(text, &j) != 0) {
        free(text);
        su_unlock();
        snprintf(steps, n, "The journal %s cannot be read. Check apkg and PkgManager with "
                 "\"apkg --version\" and their drawer, then delete the journal.", SU_JOURNAL_FILE);
        return pkg_fail(e, PKG_E_UNRESOLVED_TXN, "an interrupted update of arospkg cannot be settled", steps, SU_JOURNAL_FILE);
    }
    free(text);
    /* Opening a managed home's root runs its own recovery first, which
       settles its transaction; its registry then says which way to go. */
    if ((s = take_roots(&rs, NULL, j.root, e)) != PKG_OK) { su_unlock(); return s; }
    if (j.root[0]) managed = managed_at(j.root, j.managed_to) ? 1 : 0;
    scan_journal(&j);
    d = su_recover_decide(&j, managed, steps, n);
    for (i = 0; i < j.n && !bad; i++) {
        if (d == SU_REC_FORWARD && do_file(&j.f[i]) != 0) bad = 1;
        if (d == SU_REC_BACKWARD && undo_file(&j.f[i]) != 0) bad = 1;
    }
    if (bad) {
        scan_journal(&j);
        d = su_recover_decide(&j, managed, steps, n);
        if (d != SU_REC_DONE) { d = SU_REC_STUCK; su_manual_steps(&j, steps, n); }
    }
    if (d == SU_REC_STUCK) {
        s = pkg_fail(e, PKG_E_UNRESOLVED_TXN, "an interrupted update of arospkg cannot be settled", steps, SU_JOURNAL_FILE);
    } else {
        snprintf(steps, n, "%s", d == SU_REC_FORWARD ? "finished: the new version is in place"
                               : d == SU_REC_BACKWARD ? "undone: the previous version is back"
                               : "nothing was left to do");
        clean_news(&j);
        u_delete(SU_JOURNAL_FILE);
        s = PKG_OK;
    }
    drop_roots(&rs);
    su_unlock();
    return s;
}

/* ------------------------------------------------------------- hand-off */

pkg_status su_handoff_save(const char *file, const su_install *in, const su_stage *st,
                           const su_restart *rs, pkg_err *e)
{
    size_t cap = 8192 + (size_t)in->n * (PKG_MAXPATH + 400);
    char *buf = (char *)malloc(cap);
    js_out o;
    int i;
    if (!buf) return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", file);
    js_out_init(&o, buf, cap);
    js_raw(&o, "{\n  "); js_key(&o, "protocol"); js_vlong(&o, SU_PROTOCOL);
    js_raw(&o, ",\n  "); js_key(&o, "home");     js_vlong(&o, in->home);
    js_raw(&o, ",\n  "); js_key(&o, "drawer");   js_vstr(&o, in->drawer);
    js_raw(&o, ",\n  "); js_key(&o, "root");     js_vstr(&o, in->root);
    js_raw(&o, ",\n  "); js_key(&o, "managed_version"); js_vstr(&o, in->managed_version);
    js_raw(&o, ",\n  "); js_key(&o, "managed_revision"); js_vlong(&o, in->managed_revision);
    js_raw(&o, ",\n  "); js_key(&o, "started");  js_vstr(&o, in->started);
    js_raw(&o, ",\n  "); js_key(&o, "files");    js_raw(&o, " [");
    for (i = 0; i < in->n; i++) {
        const su_file *f = &in->f[i];
        js_raw(&o, i ? ",\n    { " : "\n    { ");
        js_key(&o, "role"); js_vlong(&o, f->role); js_raw(&o, ", ");
        js_key(&o, "copy"); js_vlong(&o, f->copy); js_raw(&o, ", ");
        js_key(&o, "act");  js_vlong(&o, f->act);  js_raw(&o, ", ");
        js_key(&o, "path"); js_vstr(&o, f->path);  js_raw(&o, ", ");
        js_key(&o, "version"); js_vstr(&o, f->version); js_raw(&o, ", ");
        js_key(&o, "sha");  js_vstr(&o, f->sha);   js_raw(&o, ", ");
        js_key(&o, "want"); js_vstr(&o, f->want_sha); js_raw(&o, " }");
    }
    js_raw(&o, "\n  ],\n  "); js_key(&o, "stage"); js_raw(&o, " { ");
    js_key(&o, "dir");     js_vstr(&o, st->dir);     js_raw(&o, ", ");
    js_key(&o, "zip");     js_vstr(&o, st->zip);     js_raw(&o, ", ");
    js_key(&o, "url");     js_vstr(&o, st->url);     js_raw(&o, ", ");
    js_key(&o, "version"); js_vstr(&o, st->version); js_raw(&o, ", ");
    js_key(&o, "target");  js_vstr(&o, st->target);  js_raw(&o, ", ");
    js_key(&o, "archive"); js_vstr(&o, st->archive); js_raw(&o, ", ");
    js_key(&o, "sha");     js_vstr(&o, st->sha);     js_raw(&o, ", ");
    js_key(&o, "size");    js_vlong(&o, st->size);   js_raw(&o, ", ");
    js_key(&o, "nreplace"); js_vlong(&o, st->nreplace); js_raw(&o, ", ");
    js_key(&o, "managed"); js_vlong(&o, st->managed); js_raw(&o, " }");
    js_raw(&o, ",\n  "); js_key(&o, "restart"); js_raw(&o, " { ");
    js_key(&o, "wb");      js_vlong(&o, rs->wb);     js_raw(&o, ", ");
    js_key(&o, "program"); js_vstr(&o, rs->program); js_raw(&o, ", ");
    js_key(&o, "args");    js_vstr(&o, rs->args);    js_raw(&o, ", ");
    {
        char t[24];
        snprintf(t, sizeof t, "%lx", rs->task);
        js_key(&o, "task"); js_vstr(&o, t); js_raw(&o, ", ");
        snprintf(t, sizeof t, "%lx", rs->module);
        js_key(&o, "module"); js_vstr(&o, t); js_raw(&o, ", ");
    }
    js_key(&o, "nonce");   js_vstr(&o, rs->nonce);   js_raw(&o, ", ");
    js_key(&o, "root");    js_vstr(&o, rs->root);    js_raw(&o, " }\n}\n");
    if (o.fail || u_write_atomicish(file, buf) != 0) {
        free(buf);
        return pkg_fail(e, PKG_E_IO, "cannot write the update's hand-off", "", file);
    }
    free(buf);
    return PKG_OK;
}

pkg_status su_handoff_load(const char *file, su_install *in, su_stage *st, su_restart *rs, pkg_err *e)
{
    char *js = u_read_all(file, NULL), t24[24];
    js_tok *t = NULL;
    int n, arr, el, s, r;
    memset(in, 0, sizeof *in); memset(st, 0, sizeof *st); memset(rs, 0, sizeof *rs);
    if (!js) return pkg_fail(e, PKG_E_NOT_FOUND, "the update's hand-off is not there", "", file);
    n = js_parse_alloc(js, strlen(js), &t, 8192);
    if (n <= 0 || js_long(js, t, js_member(js, t, n, 0, "protocol"), 0) != SU_PROTOCOL) {
        free(t); free(js);
        return pkg_fail(e, PKG_E_CONFLICT, "this apkg does not understand PkgManager's hand-off",
                        "Update from the Shell with: apkg self-update", file);
    }
    in->home = (su_home)js_long(js, t, js_member(js, t, n, 0, "home"), 0);
    su_jstr(js, t, js_member(js, t, n, 0, "drawer"), in->drawer, sizeof in->drawer);
    su_jstr(js, t, js_member(js, t, n, 0, "root"), in->root, sizeof in->root);
    su_jstr(js, t, js_member(js, t, n, 0, "managed_version"), in->managed_version, sizeof in->managed_version);
    in->managed_revision = js_long(js, t, js_member(js, t, n, 0, "managed_revision"), 0);
    su_jstr(js, t, js_member(js, t, n, 0, "started"), in->started, sizeof in->started);
    arr = js_member(js, t, n, 0, "files");
    for (el = arr >= 0 ? js_child(t, n, arr) : -1; el >= 0 && in->n < SU_MAXFILES; el = js_sibling(t, n, el)) {
        su_file *f = &in->f[in->n++];
        f->role = (su_role)js_long(js, t, js_member(js, t, n, el, "role"), 0);
        f->copy = (int)js_long(js, t, js_member(js, t, n, el, "copy"), 0);
        f->act  = (su_act)js_long(js, t, js_member(js, t, n, el, "act"), 0);
        su_jstr(js, t, js_member(js, t, n, el, "path"), f->path, sizeof f->path);
        su_jstr(js, t, js_member(js, t, n, el, "version"), f->version, sizeof f->version);
        su_jstr(js, t, js_member(js, t, n, el, "sha"), f->sha, sizeof f->sha);
        su_jstr(js, t, js_member(js, t, n, el, "want"), f->want_sha, sizeof f->want_sha);
    }
    s = js_member(js, t, n, 0, "stage");
    su_jstr(js, t, js_member(js, t, n, s, "dir"), st->dir, sizeof st->dir);
    su_jstr(js, t, js_member(js, t, n, s, "zip"), st->zip, sizeof st->zip);
    su_jstr(js, t, js_member(js, t, n, s, "url"), st->url, sizeof st->url);
    su_jstr(js, t, js_member(js, t, n, s, "version"), st->version, sizeof st->version);
    su_jstr(js, t, js_member(js, t, n, s, "target"), st->target, sizeof st->target);
    su_jstr(js, t, js_member(js, t, n, s, "archive"), st->archive, sizeof st->archive);
    su_jstr(js, t, js_member(js, t, n, s, "sha"), st->sha, sizeof st->sha);
    st->size = js_long(js, t, js_member(js, t, n, s, "size"), -1);
    st->nreplace = (int)js_long(js, t, js_member(js, t, n, s, "nreplace"), 0);
    st->managed = (int)js_long(js, t, js_member(js, t, n, s, "managed"), 0);
    r = js_member(js, t, n, 0, "restart");
    rs->wb = (int)js_long(js, t, js_member(js, t, n, r, "wb"), 0);
    su_jstr(js, t, js_member(js, t, n, r, "program"), rs->program, sizeof rs->program);
    su_jstr(js, t, js_member(js, t, n, r, "args"), rs->args, sizeof rs->args);
    t24[0] = 0;
    su_jstr(js, t, js_member(js, t, n, r, "task"), t24, sizeof t24);
    rs->task = strtoul(t24, NULL, 16);
    t24[0] = 0;
    su_jstr(js, t, js_member(js, t, n, r, "module"), t24, sizeof t24);
    rs->module = strtoul(t24, NULL, 16);
    su_jstr(js, t, js_member(js, t, n, r, "nonce"), rs->nonce, sizeof rs->nonce);
    su_jstr(js, t, js_member(js, t, n, r, "root"), rs->root, sizeof rs->root);
    free(t); free(js);
    if (st->zip[0]) {
        /* The staged files are checked again here: they waited in T: while
           the window closed, and only what is checked is installed. */
        int i;
        char got[65];
        if (sha256_file(st->zip, got) != 0 || strcmp(got, st->sha) != 0)
            return pkg_fail(e, PKG_E_VERIFY, "the downloaded archive changed after it was checked", "Nothing was changed.", st->zip);
        for (i = 0; i < in->n; i++) {
            char p[PKG_MAXPATH];
            if (in->f[i].act != SU_REPLACE) continue;
            staged_path(st, in->f[i].role, p, sizeof p);
            if (!file_is(p, in->f[i].want_sha))
                return pkg_fail(e, PKG_E_VERIFY, "a downloaded file changed after it was checked", "Nothing was changed.", p);
        }
    }
    return PKG_OK;
}

/* The PkgManager is still in its process: from Workbench the process is
   its own and ends with it; from a Shell the process is the Shell's and
   goes on, so there the program is gone once the Shell has unloaded it. */
static int program_running(const su_restart *rs)
{
    int yes;
    Forbid();
    yes = task_alive(rs->task);
    if (yes && rs->module) {
        struct Process *p = (struct Process *)rs->task;
        struct CommandLineInterface *cli = (struct CommandLineInterface *)BADDR(p->pr_CLI);
        yes = cli && (unsigned long)(IPTR)cli->cli_Module == rs->module;
    }
    Permit();
    return yes;
}

int su_wait_gone(const su_restart *rs, int seconds)
{
    int ticks;
    for (ticks = 0; ticks < seconds * 50; ticks += 10) {
        char nonces[8][24];
        int k = announcements(NULL, nonces, NULL, 8), i, there = 0;
        for (i = 0; i < k && i < 8; i++) if (!strcmp(nonces[i], rs->nonce)) there = 1;
        if (!there && !(rs->task && program_running(rs))) return 0;
        Delay(10);
    }
    return -1;
}

pkg_status su_restart_pkgmanager(const su_restart *rs, pkg_err *e)
{
    char drawer[PKG_MAXPATH], run[SU_MAXCAND][PKG_MAXPATH];
    int before, ticks, i, k;
    parent_of(rs->program, drawer, sizeof drawer);
    if (!u_exists(rs->program)) return pkg_fail(e, PKG_E_LAUNCH, "PkgManager is not there to start", "", rs->program);
    before = 0;
    k = su_running_pkgmanagers(run, SU_MAXCAND, NULL);
    for (i = 0; i < k; i++) if (same_path(run[i], drawer)) before++;
    if (rs->wb) {
        /* As from its icon: the icon's stack and tool types apply. */
        struct Library *WorkbenchBase = OpenLibrary((CONST_STRPTR)"workbench.library", 44);
        struct Process *me = (struct Process *)FindTask(NULL);
        BPTR home, t, old = BNULL;
        BOOL ok;
        if (!WorkbenchBase) return pkg_fail(e, PKG_E_LAUNCH, "there is no desktop to start it with", "", rs->program);
        /* Neither this process's home drawer (the installation) nor its
           current directory may be handed on (~/Work/AROS/CLAUDE.md). */
        t = Lock((CONST_STRPTR)(u_exists("T:") ? "T:" : "RAM:"), SHARED_LOCK);
        home = me->pr_HomeDir; me->pr_HomeDir = BNULL;
        if (t) old = CurrentDir(t);
        ok = OpenWorkbenchObjectA((STRPTR)rs->program, NULL);
        if (t) { CurrentDir(old); UnLock(t); }
        me->pr_HomeDir = home;
        CloseLibrary(WorkbenchBase);
        if (!ok) return pkg_fail(e, PKG_E_LAUNCH, "Workbench refused to start PkgManager", "", rs->program);
    } else {
        char cmd[PKG_MAXPATH + 1100];
        struct Process *me = (struct Process *)FindTask(NULL);
        BPTR wd, old, home, in, out;
        LONG rc;
        {
            /* Quoted as ReadArgs reads it back: '*' escapes. */
            size_t k = 0; const char *p;
            cmd[k++] = '"';
            for (p = rs->program; *p && k + 4 < sizeof cmd; p++) {
                if (*p == '"' || *p == '*') cmd[k++] = '*';
                cmd[k++] = *p;
            }
            snprintf(cmd + k, sizeof cmd - k, "\" %s", rs->args);
        }
        if (!(wd = Lock((CONST_STRPTR)drawer, SHARED_LOCK))) return pkg_fail(e, PKG_E_LAUNCH, "cannot enter PkgManager's drawer", "", drawer);
        in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
        out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
        /* The new process would get a copy of this one's home drawer and
           keep it locked for as long as it runs (~/Work/AROS/CLAUDE.md). */
        home = me->pr_HomeDir; me->pr_HomeDir = BNULL;
        old = CurrentDir(wd);
        rc = SystemTags((CONST_STRPTR)cmd, SYS_Input, (IPTR)in, SYS_Output, (IPTR)out, SYS_Asynch, TRUE,
                        NP_StackSize, 262144, TAG_DONE);
        CurrentDir(old);
        me->pr_HomeDir = home;
        UnLock(wd);
        if (rc == -1) {
            if (in) Close(in);
            if (out) Close(out);
            return pkg_fail(e, PKG_E_LAUNCH, "the Shell could not start PkgManager", "", rs->program);
        }
    }
    /* Started means seen: a PkgManager announcing this drawer. */
    for (ticks = 0; ticks < 30 * 50; ticks += 10) {
        int now = 0;
        k = su_running_pkgmanagers(run, SU_MAXCAND, NULL);
        for (i = 0; i < k; i++) if (same_path(run[i], drawer)) now++;
        if (now > before) return PKG_OK;
        Delay(10);
    }
    return pkg_fail(e, PKG_E_LAUNCH, "PkgManager was not seen running within 30 seconds",
                    "Start it from its drawer.", rs->program);
}

/* ------------------------------------------------------------- result */

void su_result_write(const su_result *r)
{
    char buf[1400];
    js_out o;
    js_out_init(&o, buf, sizeof buf);
    js_raw(&o, "{ "); js_key(&o, "ok"); js_vlong(&o, r->ok);
    js_raw(&o, ", "); js_key(&o, "changed"); js_vlong(&o, r->changed);
    js_raw(&o, ", "); js_key(&o, "from"); js_vstr(&o, r->from);
    js_raw(&o, ", "); js_key(&o, "to"); js_vstr(&o, r->to);
    js_raw(&o, ", "); js_key(&o, "summary"); js_vstr(&o, r->summary);
    js_raw(&o, ", "); js_key(&o, "detail"); js_vstr(&o, r->detail);
    js_raw(&o, " }\n");
    if (o.fail) return;
    u_mkdir("ENV:arospkg");
    u_write_atomicish(SU_RESULT_FILE, buf);
}

int su_result_take(su_result *r)
{
    char *js = u_read_all(SU_RESULT_FILE, NULL);
    js_tok *t = NULL;
    int n;
    memset(r, 0, sizeof *r);
    if (!js) return 0;
    n = js_parse_alloc(js, strlen(js), &t, 256);
    if (n > 0) {
        r->ok = (int)js_long(js, t, js_member(js, t, n, 0, "ok"), 0);
        r->changed = (int)js_long(js, t, js_member(js, t, n, 0, "changed"), 0);
        su_jstr(js, t, js_member(js, t, n, 0, "from"), r->from, sizeof r->from);
        su_jstr(js, t, js_member(js, t, n, 0, "to"), r->to, sizeof r->to);
        su_jstr(js, t, js_member(js, t, n, 0, "summary"), r->summary, sizeof r->summary);
        su_jstr(js, t, js_member(js, t, n, 0, "detail"), r->detail, sizeof r->detail);
    }
    free(t); free(js);
    u_delete(SU_RESULT_FILE);
    return n > 0;
}
