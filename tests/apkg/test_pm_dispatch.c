/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
/* Compile the actual native dispatcher without linking ANY imported engine. */
#include <assert.h>
#include "../../components/arospkg/src/pkgmanager/worker.c"
static int allocations, posts, forbid_depth, common_calls, fail_process, closes;
static job *active_job;
struct Process { int dummy; } process;
void *AllocVec(size_t size, unsigned flags) {
    void *p = flags & MEMF_CLEAR ? calloc(1, size) : malloc(size);
    if (p) allocations++;
    return p;
}
void FreeVec(void *p) { if (p) { allocations--; free(p); } }
void Forbid(void) { forbid_depth++; }
void Permit(void) { forbid_depth--; }
struct Task *FindTask(STRPTR name) { (void)name; return NULL; }
void Delay(unsigned long ticks) { (void)ticks; }
BPTR Open(CONST_STRPTR path, long mode) { (void)path; (void)mode; return 1; }
int Close(BPTR file) { assert(file == 1); closes++; return 1; }
struct Process *CreateNewProcTags(IPTR tag, ...) {
    assert(tag == NP_Entry);
    return fail_process ? NULL : &process;
}
pkg_status pm_common_install(job *j, pkg_err *error) {
    assert(j == active_job && job_uses_common_engine(j->kind));
    assert(strlen(j->id) == 255);
    (void)error; common_calls++;
    j->done->common_status = APKG_OK;
    return PKG_OK;
}
void PutMsg(struct MsgPort *port, struct Message *message) {
    pm_msg *m = (pm_msg *)message;
    assert(port == active_job->reply && forbid_depth == 1);
    assert(!active_job->running && m->kind == PM_DONE && m->seq == active_job->seq);
    assert(m->msg.mn_Length == sizeof *m && !m->msg.mn_ReplyPort);
    if (job_uses_common_engine(active_job->kind)) {
        assert(m->status == PKG_OK && m->common_status == APKG_OK && common_calls == 1);
    } else {
        assert(m->status == PKG_E_IO && m->common_status == APKG_UNSUPPORTED && common_calls == 0);
        assert(strstr(m->err.summary, "requires common-engine migration"));
    }
    posts++; FreeVec(m);
}
int main(void) {
    for (int kind = JOB_NONE; kind <= JOB_COMMON_SOURCE_LIST + 1; kind++) {
        job j = {0}; j.kind = (job_kind)kind; j.seq = kind + 1;
        memset(j.id, 'x', 255); j.id[255] = 0;
        strcpy(j.root, "must-not-open-an-imported-root"); active_job = &j;
        posts = common_calls = forbid_depth = 0;
        assert(job_start(&j) == 0 && j.running && j.done);
        worker_main();
        assert(posts == 1 && allocations == 0);
        /* The task's exit releases Forbid; the host simulates that boundary. */
        forbid_depth = 0;
    }
    job j = {0}; active_job = &j; fail_process = 1;
    assert(job_start(&j) == -1 && !j.running && !j.done && allocations == 0 && closes == 2);
    fail_process = 0; job_test_nomem();
    assert(job_start(&j) == -1 && !j.running && !j.done && allocations == 0);
    assert(job_wait_gone(100, NULL) == 0);
    puts("Native dispatcher: common-only routing, one completion, no imported engine linked");
    return 0;
}
