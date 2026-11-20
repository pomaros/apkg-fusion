/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
/* Compile the actual native dispatcher without linking ANY imported engine. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
static int fail_thread;
static int create_worker(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*entry)(void *), void *arg) {
    return fail_thread ? EAGAIN : pthread_create(thread, attr, entry, arg);
}
#define pthread_create create_worker
#include "../../components/arospkg/src/pkgmanager/worker.c"
#undef pthread_create
static int allocations, posts, common_calls;
static job *active_job;
void *AllocVec(size_t size, unsigned flags) {
    void *p = flags & MEMF_CLEAR ? calloc(1, size) : malloc(size);
    if (p) allocations++;
    return p;
}
void FreeVec(void *p) { if (p) { allocations--; free(p); } }
void Forbid(void) {}
void Permit(void) {}
struct Task *FindTask(STRPTR name) { (void)name; return NULL; }
void Delay(unsigned long ticks) { (void)ticks; }
pkg_status pm_common_install(job *j, pkg_err *error) {
    assert(j == active_job && job_uses_common_engine(j->kind));
    assert(strlen(j->id) == 255);
    (void)error; common_calls++;
    j->done->common_status = APKG_OK;
    return PKG_OK;
}
void PutMsg(struct MsgPort *port, struct Message *message) {
    pm_msg *m = (pm_msg *)message;
    assert(port == active_job->reply);
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
    assert(job_for_common_rexx(RXC_ROLLBACK) == JOB_ROLLBACK);
    for (int kind = JOB_NONE; kind <= JOB_COMMON_SOURCE_LIST + 1; kind++) {
        job j = {0}; j.kind = (job_kind)kind; j.seq = kind + 1;
        memset(j.id, 'x', 255); j.id[255] = 0;
        strcpy(j.root, "must-not-open-an-imported-root"); active_job = &j;
        posts = common_calls = 0;
        assert(job_start(&j) == 0);
        assert(job_wait_gone(100, NULL) == 0);
        assert(posts == 1 && allocations == 0);
    }
    job j = {0}; active_job = &j; fail_thread = 1;
    assert(job_start(&j) == -1 && !j.running && !j.done && allocations == 0);
    fail_thread = 0; job_test_nomem();
    assert(job_start(&j) == -1 && !j.running && !j.done && allocations == 0);
    assert(job_wait_gone(100, NULL) == 0);
    puts("Native dispatcher: common-only routing, one completion, no imported engine linked");
    return 0;
}
