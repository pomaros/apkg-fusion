/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Executable consumer example against fake.c, never a production engine. */
#include "apkg.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static apkg_event_copy *queued;
static apkg_event_action queue_event(void *user, const apkg_event *event)
{
    (void)user;
    /* A real GUI posts this owned copy to the UI; allocation failure asks
     * the worker to cancel at a safe boundary. Do not pass event itself. */
    if (apkg_event_clone(event, &queued) != APKG_OK)
        return APKG_REQUEST_CANCEL;
    return APKG_CONTINUE;
}

int main(void)
{
    apkg_open_options o = {0};
    apkg_ctx *ctx = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_error *error = NULL;
    apkg_text path = {"example-root", 12};
    char subject[] = "example";
    apkg_event event = {0};
    const apkg_root_view *view;
    const apkg_installed_view *row = NULL;
    o.contract_version = APKG_CONTRACT_VERSION;
    o.struct_size = sizeof o;
    o.root = path;
    o.mode = APKG_READ;
    assert(apkg_open(&o, &ctx, &error) == APKG_OK);
    assert(apkg_snapshot_read(ctx, &snapshot, &error) == APKG_OK);
    apkg_close(ctx); /* snapshot owns its backing storage independently */
    view = apkg_snapshot_root(snapshot);
    assert(view->state == APKG_ROOT_ABSENT);
    assert(view->identity_state == APKG_UNKNOWN);
    assert(view->generation.state == APKG_UNKNOWN);
    assert(apkg_snapshot_installed_count(snapshot) == 0);
    assert(apkg_snapshot_installed_at(snapshot, 0, &row, &error) == APKG_NOT_FOUND);
    assert(row == NULL);
    apkg_error_free(error);
    apkg_snapshot_free(snapshot);
    event.subject.data = subject;
    event.subject.size = strlen(subject);
    event.completed = UINT64_C(4294967297);
    event.total.state = APKG_UNKNOWN;
    assert(queue_event(NULL, &event) == APKG_CONTINUE);
    subject[0] = 'X'; /* worker frame can now be overwritten */
    assert(apkg_event_get(queued)->subject.data[0] == 'e');
    assert(apkg_event_get(queued)->completed == UINT64_C(4294967297));
    assert(apkg_event_get(queued)->total.state == APKG_UNKNOWN);
    apkg_event_free(queued);
    puts("API example: snapshot ownership, unknown state, event copy and 64-bit progress passed (test double)");
    return 0;
}
