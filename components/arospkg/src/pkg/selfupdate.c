/* apkg self-update: the Shell's front of libpkg/selfup.h, which updates
 * apkg and the PkgManager that goes with it to the same release.
 * docs/self-update.md is the user's side.
 *
 * Where the release comes from: GitHub's releases/latest/download/, which
 * names the latest STABLE release (prereleases never), and in it
 * SHA256SUMS and arospkg-<version>.<target>.zip, the names
 * tools/make-rc.sh gives them.
 *
 * The same command finishes what PkgManager starts (--apply): the window
 * stages the release, writes a hand-off and quits; this program, from the
 * installation's own drawer, waits for it to be gone, replaces the files
 * and starts it again. */
#include "selfupdate.h"
#include "present.h"
#include "../libpkg/pkg.h"
#include "../libpkg/selfup.h"
#include "../libpkg/util.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <intuition/intuition.h>
#include <dos/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *sums_s;
void su_test_from(const char *u) { su_set_base(u); }
void su_test_sums(const char *f) { sums_s = f; }

/* The file this process was loaded from: its PROGDIR: and its name. Never
   another copy found on the command path. */
static int self_path(char *out, size_t n)
{
    char name[256];
    if (!GetProgramName((STRPTR)name, sizeof name) || !name[0]) return -1;
    if (!GetProgramDir() || !NameFromLock(GetProgramDir(), (STRPTR)out, (LONG)n)) return -1;
    if (!AddPart((STRPTR)out, FilePart((STRPTR)name), (LONG)n)) return -1;
    return u_exists(out) ? 0 : -1;
}

static int checking;                     /* --check: the wording of a failure */

static int fail(const char *what, const char *detail)
{
    char head[400];
    snprintf(head, sizeof head, "Could not %s: %s", checking ? "check for a newer arospkg" : "update arospkg", what);
    fputs(pr_color(PR_RED), stdout);
    pr_text(head, 0);
    fputs(pr_off(), stdout);
    if (detail && *detail) pr_text(detail, 2);
    return 10;
}

static int fail_e(const pkg_err *e) { return fail(e->summary, e->detail); }

/* Ctrl-C cancels while the library still takes it: the download and the
   checks. Once files move, the break waits until they are settled. */
static void on_progress(void *user, const pkg_progress_ev *ev, int *cancel)
{
    (void)user;
    pr_progress(ev->id, ev->phase, ev->done, ev->total);
    if (ev->can_cancel && (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C)) {
        SetSignal(0L, SIGBREAKF_CTRL_C);
        *cancel = 1;
    }
}

static const char *home_words(const su_install *in, char *buf, size_t n)
{
    if (in->home == SU_HOME_MANAGED)
        snprintf(buf, n, "%s (package arospkg %s in %s)", in->drawer,
                 in->managed_version[0] ? in->managed_version : "?", in->root);
    else if (in->home == SU_HOME_DRAWER) snprintf(buf, n, "%s", in->drawer);
    else snprintf(buf, n, "none: apkg on its own, without PkgManager");
    return buf;
}

/* One line per file: role, the version it says, where, and what happens. */
static void show_files(const su_install *in, const char *release, int planned)
{
    int i;
    for (i = 0; i < in->n; i++) {
        const su_file *f = &in->f[i];
        const char *state;
        if (!planned && f->role != SU_APKG && f->role != SU_PKGMANAGER) continue;
        state = planned ? (f->act == SU_REPLACE ? "update" : "keep") : su_state_word(f, release);
        printf("  %-16s %-14s %-7s %s\n", su_role_name(f->role),
               f->role == SU_APKG || f->role == SU_PKGMANAGER ? (f->version[0] ? f->version : "?") : "",
               state, f->path);
    }
}

static void list_candidates(const su_facts *f)
{
    int i;
    printf("This apkg (%s) is a copy, and it is not recorded which installation it belongs to.\n", f->started);
    for (i = 0; i < f->ncand; i++) printf("  %d  %s (%s)\n", i + 1, f->cand[i], f->cand_how[i]);
    printf("  0  none: update only this apkg\n");
}

/* A copy with no record and somewhere it could belong: asked in a window;
   in a script, told how to answer. Returns the chosen drawer ("" = on its
   own), or NULL when nothing was chosen. */
static const char *choose(const su_facts *f, char *buf, size_t n)
{
    char ans[16];
    int k;
    list_candidates(f);
    if (!IsInteractive(Input())) {
        pr_text("Choose with --drawer <drawer> or --apkg-only and run it again; the choice is recorded. "
                "Nothing was changed.", 0);
        return NULL;
    }
    printf("Which installation is it part of? [0-%d, Return cancels] ", f->ncand);
    fflush(stdout);
    if (!fgets(ans, sizeof ans, stdin) || ans[0] < '0' || ans[0] > '9') { pr_text("Nothing was changed.", 0); return NULL; }
    k = atoi(ans);
    if (k < 0 || k > f->ncand) { pr_text("Nothing was changed.", 0); return NULL; }
    snprintf(buf, n, "%s", k ? f->cand[k - 1] : "");
    return buf;
}

static int yes_no(const char *q)
{
    char ans[16];
    printf("%s [y/N] ", q);
    fflush(stdout);
    return fgets(ans, sizeof ans, stdin) && (ans[0] == 'y' || ans[0] == 'Y');
}

/* Settle an earlier update that did not finish. Returns 0 when nothing is
   in the way any more. */
static int recover_first(void)
{
    char steps[1200];
    int found;
    pkg_err e;
    memset(&e, 0, sizeof e);
    if (!su_journal_pending()) return 0;
    printf("An earlier update of arospkg did not finish.\n");
    if (su_recover(&found, steps, sizeof steps, &e) != PKG_OK) {
        if (e.status != PKG_E_UNRESOLVED_TXN) return fail_e(&e);
        fail(e.summary, "");
        printf("To repair it by hand:\n%sThen delete %s.\n", steps, SU_JOURNAL_FILE);
        return 10;
    }
    printf("  %s\n\n", steps);
    return 0;
}

static void stage_name(char *out, size_t n)
{
    snprintf(out, n, "%sarospkg-update-%lx", u_exists("T:") ? "T:" : "RAM:",
             (unsigned long)(IPTR)FindTask(NULL) ^ (unsigned long)time(NULL));
}

int apkg_self_update(const char *current, const char *root, const su_opts *o)
{
    char self[PKG_MAXPATH], buf[PKG_MAXPATH + 200], chosen_buf[PKG_MAXPATH], stage[PKG_MAXPATH];
    const char *chosen = o->drawer;
    su_install in;
    su_where w;
    su_facts f;
    su_stage st;
    pkg_release r;
    pkg_err e;
    int rc, i;

    (void)current;
    memset(&e, 0, sizeof e);
    checking = o->check;
    if (o->protocol) { printf("arospkg-self-update %d\n", SU_PROTOCOL); return 0; }
    if (o->apply) return apkg_self_update_apply(o->apply);
    if (o->recover) {
        if (!su_journal_pending()) { printf("No unfinished update of arospkg.\n"); return 0; }
        return recover_first();
    }
    if (!pkg_release_target())
        return fail("this build's CPU and ABI have no release archive",
                    "Update it by hand from https://github.com/tomaszstaniak/arospkg/releases");
    if (self_path(self, sizeof self) != 0)
        return fail("cannot find the file this apkg was started from",
                    "A resident apkg is not updated; start it from its file and try again.");
    if (o->check) {
        if (su_journal_pending()) printf("An earlier update of arospkg did not finish: apkg self-update settles it.\n\n");
    } else if ((rc = recover_first()) != 0) return rc;

    if (su_locate(self, 1, root, chosen, &in, &w, &f, &e) != PKG_OK) return fail_e(&e);
    if (w.kind == SU_WHERE_BROKEN) return fail("cannot tell which installation this apkg belongs to", w.why);
    if (w.kind == SU_WHERE_CHOOSE) {
        if (o->check) {
            list_candidates(&f);
            printf("apkg self-update asks which one; nothing else is checked until then.\n");
            return 0;
        }
        if (!(chosen = choose(&f, chosen_buf, sizeof chosen_buf))) return 5;
        if (su_locate(self, 1, root, chosen, &in, &w, &f, &e) != PKG_OK) return fail_e(&e);
        if (w.kind == SU_WHERE_BROKEN) return fail("cannot use that installation", w.why);
    }
    if (w.record && !o->check) {
        if (su_record(self, w.kind == SU_WHERE_ALONE ? "" : w.drawer, &e) != PKG_OK) return fail_e(&e);
        printf("Recorded: %s %s.\n\n", self, w.kind == SU_WHERE_ALONE ? "is used on its own"
                                                                       : "belongs to that installation");
    }
    if (!o->check && in.home != SU_HOME_NONE) {
        /* C:apkg is where the release README puts a copy: a known place,
           asked about once, never searched for anywhere else. */
        char cp[PKG_MAXPATH], cv[48];
        if (su_unrecorded_c_apkg(&in, cp, sizeof cp, cv, sizeof cv)) {
            printf("%s (%s) is another apkg on this machine, not recorded with any installation.\n", cp, cv);
            if (IsInteractive(Input())) {
                int y = yes_no("Update it together with this installation?");
                if (su_record(cp, y ? in.drawer : "", &e) != PKG_OK) {
                    fail_e(&e);
                    printf("Nothing was changed.\n");
                    return 10;
                }
                if (y && su_add_copy(&in, cp) != 0) return fail("cannot read the copy", cp);
                printf("Recorded: %s %s.\n\n", cp, y ? "belongs to this installation" : "is used on its own");
            } else
                printf("It is left as it is. Answer in a Shell window, or tie it to this installation with\n"
                       "  \"%s\" self-update --drawer \"%s\"\n\n", cp, in.drawer);
        }
    }

    if (su_release(NULL, sums_s, &r, &e) != PKG_OK) return fail_e(&e);
    pr_heading("arospkg");
    pr_field("Started from", self, -1);
    pr_field("Installation", home_words(&in, buf, sizeof buf), -1);
    if (r.found == 1) snprintf(buf, sizeof buf, "%s (%s)", r.version, r.target);
    else snprintf(buf, sizeof buf, "%s, with no archive for %s", r.other_version[0] ? r.other_version : "unknown version", r.target);
    pr_field("Latest stable release", buf, -1);
    if (w.kind == SU_WHERE_ALONE) pr_field("PkgManager", w.why, -1);
    putchar('\n');
    show_files(&in, r.version, 0);
    putchar('\n');
    if (r.found != 1) {
        if (o->check && !r.found) {
            /* Not a failure of the question: the answer is that there is
               nothing newer for this machine. */
            printf("No newer arospkg for this machine. This one stays as it is.\n");
            return 0;
        }
        return fail(r.found ? "the release lists more than one archive for this system"
                            : "the latest stable release has no archive for this CPU and ABI", r.target);
    }
    if (o->check) {
        int older = su_managed_behind(&in, r.version), newer = 0;
        for (i = 0; i < in.n; i++) {
            const char *s = su_state_word(&in.f[i], r.version);
            if (in.f[i].role != SU_APKG && in.f[i].role != SU_PKGMANAGER) continue;
            if (!strcmp(s, "older") || !strcmp(s, "unknown")) older++;
            if (!strcmp(s, "newer")) newer++;
        }
        if (newer) printf("This installation has a build newer than the latest stable release.\n");
        else if (older) printf("A newer release is available. Install it with: apkg self-update\n");
        else { pr_word(PR_GREEN, "Up to date"); putchar('\n'); }
        return 0;
    }

    if (su_all_current(&in, r.version)) { pr_word(PR_GREEN, "Up to date"); putchar('\n'); return 0; }
    if (su_precheck(&in, root, 0, &e) != PKG_OK) return fail_e(&e);
    stage_name(stage, sizeof stage);
    if (su_stage_release(&in, &r, NULL, stage, on_progress, NULL, &st, &e) != PKG_OK) {
        u_deltree(stage);
        return e.status == PKG_E_CANCELLED ? (fail_e(&e), 5) : fail_e(&e);
    }
    if (!st.nreplace && !st.managed) {
        u_deltree(stage);
        pr_word(PR_GREEN, "Up to date"); printf(": every file already is the release's.\n");
        return 0;
    }
    putchar('\n');
    snprintf(buf, sizeof buf, "Updating arospkg to %s", st.version);
    pr_heading(buf);
    show_files(&in, st.version, 1);
    if (st.managed)
        printf("  The package arospkg in %s is replaced as an upgrade (%s -> %s).\n",
               in.root, in.managed_version, st.version);
    printf("  Icons and settings are kept.\n");
    rc = su_apply(&in, &st, root, &e);
    u_deltree(stage);
    SetSignal(0L, SIGBREAKF_CTRL_C);       /* a break during the swap waited for it */
    if (rc != PKG_OK) {
        fail_e(&e);
        if (e.status == PKG_E_UNRESOLVED_TXN) printf("Run apkg self-update again to finish or undo it.\n");
        return 10;
    }
    putchar('\n');
    pr_word(PR_GREEN, "Updated"); printf(" arospkg to %s\n", st.version);
    for (i = 0; i < in.n; i++)
        if (in.f[i].act == SU_REPLACE && (in.home != SU_HOME_MANAGED || in.f[i].copy))
            printf("  %s (previous version: %s.old)\n", in.f[i].path, in.f[i].path);
    if (st.managed) printf("  %s (previous version: apkg rollback arospkg)\n", in.drawer);
    /* This process is still the old code; only the files have changed. */
    pr_text("The new version runs from the next apkg command.", 2);
    return 0;
}

/* ---------------------------------------------------- PkgManager's hand-off */

/* Said in a requester: this process has no console of its own. */
static void tell(const char *text)
{
    struct Library *IntuitionBase = OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    struct EasyStruct es;
    IPTR args[1];
    if (!IntuitionBase) return;
    es.es_StructSize = sizeof es; es.es_Flags = 0;
    es.es_Title = (CONST_STRPTR)"arospkg update";
    es.es_TextFormat = (CONST_STRPTR)"%s";
    es.es_GadgetFormat = (CONST_STRPTR)"OK";
    /* EasyRequestArgs, not the varargs EasyRequest: on some SDKs that one
       is a link-library stub using a global IntuitionBase. */
    args[0] = (IPTR)text;
    EasyRequestArgs(NULL, &es, NULL, (RAWARG)args);
    CloseLibrary(IntuitionBase);
}

int apkg_self_update_apply(const char *handoff)
{
    su_install in;
    su_stage st;
    su_restart rs;
    su_result res;
    pkg_err e, re;
    pkg_status s;
    char steps[1200] = "", text[1400];
    int found = 0, i;

    memset(&e, 0, sizeof e); memset(&re, 0, sizeof re); memset(&res, 0, sizeof res);
    s = su_handoff_load(handoff, &in, &st, &rs, &e);
    if (!rs.program[0]) {
        snprintf(text, sizeof text, "The update of arospkg could not start: %s\n%s", e.summary, e.detail);
        tell(text);
        return 10;
    }
    /* The window first: it closes, its worker ends, its announcement goes. */
    if (su_wait_gone(&rs, 60) != 0) {
        tell("PkgManager did not close within a minute, so nothing was changed.");
        return 10;
    }
    if (s == PKG_OK && su_journal_pending()) {
        /* A journal here was written after the window planned (it refuses
           to plan while one exists): another update stopped halfway. It is
           settled, and this plan, made before it, is not applied. */
        if (su_recover(&found, steps, sizeof steps, &e) != PKG_OK) s = e.status;
        else if (st.version[0]) {
            s = e.status = PKG_E_CONFLICT;
            snprintf(e.summary, sizeof e.summary, "an earlier update had not finished, and has been settled (%s)", steps);
            snprintf(e.detail, sizeof e.detail, "Nothing else was changed. Check for a newer version and update again.");
        }
    }
    if (s == PKG_OK) s = su_apply(&in, &st, rs.root, &e);
    if (st.dir[0]) u_deltree(st.dir);
    u_delete(handoff);

    res.ok = s == PKG_OK;
    res.changed = s == PKG_OK && (st.nreplace || st.managed || found);
    snprintf(res.to, sizeof res.to, "%s", st.version);
    for (i = 0; i < in.n && !res.from[0]; i++)
        if (in.f[i].role == SU_PKGMANAGER && in.f[i].version[0]) snprintf(res.from, sizeof res.from, "%s", in.f[i].version);
    if (s == PKG_OK && !st.version[0])
        snprintf(res.summary, sizeof res.summary, "The unfinished update of arospkg was settled: %s.",
                 found ? steps : "nothing was left to do");
    else if (s == PKG_OK) {
        snprintf(res.summary, sizeof res.summary, "arospkg was updated to %s.", st.version);
        snprintf(res.detail, sizeof res.detail, "%s", in.home == SU_HOME_MANAGED
                 ? "The previous version can be restored with: apkg rollback arospkg"
                 : "The previous files are kept beside the new ones, named .old.");
    } else {
        snprintf(res.summary, sizeof res.summary, "arospkg was not updated: %s", e.summary);
        snprintf(res.detail, sizeof res.detail, "%s%s", e.status == PKG_E_UNRESOLVED_TXN ? "To repair it by hand:\n" : "",
                 e.status == PKG_E_UNRESOLVED_TXN && steps[0] ? steps : e.detail);
    }
    su_result_write(&res);
    if (su_restart_pkgmanager(&rs, &re) != PKG_OK) {
        /* Told apart from the update itself: the files are what res says. */
        snprintf(text, sizeof text, "%s\n%s\n\nPkgManager could not be started again: %s\nStart it from %s",
                 res.summary, res.detail, re.summary, rs.program);
        tell(text);
        return s == PKG_OK ? 5 : 10;
    }
    return s == PKG_OK ? 0 : 10;
}
