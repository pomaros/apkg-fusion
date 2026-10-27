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
static size_t last_public_size;
void *AllocVec(size_t n, unsigned flags) {
    if (++allocation_calls == fail_at)
        return NULL;
    void *p = (flags & MEMF_CLEAR) ? calloc(1, n) : malloc(n);
    if (p) {
        allocations++;
        last_public_size = n;
    }
    return p;
}
static void owned_text(const void *model, apkg_text text) {
    uintptr_t base = (uintptr_t)model, address = (uintptr_t)text.data;
    assert(address >= base && address - base < last_public_size);
    assert(text.size < last_public_size - (address - base));
    assert(text.data[text.size] == 0);
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
    if (atoi(argv[3]) == 7 || atoi(argv[3]) == 13) {
        int extended = atoi(argv[3]) == 13;
        pm_common_installed *entries = NULL;
        fail_at = extended && argc >= 5 ? atoi(argv[4]) : 0;
        apkg_status status = pm_common_installed_read(
            argv[1], extended ? argv[2] : NULL, &entries, NULL);
        if (extended)
            printf("status %d\n", (int)status);
        else
            assert(status == APKG_OK);
        if (status)
            assert(!entries);
        if (entries)
            for (size_t i = 0; i < entries->count; i++) {
                const apkg_installed_view *v = &entries->rows[i].view;
                apkg_text fields[] = {
                    v->authority.value,
                    v->authority.evidence,
                    v->name.value,
                    v->name.evidence,
                    v->version.value,
                    v->version.evidence,
                    v->target.value,
                    v->target.evidence,
                    v->revision.evidence,
                    {(const char *)v->record.data, v->record.size}};
                for (size_t k = 0; k < sizeof fields / sizeof fields[0]; k++)
                    owned_text(entries, fields[k]);
                owned_text(entries,
                           (apkg_text){entries->rows[i].version_label,
                                       strlen(entries->rows[i].version_label)});
                if (extended)
                    printf("%s\t%s\t%llu\t%s\t%.*s\n", v->name.value.data,
                           v->version.value.data,
                           (unsigned long long)v->revision.value,
                           entries->rows[i].version_label, (int)v->record.size,
                           v->record.data);
                else
                    printf("%s %s\n", v->name.value.data,
                           v->version.value.data);
            }
        FreeVec(entries);
        assert(allocations == 0);
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
    if (choice == 12) {
        j.kind = JOB_RUN;
        assert(strlen(argv[2]) < sizeof j.id);
        strcpy(j.id, argv[2]);
        result = pm_common_install(&j, &error);
        /* A valid AROS bundle reaches the host's explicit runtime boundary;
         * truncating the name would instead yield NOT_FOUND before this. */
        assert(result == PKG_E_IO && final.common_status == APKG_UNSUPPORTED);
        assert(!previews && final.common_outcome == APKG_UNCHANGED);
        assert(allocations == 0);
        return 0;
    }
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
