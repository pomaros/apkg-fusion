/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Common signed-install path, running in the existing DOS Process worker. */
#ifdef APKG_FUSION
#include "apkg_client.h"
#include "job.h"
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>
static void post(job *j, pm_msg *m) {
    m->msg.mn_Length = sizeof *m;
    m->msg.mn_ReplyPort = NULL;
    m->seq = j->seq;
    PutMsg(j->reply, &m->msg);
}
static int ask(void *user, const apkg_plan *plan, const char *text) {
    job *j = user;
    size_t n = strlen(text) + 1;
    pm_msg *m = AllocVec(sizeof *m, MEMF_PUBLIC | MEMF_CLEAR);
    (void)plan;
    if (!m) {
        j->done->common_status = APKG_NOMEM;
        return 0;
    }
    m->common_preview = AllocVec(n, MEMF_PUBLIC);
    if (!m->common_preview) {
        FreeVec(m);
        j->done->common_status = APKG_NOMEM;
        return 0;
    }
    memcpy(m->common_preview, text, n);
    m->kind = PM_COMMON_PREVIEW;
    post(j, m);
    /* Keep context/plan alive on this worker Process until its UI responds.
     * Nothing is re-resolved and no malloc-owned handle crosses process exit.
     */
    while (!j->common_decision && !j->cancel)
        Delay(1);
    return j->common_decision == 1 && !j->cancel;
}
static apkg_event_action progress(void *user, const apkg_event *event) {
    job *j = user;
    pm_msg *m = AllocVec(sizeof *m, MEMF_PUBLIC | MEMF_CLEAR);
    if (!m)
        return APKG_REQUEST_CANCEL;
    m->kind = PM_COMMON_PROGRESS;
    m->common_done = event->completed;
    m->common_total = event->total.value;
    m->common_total_known = event->total.state == APKG_KNOWN;
    m->can_cancel = event->cancellable;
    post(j, m);
    return j->cancel ? APKG_REQUEST_CANCEL : APKG_CONTINUE;
}
pkg_status pm_common_install(job *j, pkg_err *error) {
    apkg_result *result = NULL;
    apkg_error *detail = NULL;
    apkg_cancel *cancel = NULL;
    apkg_observer observer = {progress, j, NULL};
    apkg_status status;
    pkg_status legacy;
    if (apkg_cancel_create(&cancel) != APKG_OK) {
        if (error) {
            memset(error, 0, sizeof *error);
            error->status = PKG_E_NOMEM;
            snprintf(error->summary, sizeof error->summary, "out of memory");
        }
        j->done->common_status = APKG_NOMEM;
        return PKG_E_NOMEM;
    }
    observer.cancel = cancel;
    status = j->kind == JOB_RUN
                 ? apkg_client_run(j->root, j->id, &detail)
                 : apkg_client_apply((apkg_client_action)j->common_action,
                                     j->root, j->common_package, j->common_pool,
                                     ask, j, &observer, &result, &detail);
    if (status == APKG_CANCELLED && j->done->common_status == APKG_NOMEM)
        status = APKG_NOMEM;
    j->done->common_status = status;
    if (result) {
        const apkg_result_view *r = apkg_result_get(result);
        j->done->common_outcome = r->outcome;
        if (r->record.size) {
            char *copy = AllocVec(r->record.size + 1, MEMF_PUBLIC);
            if (copy) {
                memcpy(copy, r->record.data, r->record.size);
                copy[r->record.size] = 0;
                j->done->common_record = copy;
            }
        }
    }
    legacy = status == APKG_OK          ? PKG_OK
             : status == APKG_CANCELLED ? PKG_E_CANCELLED
             : status == APKG_NOMEM     ? PKG_E_NOMEM
                                        : PKG_E_IO;
    if (error) {
        memset(error, 0, sizeof *error);
        error->status = legacy;
        if (detail) {
            const apkg_error_view *v = apkg_error_get(detail);
            snprintf(error->summary, sizeof error->summary, "%.*s",
                     (int)v->message.size,
                     v->message.data ? v->message.data : "");
        } else if (status != APKG_OK)
            snprintf(error->summary, sizeof error->summary,
                     "signed installation refused (code %d)", (int)status);
    }
    apkg_error_free(detail);
    apkg_result_free(result);
    apkg_cancel_free(cancel);
    return legacy;
}
#endif
