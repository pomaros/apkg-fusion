/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Actual PkgManager signed worker bridge with host message-port adapters. */
#include "apkg_client.h"
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
        unsigned char zero[16] = {0};
        assert(memcmp(m->common_operation_id, zero, 16));
        assert(m->common_event_sequence > 0);
        assert(m->common_phase >= APKG_PHASE_CHECK &&
               m->common_phase <= APKG_PHASE_RECOVER);
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
    if (atoi(argv[3]) == 8) {
        assert(argc == 6);
        char *json = NULL;
        assert(pm_common_channel_query(argv[1], argv[2], argv[5], &json,
                                       &error) == PKG_OK);
        puts(json);
        apkg_client_text_free(json);
        return 0;
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
    if (choice >= 9 && choice <= 11) {
        assert(argc == 6);
        j.kind = JOB_COMMON_DISCOVER;
        fail_at = choice == 10 ? 1 : 0;
        assert(apkg_cancel_create(&j.common_cancel) == APKG_OK);
        if (choice == 11)
            apkg_cancel_request(j.common_cancel);
        result = pm_common_install(&j, &error);
        apkg_cancel_free(j.common_cancel);
        assert(previews == 0);
        if (choice == 11)
            assert(progress_count == 0);
        if (choice == 9) {
            assert(result == PKG_OK && final.common_status == APKG_OK);
            assert(final.common_catalogue);
            assert(!strcmp(final.common_catalogue->endpoint, argv[2]));
            assert(strstr(final.common_catalogue->summary, argv[5]));
            assert(strstr(final.common_catalogue->summary,
                          "online freshness: unknown"));
            assert(final.common_catalogue->channel.signature ==
                   APKG_SIGNATURE_VALID);
            for (size_t i = 0; i < final.common_catalogue->count; i++) {
                pm_common_row *row = &final.common_catalogue->rows[i];
                assert(strlen(row->selection) == 162);
                assert(strstr(row->detail, "Review: not asserted"));
                assert(strstr(row->detail, "Runtime testing: not asserted"));
                assert(strstr(row->detail, row->name));
                printf("%s\t%s\t%s\t%llu\t%d\t%d\n", row->selection, row->name,
                       row->version, (unsigned long long)row->revision,
                       row->author_trust, row->target_matches);
            }
        } else {
            assert(!final.common_catalogue);
            assert(final.common_status ==
                   (choice == 10 ? APKG_NOMEM : APKG_CANCELLED));
        }
        FreeVec(final.common_catalogue);
        assert(allocations == 0);
        return 0;
    }
    result = pm_common_install(&j, &error);
    if (j.common_action == APKG_CLIENT_CHANNEL &&
        (choice == 3 || choice == 4 || choice == 6))
        assert(previews == 0);
    else
        assert(previews == (choice == 3 ? 0 : 1));
    if (choice == 1) {
        assert(result == PKG_OK);
        assert(final.common_outcome == APKG_COMMITTED);
        assert(progress_count > 0);
    } else if (choice == 3 || choice == 4) {
        assert(result == PKG_E_NOMEM);
        assert(final.common_status == APKG_NOMEM);
        assert(final.common_outcome == APKG_UNCHANGED);
    } else {
        assert(result == PKG_E_CANCELLED);
        assert(final.common_status == APKG_CANCELLED);
        assert(final.common_outcome ==
               (choice == 6 && j.common_action != APKG_CLIENT_CHANNEL
                    ? APKG_RESTORED
                    : APKG_UNCHANGED));
    }
    FreeVec(final.common_record);
    assert(allocations == 0);
    printf("PkgManager bridge: choice=%d status=%d outcome=%d; message "
           "ownership balanced\n",
           choice, final.common_status, final.common_outcome);
    return 0;
}
