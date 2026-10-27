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
    if (!m) {
        j->done->common_status = APKG_NOMEM;
        return APKG_REQUEST_CANCEL;
    }
    m->kind = PM_COMMON_PROGRESS;
    memcpy(m->common_operation_id, event->operation_id.bytes, 16);
    m->common_event_sequence = event->sequence;
    m->common_phase = event->phase;
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
    int owns_cancel = j->common_cancel == NULL;
    cancel = j->common_cancel;
    if (owns_cancel && apkg_cancel_create(&cancel) != APKG_OK) {
        if (error) {
            memset(error, 0, sizeof *error);
            error->status = PKG_E_NOMEM;
            snprintf(error->summary, sizeof error->summary, "out of memory");
        }
        j->done->common_status = APKG_NOMEM;
        return PKG_E_NOMEM;
    }
    observer.cancel = cancel;
    if (j->cancel)
        apkg_cancel_request(cancel);
    if (j->kind == JOB_COMMON_ATTESTATION) {
        char *text = NULL;
        status = j->cancel ? APKG_CANCELLED
                           : apkg_client_attestation(j->root, j->common_package,
                                                     &text, &detail);
        if (!status && !j->cancel) {
            size_t size = strlen(text) + 1;
            j->done->common_record = AllocVec(size, MEMF_PUBLIC);
            if (!j->done->common_record)
                status = APKG_NOMEM;
            else
                memcpy(j->done->common_record, text, size);
        }
        apkg_client_text_free(text);
        if (j->cancel) {
            FreeVec(j->done->common_record);
            j->done->common_record = NULL;
            status = APKG_CANCELLED;
        }
    } else if (j->kind == JOB_COMMON_RECOMPARE ||
               j->kind == JOB_COMMON_SOURCE_LOAD) {
        status =
            j->cancel ? APKG_CANCELLED
            : j->kind == JOB_COMMON_SOURCE_LOAD
                ? pm_common_source_load(j->root, j->common_package,
                                        &j->done->common_catalogue)
                : pm_common_catalogue_refresh(j->root, j->common_observation,
                                              &j->done->common_catalogue);
        if (j->cancel) {
            FreeVec(j->done->common_catalogue);
            j->done->common_catalogue = NULL;
            status = APKG_CANCELLED;
        }
    } else if (j->kind == JOB_COMMON_SELF_UPDATE) {
        /* common_package: endpoint, common_pool: channel id, common_action:
         * 1 checks only. The report becomes the window's record. */
        char *report = NULL;
        status = apkg_client_self_update(
            j->root, j->common_package, j->common_pool, j->common_executable,
            j->common_action == 1, ask, j, &observer, &report,
            j->common_action == 1 ? NULL : &result, &detail);
        if (report) {
            size_t size = strlen(report) + 1;
            j->done->common_record = AllocVec(size, MEMF_PUBLIC);
            if (j->done->common_record)
                memcpy(j->done->common_record, report, size);
            else if (!status)
                status = APKG_NOMEM;
        }
        apkg_client_text_free(report);
    } else if (j->kind == JOB_COMMON_DISCOVER) {
        apkg_channel *channel = NULL;
        apkg_snapshot *snapshot = NULL;
        status = apkg_client_channel_refresh(j->root, j->common_package,
                                             j->common_pool, 0, &observer,
                                             &snapshot, &channel, &detail);
        if (!status && !j->cancel)
            status =
                pm_common_catalogue_copy(channel, snapshot, j->common_package,
                                         &j->done->common_catalogue);
        if (j->cancel) {
            FreeVec(j->done->common_catalogue);
            j->done->common_catalogue = NULL;
            status = APKG_CANCELLED;
        }
        apkg_channel_free(channel);
        apkg_snapshot_free(snapshot);
    } else
        status =
            j->kind == JOB_RUN
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
        if (r->record.size && !j->done->common_record) {
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
    if (owns_cancel)
        apkg_cancel_free(cancel);
    return legacy;
}
#endif
