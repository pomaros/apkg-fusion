/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Actual PkgManager signed worker bridge with host message-port adapters. */
#include "apkg.h"
#include "job.h"
#include <assert.h>
#include <proto/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static job *running;
static int choice, previews, progress_count, allocations, allocation_calls,
    fail_at;
void *AllocVec(size_t n, unsigned flags) {
    if (++allocation_calls == fail_at)
        return NULL;
    void *p = (flags & MEMF_CLEAR) ? calloc(1, n) : malloc(n);
    if (p)
        allocations++;
    return p;
}
void FreeVec(void *p) {
    if (p) {
        allocations--;
        free(p);
    }
}
void PutMsg(struct MsgPort *port, struct Message *message) {
    pm_msg *m = (pm_msg *)message;
    (void)port;
    assert(m->seq == running->seq);
    if (m->kind == PM_COMMON_PREVIEW) {
        previews++;
        assert(m->common_preview);
        assert(strstr(m->common_preview, "Approved plan:"));
        assert(strstr(m->common_preview, "SHA-256"));
        if (running->common_action != 1 && running->common_action != 4)
            assert(strstr(m->common_preview, "Signature: verified"));
        running->common_decision = choice == 0 ? -1 : 1;
        if (choice == 2) {
            running->common_decision = 0;
            running->cancel = 1;
        }
        FreeVec(m->common_preview);
    } else {
        assert(m->kind == PM_COMMON_PROGRESS);
        progress_count++;
        if (choice == 6 && progress_count == 2)
            running->cancel = 1;
    }
    FreeVec(m);
}
void Delay(unsigned long ticks) {
    (void)ticks;
    assert(0 && "confirmation should be returned by host message adapter");
}
int main(int argc, char **argv) {
    job j = {0};
    pm_msg final = {0};
    pkg_err error = {0};
    pkg_status result;
    assert(argc >= 4 && argc <= 6);
    j.common_action = argc >= 5 ? atoi(argv[4]) : 0;
    if (argc == 6) {
        assert(strlen(argv[5]) < sizeof j.common_pool);
        strcpy(j.common_pool, argv[5]);
    }
    if (atoi(argv[3]) == 7) {
        pkg_entries entries = {0};
        assert(pm_common_query(argv[1], NULL, &entries, &error) == PKG_OK);
        for (int i = 0; i < entries.n; i++)
            printf("%s %s\n", entries.v[i].id, entries.v[i].version);
        free(entries.v);
        return 0;
    }
    running = &j;
    choice = atoi(argv[3]);
    fail_at = choice == 3 ? 1 : choice == 4 ? 3 : 0;
    j.seq = 42;
    j.done = &final;
    assert(strlen(argv[1]) < sizeof j.root &&
           strlen(argv[2]) < sizeof j.common_package);
    strcpy(j.root, argv[1]);
    strcpy(j.common_package, argv[2]);
    result = pm_common_install(&j, &error);
    assert(previews == (choice == 3 ? 0 : 1));
    if (choice == 1) {
        assert(result == PKG_OK);
        assert(final.common_outcome == APKG_COMMITTED);
        assert(progress_count > 0);
    } else if (choice == 3) {
        assert(result == PKG_E_NOMEM);
        assert(final.common_status == APKG_NOMEM);
        assert(final.common_outcome == APKG_UNCHANGED);
    } else {
        assert(result == PKG_E_CANCELLED);
        assert(final.common_status == APKG_CANCELLED);
        assert(final.common_outcome ==
               (choice == 6 ? APKG_RESTORED : APKG_UNCHANGED));
    }
    FreeVec(final.common_record);
    assert(allocations == 0);
    printf("PkgManager bridge: choice=%d status=%d outcome=%d; message "
           "ownership balanced\n",
           choice, final.common_status, final.common_outcome);
    return 0;
}
