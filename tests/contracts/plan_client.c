/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Compile-only example: no planner/executor implementation is linked. */
#include "apkg.h"
typedef int (*confirm_plan)(const apkg_plan_view *, const apkg_plan *);
/* confirm_plan must inspect/present every material effect, then return 1 only
 * if authorized. A real application sends this between UI and worker. */
apkg_status example_install(apkg_ctx *ctx, const apkg_snapshot *snapshot,
                            const apkg_package *package, confirm_plan confirm,
                            const apkg_observer *observer, apkg_result **result,
                            apkg_error **error)
{
    apkg_plan *plan = NULL;
    apkg_approval *approval = NULL;
    apkg_status status;
    const apkg_plan_view *view;
    *result = NULL;
    status = apkg_plan_install(ctx, snapshot, package, observer, &plan, error);
    if (status != APKG_OK) goto done;
    view = apkg_plan_get(plan);
    if (!view->executable) { status = APKG_POLICY; goto done; }
    if (!confirm(view, plan)) { status = APKG_CANCELLED; goto done; }
    status = apkg_plan_approve(ctx, plan, &view->plan_digest, &approval, error);
    if (status != APKG_OK) goto done;
    status = apkg_execute(ctx, plan, approval, observer, result, error);
    /* Caller must inspect result outcome even when status != OK; it owns
     * result after plan/context cleanup and can send it to the UI. */
done:
    apkg_approval_free(approval);
    apkg_plan_free(plan);
    return status;
}

/* Compile-time coverage of explicit initialization, which is implemented only
 * in later planning/execution batches. Approval is still required to execute. */
apkg_status example_initial_plan(apkg_ctx *ctx, const apkg_snapshot *absent,
                                apkg_text target, apkg_bytes policy,
                                apkg_plan **plan, apkg_error **error)
{
    return apkg_plan_initialize(ctx, absent, target, policy, plan, error);
}
