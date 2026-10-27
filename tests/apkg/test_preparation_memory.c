/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
extern void apkg_test_fault(int, int);
int main(int argc, char **argv) {
    assert(argc == 4);
    int point = atoi(argv[3]);
    apkg_open_options options = {0};
    options.contract_version = 1;
    options.struct_size = sizeof options;
    options.mode = APKG_WRITE;
    options.root.data = argv[1];
    options.root.size = strlen(argv[1]);
    apkg_ctx *ctx = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_package *package = NULL;
    apkg_plan *plan = NULL;
    apkg_approval *approval = NULL;
    apkg_result *result = NULL;
    assert(apkg_open(&options, &ctx, NULL) == APKG_OK);
    apkg_text archive = {argv[2], strlen(argv[2])};
    assert(apkg_package_import(ctx, archive, NULL, &package, NULL) == APKG_OK);
    assert(apkg_snapshot_read(ctx, &snapshot, NULL) == APKG_OK);
    assert(apkg_plan_install(ctx, snapshot, package, NULL, &plan, NULL) ==
           APKG_OK);
    apkg_snapshot_free(snapshot);
    apkg_package_free(package);
    assert(apkg_plan_approve(ctx, plan, &plan->view.plan_digest, &approval,
                             NULL) == APKG_OK);
    apkg_test_fault(point, 1);
    assert(apkg_execute(ctx, plan, approval, NULL, &result, NULL) == APKG_IO);
    apkg_test_fault(0, 0);
    assert(apkg_result_get(result)->outcome ==
           (point == 1205 ? APKG_RESTORED : APKG_UNCHANGED));
    apkg_result_free(result);
    result = NULL;
    apkg_approval_free(approval);
    approval = NULL;
    assert(apkg_recover(ctx, NULL, &result, NULL) == APKG_OK);
    assert(apkg_result_get(result)->outcome == APKG_UNCHANGED);
    apkg_result_free(result);
    result = NULL;
    assert(apkg_plan_approve(ctx, plan, &plan->view.plan_digest, &approval,
                             NULL) == APKG_OK);
    assert(apkg_execute(ctx, plan, approval, NULL, &result, NULL) == APKG_OK);
    assert(apkg_result_get(result)->outcome == APKG_COMMITTED);
    apkg_result_free(result);
    apkg_approval_free(approval);
    apkg_plan_free(plan);
    apkg_close(ctx);
    return 0;
}
