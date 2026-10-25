/* What the window and the worker share. One job at a time, by design: the
 * window disables its buttons while one runs, and the root lock in libpkg
 * would refuse a second anyway. */
#ifndef PKGMANAGER_JOB_H
#define PKGMANAGER_JOB_H

#include "../libpkg/pkg.h"
#include "../libpkg/release.h"
#include "../libpkg/selfup.h"
#include <exec/ports.h>

typedef enum { JOB_NONE, JOB_INSTALL, JOB_REMOVE, JOB_UPDATE,
               JOB_PREVIEW_UPGRADE, JOB_UPGRADE, JOB_PREVIEW_ROLLBACK, JOB_ROLLBACK,
               JOB_CHECK_RELEASE, JOB_VERIFY, JOB_RUN,
               JOB_SU_LOCATE, JOB_SU_STAGE } job_kind;

/* Updating arospkg (libpkg/selfup.h): what the worker finds and stages for
   the window, which shows it and decides. Too big for a message, and only
   one job is ever out, so the window owns it and the worker fills it in. */
typedef struct {
    char        program[PKG_MAXPATH];  /* this PkgManager's file */
    su_install  in;
    su_where    where;
    su_facts    facts;
    pkg_release rel;
    su_stage    st;
    char        stage_dir[PKG_MAXPATH];
    char        c_path[PKG_MAXPATH];   /* an unrecorded C:apkg to ask about */
    char        c_version[48];
    int         located;               /* JOB_CHECK_RELEASE: in/where describe this installation */
} su_job;

struct pm_msg_s;

typedef struct {
    job_kind        kind;
    long            seq;        /* the job's number; every message carries it */
    char            id[PKG_MAXID];
    char            root[PKG_MAXPATH];
    char            index[PKG_MAXPATH];
    char            index_url[512];
    char            current[16];  /* JOB_CHECK_RELEASE: this PkgManager's version */
    struct MsgPort *reply;      /* the window's port; the worker sends here */
    int             confirmed;  /* JOB_INSTALL: the user accepted a plan with more steps */
    char            plan_sig[700];  /* ...namely this one, step by step (plan_signature) */
    volatile int    cancel;     /* set by the window, read by the callback  */
    volatile int    running;
    /* The final message, allocated by job_start before the worker exists:
       an operation must never end with nothing to say so because memory ran
       out after the work was done. */
    struct pm_msg_s *done;
    su_job         *su;         /* JOB_SU_*: the window's record of the update */
} job;

/* Every message the worker sends. PM_PROGRESS carries one pkg_progress_ev,
 * copied, because the event's strings belong to the worker's stack frame.
 * PM_DONE carries the outcome. The window frees each with FreeVec. */
enum { PM_PROGRESS = 1, PM_DONE = 2 };

typedef struct pm_msg_s {
    struct Message msg;
    int            kind;
    long           seq;         /* job.seq of the job it reports on */
    char           phase[16];
    char           id[PKG_MAXID];
    unsigned long  done, total;
    int            can_cancel;
    pkg_status     status;
    pkg_err        err;
    pkg_plan       plan;        /* filled for the preview jobs */
    pkg_release    rel;         /* filled for JOB_CHECK_RELEASE */
    pkg_verify_result ver;      /* JOB_VERIFY: the counts */
    char          *listing;     /* JOB_VERIFY: one line per file, AllocVec'd; the window frees it */
    pkg_run_result run;         /* JOB_RUN: what was started */
    pkg_removal    rem;         /* JOB_REMOVE: what was kept in place */
    char           note[200];   /* a replacement recovery finished when the root was opened */
} pm_msg;

/* Start the worker process for `j`. Returns 0 on success; the worker will
 * send PM_DONE exactly once, whatever happens. -1 when the final message or
 * the process could not be had; nothing was started then. */
int job_start(job *j);

/* The steps of an install plan as one string, so the worker can tell the
   plan it is about to run from the one the user accepted. */
void plan_signature(const pkg_install_plan *pl, char *out, size_t n);
void job_set_slow(int ms);      /* testing only */
void job_test_nomem(void);      /* testing only */
long job_wait_gone(long report_ms, void (*still)(long ms));

#endif
