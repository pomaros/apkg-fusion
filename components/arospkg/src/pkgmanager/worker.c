/* The worker: a DOS process of its own, because pkg_install does file and
 * network I/O and those are per-process state -- Open(), Lock(), the current
 * directory and bsdsocket.library all live in the Process, not the Task. A
 * bare CreateTask() would fail inside libpkg in ways that look like bugs in
 * libpkg. See docs/zune-frontend-brief.md, "The execution contract".
 *
 * It opens its own pkg_ctx (the root lock belongs to whoever opened it),
 * runs one operation, and reports through the window's message port. It
 * never touches a MUI object: the callback copies the event into a message
 * and the window's own loop does the drawing. */
#include "job.h"
#include "../libpkg/util.h"
#include <stdio.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <stdlib.h>
#include <string.h>

static job *current;
#define WORKER_NAME "PkgManager worker"

/* Testing only, passed through from --slow: see pkg_test_slow. The knob is
   a static in libpkg, so the worker process sets it for itself. */
static int slow_ms;
void job_set_slow(int ms) { slow_ms = ms; }

/* Testing only, from --test-nomem start: the final message's allocation
   fails, as it would with no memory left. */
static int test_nomem_start;
void job_test_nomem(void) { test_nomem_start = 1; }

static void send(job *j, pm_msg *m)
{
    m->msg.mn_Length = sizeof *m;
    m->msg.mn_ReplyPort = NULL;
    PutMsg(j->reply, &m->msg);
}

static void on_progress(void *user, const pkg_progress_ev *ev, int *cancel)
{
    job *j = (job *)user;
    pm_msg *m = (pm_msg *)AllocVec(sizeof *m, MEMF_PUBLIC | MEMF_CLEAR);
    if (m) {
        m->kind = PM_PROGRESS;
        m->seq = j->seq;
        strncpy(m->phase, ev->phase ? ev->phase : "", sizeof m->phase - 1);
        strncpy(m->id, ev->id ? ev->id : "", sizeof m->id - 1);
        m->done = ev->done; m->total = ev->total; m->can_cancel = ev->can_cancel;
        send(j, m);
    }
    /* Polled by the library, not signalled: the window sets the flag and the
       next event carries it back. Whether it is honoured is the library's
       decision, per pkg.h. */
    *cancel = j->cancel;
}

static void worker_main(void)
{
    job *j = current;
    pkg_ctx *c = NULL;
    pkg_err e;
    pkg_status st;
    pm_msg *m;
    pkg_plan plan;

    memset(&e, 0, sizeof e);
    memset(&plan, 0, sizeof plan);
    pkg_test_slow(slow_ms);
    /* The release check reads the network and nothing in the root: no
       lock, so it never waits for, or blocks, a package operation. */
    if (j->kind == JOB_CHECK_RELEASE) {
        st = pkg_release_check(su_base(), NULL, j->current, &j->done->rel, &e);
        if (st == PKG_OK && j->su) {
            /* The rest of the installation too, so the window can offer an
               update when only apkg or a copy is behind. Hashing is slow,
               so it is done here and not on the window's task. */
            pkg_err e2;
            memset(&e2, 0, sizeof e2);
            j->su->located = su_locate(j->su->program, 0, j->root, NULL, &j->su->in, &j->su->where,
                                       &j->su->facts, &e2) == PKG_OK;
        }
    } else if (j->kind == JOB_SU_LOCATE) {
        /* Where this PkgManager's installation is, and its files now: the
           files are hashed, which takes a moment on a slow machine. */
        su_job *s = j->su;
        st = su_locate(s->program, 0, j->root, NULL, &s->in, &s->where, &s->facts, &e);
        s->c_path[0] = 0;
        if (st == PKG_OK && s->in.home != SU_HOME_NONE)
            su_unrecorded_c_apkg(&s->in, s->c_path, sizeof s->c_path, s->c_version, sizeof s->c_version);
    } else if (j->kind == JOB_SU_STAGE) {
        /* The release, downloaded and checked; nothing outside T: changes.
           An operation that holds the root now would refuse the update
           after the window has closed: it is refused here instead. */
        su_job *s = j->su;
        if ((st = su_precheck(&s->in, j->root, 1, &e)) == PKG_OK
            && (st = su_release(NULL, NULL, &s->rel, &e)) == PKG_OK)
            st = su_stage_release(&s->in, &s->rel, NULL, s->stage_dir, on_progress, j, &s->st, &e);
    } else if (j->kind == JOB_VERIFY || j->kind == JOB_RUN) {
        /* Both only read the root: no lock, so neither waits for, or holds
           up, an install in another program. Verify hashes every file, which
           takes seconds for a large package, and a Workbench start is
           confirmed by watching for the process; neither belongs on the
           window's task. */
        if ((st = pkg_open(j->root, PKG_OPEN_READ, &c, &e)) == PKG_OK) {
            if (j->kind == JOB_VERIFY) {
                char *listing = NULL;
                st = pkg_verify(c, j->id, &j->done->ver, &listing, &e);
                if (listing) {
                    size_t n = strlen(listing) + 1;
                    if ((j->done->listing = (char *)AllocVec(n, MEMF_PUBLIC)) != NULL)
                        memcpy(j->done->listing, listing, n);
                    free(listing);
                }
            } else {
                st = pkg_run(c, j->id, NULL, 0, 0, &j->done->run, &e);
            }
            pkg_close(c);
        }
    } else if ((st = pkg_open(j->root, PKG_OPEN_WRITE, &c, &e)) == PKG_OK) {
        snprintf(j->done->note, sizeof j->done->note, "%s", pkg_replacements_finished(c));
        pkg_set_progress(c, on_progress, j);
        switch (j->kind) {
        case JOB_INSTALL: {
            /* The same plan the window showed, made again under the lock:
               if it grew since, it was not what the user accepted. */
            pkg_install_plan *pl = (pkg_install_plan *)AllocVec(sizeof *pl, MEMF_ANY | MEMF_CLEAR);
            if (!pl) { st = PKG_E_NOMEM; snprintf(e.summary, sizeof e.summary, "out of memory"); break; }
            st = pkg_plan_install(c, j->index, j->id, pl, &e);
            if (st == PKG_OK && !pl->ok) {
                st = e.status = pl->status ? pl->status : PKG_E_REQUIRES;
                snprintf(e.summary, sizeof e.summary, "cannot install it");
                snprintf(e.detail, sizeof e.detail, "%s", pl->why);
            } else if (st == PKG_OK && pl->extra) {
                char sig[700];
                plan_signature(pl, sig, sizeof sig);
                if (!j->confirmed || strcmp(sig, j->plan_sig)) {
                    st = e.status = PKG_E_REQUIRES;
                    snprintf(e.summary, sizeof e.summary, "the plan changed since it was shown");
                    snprintf(e.detail, sizeof e.detail, "Nothing was changed. Press Install again to see it.");
                } else st = pkg_install_plan_run(c, j->index, pl, PKG_STOP_NONE, &e);
            } else if (st == PKG_OK)
                st = pkg_install_plan_run(c, j->index, pl, PKG_STOP_NONE, &e);
            FreeVec(pl);
            break;
        }
        case JOB_REMOVE:   st = pkg_remove(c, j->id, PKG_STOP_NONE, &e);
                           if (st == PKG_OK) pkg_last_removal(c, &j->done->rem);
                           break;
        case JOB_UPDATE:   st = pkg_update(c, j->index_url, &e); break;
        case JOB_UPGRADE:  st = pkg_upgrade(c, j->index, j->id, PKG_STOP_NONE, &e); break;
        case JOB_ROLLBACK: st = pkg_rollback(c, j->id, PKG_STOP_NONE, &e); break;
        case JOB_PREVIEW_UPGRADE:  st = pkg_upgrade_preview(c, j->index, j->id, 1, &plan, &e); break;
        case JOB_PREVIEW_ROLLBACK: st = pkg_rollback_preview(c, j->id, &plan, &e); break;
        default: break;
        }
        pkg_close(c);
    }

    m = j->done;
    m->kind = PM_DONE;
    m->seq = j->seq;
    strncpy(m->id, j->id, sizeof m->id - 1);
    m->status = st;
    m->err = e;
    m->plan = plan;
    j->running = 0;
    /* Testing only (--slow): hold open for three seconds the gap between
       the worker being finished and the window hearing it, so a test can
       show that the window stays busy through it instead of starting the
       next job -- which then took over the record this message belongs to. */
    if (slow_ms) Delay(150);
    /* Forbid, then send, then return: the classic way for a child to leave
       before its parent can act. The window may quit on PM_DONE, and quitting
       unloads the code this process is still running until it has exited.
       Under Forbid the parent does not get the CPU until this task is gone;
       the Forbid ends with the task. Nothing here is touched after PutMsg. */
    Forbid();
    send(j, m);
}

/* "Done" is the operation's end, not the process's. Before the window
 * unloads this program it waits until the worker task is no longer in the
 * system, by name, under Forbid, and says how long that took. There is no
 * giving up: a task that is still running this program's code must not have
 * the code taken from under it, whatever the clock says. `report_ms` is only
 * how often the caller is told the wait is still going on. Returns the wait
 * in ms. */
long job_wait_gone(long report_ms, void (*still)(long ms))
{
    long waited = 0, next = report_ms;
    for (;;) {
        struct Task *t;
        Forbid();
        t = FindTask((STRPTR)WORKER_NAME);
        Permit();
        if (!t) return waited;
        if (still && waited >= next) { still(waited); next += report_ms; }
        Delay(1);              /* 1/50 s */
        waited += 20;
    }
}

int job_start(job *j)
{
    struct Process *p;
    BPTR nil_in, nil_out;

    current = j;
    j->cancel = 0;
    j->done = (pm_msg *)AllocVec(sizeof *j->done, MEMF_PUBLIC | MEMF_CLEAR);
    if (test_nomem_start && j->done) { FreeVec(j->done); j->done = NULL; }
    if (!j->done) return -1;
    j->running = 1;
    /* libpkg prints its running commentary with printf. A process with no
       console would hand that to a NULL handle; give it NIL: instead. The
       window shows what matters through the messages. */
    nil_in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    nil_out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    p = CreateNewProcTags(NP_Entry,       (IPTR)worker_main,
                          NP_Name,        (IPTR)WORKER_NAME,
                          NP_StackSize,   262144,
                          /* The window's own priority. At -1 the worker
                             only ran when the window and every script
                             talking to it were waiting: a script polling
                             JOB without a pause kept the install from ever
                             finishing (run AX3). Equal priorities take
                             turns, and the window mostly waits anyway. */
                          NP_Priority,    0,
                          NP_Input,       (IPTR)nil_in,
                          NP_Output,      (IPTR)nil_out,
                          NP_CloseInput,  TRUE,
                          NP_CloseOutput, TRUE,
                          TAG_DONE);
    if (!p) {
        if (nil_in)  Close(nil_in);
        if (nil_out) Close(nil_out);
        FreeVec(j->done); j->done = NULL;
        j->running = 0;
        return -1;
    }
    return 0;
}

void plan_signature(const pkg_install_plan *pl, char *out, size_t n)
{
    int i; size_t k = 0;
    out[0] = 0;
    for (i = 0; i < pl->nops && k + 2 < n; i++)
        k += (size_t)snprintf(out + k, n - k, "%d:%s:%s:%ld;", pl->ops[i].kind, pl->ops[i].id,
                              pl->ops[i].version, pl->ops[i].revision);
}
