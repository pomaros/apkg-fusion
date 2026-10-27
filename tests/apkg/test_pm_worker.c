/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Actual PkgManager signed worker bridge with host message-port adapters. */
#include "apkg_client.h"
#include "internal.h"
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
        if (running->kind == JOB_COMMON_SOURCE_ADD || running->kind == JOB_COMMON_SOURCE_REMOVE)
            assert(strstr(m->common_preview, running->kind == JOB_COMMON_SOURCE_ADD ? "Add source " : "Remove source "));
        else
            assert(strstr(m->common_preview, "SHA-256"));
        if (running->kind != JOB_COMMON_SOURCE_ADD && running->kind != JOB_COMMON_SOURCE_REMOVE &&
            running->common_action != 1 && running->common_action != 4 &&
            running->common_action != APKG_CLIENT_ROLLBACK_LAST &&
            running->common_action != APKG_CLIENT_CHANNEL_ACCEPT)
            assert(strstr(m->common_preview, "Signature: verified"));
        if (running->common_action == APKG_CLIENT_CHANNEL_ACCEPT)
            assert(strstr(m->common_preview, "Channel signing key:"));
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
static apkg_bytes claim_file(const char *folder, const char *name) {
    char path[4096];
    assert(snprintf(path, sizeof path, "%s/%s", folder, name) <
           (int)sizeof path);
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f);
    assert(n >= 0 && n <= 524288);
    rewind(f);
    unsigned char *bytes = malloc((size_t)n + 1);
    assert(bytes);
    assert(fread(bytes, 1, (size_t)n, f) == (size_t)n);
    assert(!fclose(f));
    return (apkg_bytes){bytes, (size_t)n};
}
int main(int argc, char **argv) {
    job j = {0};
    pm_msg final = {0};
    pkg_err error = {0};
    pkg_status result;
    assert(argc >= 4 && argc <= 9);
    assert(!job_uses_common_engine(JOB_NONE) && !job_uses_common_engine(JOB_INSTALL));
    j.common_action = argc >= 5 ? atoi(argv[4]) : 0;
    if(atoi(argv[3])==54){
        assert(argc==6);
        apkg_source_config *old=NULL;apkg_snapshot *snapshot=NULL;apkg_channel *current=NULL,*history=NULL,*active=(void *)1;
        assert(apkg_source_config_read(argv[4],&old,NULL)==APKG_OK);
        assert(apkg_client_channel_refresh(argv[1],argv[2],argv[5],0,NULL,&snapshot,&current,NULL)==APKG_OK);
        const apkg_source_config_view *v=apkg_source_config_get(old);
        apkg_open_options options={0};options.contract_version=1;options.struct_size=sizeof options;
        options.root=(apkg_text){argv[1],strlen(argv[1])};apkg_ctx *ctx=NULL;
        assert(apkg_open(&options,&ctx,NULL)==APKG_OK);
        assert(apkg_channel_read(ctx,&v->id,0,v->observation,v->signature,&active,NULL)==APKG_STALE&&!active);
        assert(ak_channel_history_read(snapshot,current,v->observation,v->signature,&history,NULL)==APKG_OK);
        apkg_package *package=(void *)1;apkg_plan *plan=(void *)1;
        assert(apkg_channel_package(ctx,history,0,&package,NULL)==APKG_UNSUPPORTED&&!package);
        package=(void *)1;assert(ak_channel_metadata(ctx,history,0,&package,NULL)==APKG_UNSUPPORTED&&!package);
        assert(apkg_plan_channel_accept(ctx,snapshot,history,&plan,NULL)==APKG_UNSUPPORTED&&!plan);
        apkg_channel_free(history);apkg_channel_free(current);apkg_snapshot_free(snapshot);apkg_source_config_free(old);apkg_close(ctx);
        puts("historical observation cannot select packages or advance replay floors");return 0;
    }
    if (atoi(argv[3]) == 52 || atoi(argv[3]) == 53) {
        int discover=atoi(argv[3])==52;
        assert(strlen(argv[1])<sizeof j.root&&strlen(argv[2])<sizeof j.common_package);
        strcpy(j.root,argv[1]);strcpy(j.common_package,argv[2]);j.done=&final;running=&j;
        if(discover){
            assert(argc==7||argc==8);
            assert(strlen(argv[4])<sizeof j.common_pool&&strlen(argv[5])<sizeof j.common_profile);
            strcpy(j.common_pool,argv[4]);strcpy(j.common_profile,argv[5]);
            j.cancel=atoi(argv[6]);fail_at=argc==8?atoi(argv[7]):0;j.kind=JOB_COMMON_DISCOVER;
        }else{assert(argc>=4&&argc<=6);fail_at=argc>=5?atoi(argv[4]):0;j.kind=JOB_COMMON_SOURCE_LOAD;}
        pm_common_install(&j,&error);fail_at=0;
        pm_common_catalogue *model=final.common_catalogue;
        int code=final.common_status?final.common_status:(int)model->last_status;
        printf("status %d\nsaved %d\n",code,final.common_profile_saved);
        if(!code){
            if(!discover&&argc==6){
                char identity[33];const char hex[]="0123456789abcdef";
                for(size_t i=0;i<16;i++){identity[i*2]=hex[model->channel.id.bytes[i]>>4];identity[i*2+1]=hex[model->channel.id.bytes[i]&15];}identity[32]=0;
                assert(pm_common_source_save(j.root,argv[5],model->endpoint,identity,model)==APKG_OK);
            }
            const apkg_news_view *n=&model->last;
            printf("summary\t%d\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",n->has_previous,
                (unsigned long long)n->count,(unsigned long long)n->applications,(unsigned long long)n->builds,
                (unsigned long long)n->updated,(unsigned long long)n->content_changed,(unsigned long long)n->recent,
                (unsigned long long)n->unknown_dates,(unsigned long long)n->indeterminate);
            owned_text(model,(apkg_text){(const char *)model->previous_observation.data,model->previous_observation.size});
            owned_text(model,(apkg_text){(const char *)model->previous_signature.data,model->previous_signature.size});
            for(uint64_t rank=1;rank<=n->count;rank++)for(size_t i=0;i<model->count;i++){
                const pm_common_row *r=&model->rows[i];if(r->last_rank!=rank)continue;
                const char *sha=strrchr(r->selection,'/');assert(sha);
                printf("change\t%s\t%s\t%s\t%s\t%s\t%llu\t%s\t%llu\t%d\t%s\t%d\n",
                    apkg_news_kind_name(r->last_kind),r->authority,r->name,r->target,r->version,
                    (unsigned long long)r->revision,r->previous_version,(unsigned long long)r->previous_revision,
                    r->earlier_publication,sha+1,r->last_direction);
                char *detail=NULL;assert(pm_common_last_detail(r,&detail)==APKG_OK);
                assert(strstr(detail,apkg_news_kind_name(r->last_kind)));
                if(*r->previous_version)assert(strstr(detail,r->previous_version));
                apkg_client_text_free(detail);
            }
            pm_common_catalogue *refreshed=NULL;
            assert(pm_common_catalogue_refresh(j.root,model,&refreshed)==APKG_OK);
            assert(refreshed->last.has_previous==n->has_previous&&refreshed->last.count==n->count);
            assert(refreshed->previous_observation.size==model->previous_observation.size);
            FreeVec(refreshed);
        }
        FreeVec(model);FreeVec(final.common_record);assert(previews==0&&allocations==0);return 0;
    }
    if (atoi(argv[3]) == 51) {
        assert(argc == 9);
        apkg_snapshot *snapshot = NULL;
        apkg_channel *channel = NULL;
        apkg_recent *recent = NULL;
        apkg_recent_options options = {0};
        options.builds = atoi(argv[4]); options.limit = strtoull(argv[6],NULL,10);
        options.since = (apkg_text){argv[7],strlen(argv[7])};
        apkg_status code = apkg_client_channel_refresh(argv[1],argv[2],argv[5],0,NULL,&snapshot,&channel,NULL);
        if (!code) code = apkg_channel_recent(channel,&options,&recent,NULL);
        char *text = NULL;
        if (!code) code = apkg_recent_format(recent,atoi(argv[8]),&text,NULL);
        printf("status %d\n",(int)code);
        if (!code) fputs(text,stdout); else assert(!text);
        apkg_client_text_free(text); apkg_recent_free(recent);
        apkg_snapshot_free(snapshot); apkg_channel_free(channel);
        assert(allocations == 0);
        return 0;
    }
    if (atoi(argv[3]) == 50 || atoi(argv[3]) == 55) {
        assert(argc == 9);
        apkg_snapshot *snapshot = NULL;
        apkg_channel *channel = NULL;
        pm_common_catalogue *catalogue = NULL;
        pm_common_installed *installed = NULL;
        pm_common_library *model = (void *)1;
        if (strcmp(argv[2], "-")) {
            assert(apkg_client_channel_refresh(argv[1], argv[2], argv[5], 0, NULL,
                &snapshot, &channel, NULL) == APKG_OK);
            assert(pm_common_catalogue_copy(channel, snapshot, argv[2], &catalogue) == APKG_OK);
        }
        apkg_snapshot_free(snapshot); apkg_channel_free(channel);
        assert(pm_common_installed_read(argv[1], NULL, &installed, NULL) == APKG_OK);
        int fault = atoi(argv[8]);
        if (fault == -1) installed->generation++;
        if (fault == -2) installed->count = 4097;
        if (fault == -3) installed->snapshot.bytes[0] ^= 1;
        allocation_calls = 0; fail_at = fault > 0 ? fault : 0;
        apkg_status code = pm_common_library_build(installed, catalogue, &model);
        fail_at = 0;
        printf("status %d\n", (int)code);
        if (code) assert(!model);
        else {
            if (atoi(argv[3]) == 55) {
                const pm_common_library_row *row = (void *)1;
                apkg_status found = pm_common_library_find_name(model,argv[6],&row);
                if (found) assert(!row);
                else assert(row >= model->rows && row < model->rows + model->count);
                printf("show\t%d\t%s\t%s\t%s\n",(int)found,
                    row ? (row->installed ? "installed" : "candidate") : "",
                    row ? row->name : "", row ? row->version : "");
            }
            if (catalogue)
                printf("reported-on\t%.*s\n",(int)catalogue->channel.published_on.size,
                    catalogue->channel.published_on.data);
            for (size_t i = 1; i <= model->category_count; i++) {
                owned_text(model, (apkg_text){model->categories[i],strlen(model->categories[i])});
                if (i > 1) assert(strcmp(model->categories[i-1],model->categories[i]) < 0);
                printf("category\t%s\n", model->categories[i]);
            }
            assert(!model->categories[model->category_count+1]);
            for (size_t i = 0; i < model->count; i++) {
                const pm_common_library_row *r = pm_common_library_at(model,(pm_library_filter)atoi(argv[4]),i);
                if (!r) continue;
                assert(!!r->installed != !!r->candidate);
                if (r->candidate) assert(strstr(r->candidate->selection,"/"));
                if (r->news && r->news->recent_rank)
                    printf("news\t%s\t%s\t%llu\n",r->name,r->news->app_first_added,
                        (unsigned long long)r->news->recent_rank);
                if (pm_common_library_matches(r,(pm_library_filter)atoi(argv[4]),argv[6],argv[7]))
                    printf("row\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                        r->installed ? "installed" : "candidate",r->name,r->authority,
                        r->version,r->state,r->summary ? r->summary : "[unrecorded]",
                        r->category ? r->category : "[unrecorded]");
            }
        }
        FreeVec(model); FreeVec(installed); FreeVec(catalogue);
        assert(allocations == 0);
        return 0;
    }
    if (atoi(argv[3]) == 47) {
        /* ROOT NAME 47 CHOICE: actual common diagnostic worker. */
        assert(argc == 5 && strlen(argv[1]) < sizeof j.root && strlen(argv[2]) < sizeof j.id);
        strcpy(j.root, argv[1]); strcpy(j.id, argv[2]);
        j.kind = job_for_common_rexx(RXC_VERIFY);
        assert(j.kind == JOB_VERIFY && job_uses_common_engine(j.kind));
        j.done = &final; running = &j;
        if (atoi(argv[4]) == 1) j.cancel = 1;
        if (atoi(argv[4]) == 2) fail_at = 1;
        pm_common_install(&j, &error);
        printf("status %d\n%s", final.common_status, final.common_record ? final.common_record : "");
        FreeVec(final.common_record);
        assert(previews == 0 && allocations == 0);
        return 0;
    }
    if (atoi(argv[3]) == 45 || atoi(argv[3]) == 46) {
        /* ROOT ENDPOINT 45 WAIT CHANNEL-ID MATCH: retain, then query offline. */
        assert(argc == 7);
        assert(strlen(argv[1]) < sizeof j.root && strlen(argv[2]) < sizeof j.common_package && strlen(argv[5]) < sizeof j.common_pool);
        strcpy(j.root, argv[1]); strcpy(j.common_package, argv[2]); strcpy(j.common_pool, argv[5]);
        j.kind = JOB_COMMON_DISCOVER; j.done = &final; running = &j;
        pm_common_install(&j, &error);
        assert(final.common_status == APKG_OK);
        if (atoi(argv[4])) {
            char line[2]; puts("loaded"); fflush(stdout);
            assert(fgets(line, sizeof line, stdin));
        }
        char *text = NULL;
        assert(pm_common_rexx_list(j.root, NULL, NULL, &text) == APKG_NOT_FOUND && !text);
        apkg_status code = atoi(argv[3]) == 46
            ? pm_common_rexx_requirements(j.root, final.common_catalogue, argv[6], &text)
            : pm_common_rexx_list(j.root, final.common_catalogue, argv[6], &text);
        FreeVec(final.common_catalogue);
        printf("status %d\n", code);
        if (!code) { assert(text && strlen(text) <= RX_MAXRESULT); puts(text); }
        else assert(!text);
        apkg_client_text_free(text);
        assert(allocations == 0);
        return 0;
    }
    if (atoi(argv[3]) == 30 || atoi(argv[3]) == 31) {
        assert(strlen(argv[1]) < sizeof j.root &&
               strlen(argv[2]) < sizeof j.common_package);
        strcpy(j.root, argv[1]);
        strcpy(j.common_package, argv[2]);
        j.kind =
            atoi(argv[3]) == 30 ? JOB_COMMON_DISCOVER : JOB_COMMON_SOURCE_LOAD;
        if (j.kind == JOB_COMMON_DISCOVER) {
            assert(argc == 6);
            assert(strlen(argv[5]) < sizeof j.common_pool);
            strcpy(j.common_pool, argv[5]);
        }
        j.done = &final;
        running = &j;
        pm_common_install(&j, &error);
        printf("status %d\n", final.common_status);
        if (!final.common_status) {
            for (size_t i = 0; i < final.common_catalogue->count; i++) {
                const pm_common_row *r = &final.common_catalogue->rows[i];
                owned_text(final.common_catalogue,
                           (apkg_text){r->detail, strlen(r->detail)});
                printf("row %s %llu %llu %llu\n%s\n", r->name,
                       (unsigned long long)r->review_observations,
                       (unsigned long long)r->runtime_observations,
                       (unsigned long long)r->revocations, r->detail);
                assert(pm_common_row_matches(
                           r, PM_CATALOGUE_REVIEW_OBSERVATIONS, NULL) ==
                       (r->review_observations != 0));
                assert(pm_common_row_matches(
                           r, PM_CATALOGUE_RUNTIME_OBSERVATIONS, NULL) ==
                       (r->runtime_observations != 0));
                assert(pm_common_row_matches(r, PM_CATALOGUE_REVOCATIONS,
                                             NULL) == (r->revocations != 0));
            }
        } else
            assert(!final.common_catalogue);
        FreeVec(final.common_catalogue);
        assert(allocations == 0);
        return 0;
    }
    if (atoi(argv[3]) >= 42 && atoi(argv[3]) <= 44) {
        /* ROOT NAME MODE CHOICE [ENDPOINT CHANNEL-ID]. */
        int mode = atoi(argv[3]);
        assert(argc == (mode == 42 ? 7 : 5));
        assert(strlen(argv[1]) < sizeof j.root && strlen(argv[2]) < sizeof j.id);
        strcpy(j.root, argv[1]); strcpy(j.id, argv[2]);
        j.kind = mode == 42 ? JOB_COMMON_SOURCE_ADD : mode == 43 ? JOB_COMMON_SOURCE_REMOVE : JOB_COMMON_SOURCE_LIST;
        assert(job_uses_common_engine(j.kind));
        if (mode == 42) {
            assert(strlen(argv[5]) < sizeof j.common_package && strlen(argv[6]) < sizeof j.common_pool);
            strcpy(j.common_package, argv[5]); strcpy(j.common_pool, argv[6]);
        }
        choice = atoi(argv[4]);
        if (choice == 3) fail_at = 1;
        if (choice == 4) j.cancel = 1;
        j.done = &final; running = &j;
        pm_common_install(&j, &error);
        printf("status %d outcome %d previews %d\n%s", final.common_status,
               final.common_outcome, previews, final.common_record ? final.common_record : "");
        FreeVec(final.common_record);
        assert(allocations == 0);
        return 0;
    }
    if (atoi(argv[3]) == 41) {
        /* ROOT NAME 41: install by name from the root's source list. */
        assert(argc == 4 && strlen(argv[1]) < sizeof j.root &&
               strlen(argv[2]) < sizeof j.id);
        strcpy(j.root, argv[1]);
        strcpy(j.id, argv[2]);
        j.kind = job_for_common_rexx(RXC_INSTALL);
        assert(j.kind == JOB_COMMON_SOURCE_INSTALL && job_for_common_rexx(RXC_UPGRADE) == j.kind);
        assert(job_uses_common_engine(j.kind));
        j.done = &final;
        running = &j;
        choice = 1;
        pm_common_install(&j, &error);
        printf("status %d outcome %d previews %d\n", final.common_status,
               final.common_outcome, previews);
        FreeVec(final.common_record);
        assert(allocations == 0);
        return 0;
    }
    if (atoi(argv[3]) == 40 || atoi(argv[3]) == 49) {
        /* ROOT ENDPOINT 40 CHECK CHANNEL-ID EXECUTABLE: self-update through
         * the actual worker bridge; the window approves the plan. */
        assert(argc == 7 && strlen(argv[1]) < sizeof j.root &&
               strlen(argv[2]) < sizeof j.common_package &&
               strlen(argv[5]) < sizeof j.common_pool &&
               strlen(argv[6]) < sizeof j.common_executable);
        strcpy(j.root, argv[1]);
        strcpy(j.common_package, argv[2]);
        strcpy(j.common_pool, argv[5]);
        strcpy(j.common_executable, argv[6]);
        j.kind = j.common_action == 1 || atoi(argv[3]) == 49 ? JOB_CHECK_RELEASE : JOB_COMMON_SELF_UPDATE;
        assert(job_uses_common_engine(j.kind));
        j.done = &final;
        running = &j;
        choice = 1;
        pm_common_install(&j, &error);
        printf("status %d outcome %d previews %d\n%s", final.common_status,
               final.common_outcome, previews,
               final.common_record ? final.common_record : "");
        assert(previews == (j.kind == JOB_CHECK_RELEASE || j.common_action == 1 || final.common_status ? 0 : 1));
        FreeVec(final.common_record);
        assert(allocations == 0);
        return 0;
    }
    if (atoi(argv[3]) == 29) {
        const char *names[] = {
            "descriptor.json",  "signature.json",
            "first.json",       "first.signature.json",
            "attestation.json", "attestation.signature.json"};
        apkg_bytes files[6];
        for (size_t i = 0; i < 6; i++)
            files[i] = claim_file(argv[2], names[i]);
        apkg_open_options options = {0};
        options.contract_version = APKG_CONTRACT_VERSION;
        options.struct_size = sizeof options;
        options.root = (apkg_text){argv[1], strlen(argv[1])};
        apkg_ctx *ctx = NULL;
        apkg_snapshot *snapshot = NULL;
        apkg_attestation *first = NULL, *last = NULL;
        char *text = NULL;
        assert(apkg_open(&options, &ctx, NULL) == APKG_OK);
        assert(apkg_snapshot_read(ctx, &snapshot, NULL) == APKG_OK);
        apkg_status code =
            apkg_attestation_read(snapshot, files[0], files[1], files[2],
                                  files[3], NULL, &first, NULL);
        if (!code)
            code = apkg_attestation_read(snapshot, files[0], files[1], files[4],
                                         files[5], first, &last, NULL);
        apkg_attestation_free(first);
        apkg_snapshot_free(snapshot);
        apkg_close(ctx);
        for (size_t i = 0; i < 6; i++)
            free((void *)files[i].data);
        if (!code)
            assert(apkg_attestation_format(last, &text, NULL) == APKG_OK);
        else
            assert(!last && !text);
        printf("status %d\n", (int)code);
        if (text)
            puts(text);
        apkg_client_text_free(text);
        apkg_attestation_free(last);
        return 0;
    }
    if (atoi(argv[3]) == 28) {
        assert(strlen(argv[1]) < sizeof j.root &&
               strlen(argv[2]) < sizeof j.common_package);
        strcpy(j.root, argv[1]);
        strcpy(j.common_package, argv[2]);
        j.kind = JOB_COMMON_ATTESTATION;
        j.done = &final;
        running = &j;
        if (argc >= 5) {
            int fault = atoi(argv[4]);
            if (fault < 0)
                j.cancel = 1;
            else
                fail_at = fault;
        }
        pm_common_install(&j, &error);
        printf("status %d\n", final.common_status);
        if (!final.common_status) {
            assert(final.common_record);
            puts(final.common_record);
        } else
            assert(!final.common_record);
        FreeVec(final.common_record);
        assert(allocations == 0);
        return 0;
    }
    if (atoi(argv[3]) == 24) {
        assert(strlen(argv[1]) < sizeof j.root &&
               strlen(argv[2]) < sizeof j.common_package);
        strcpy(j.root, argv[1]);
        strcpy(j.common_package, argv[2]);
        j.kind = JOB_COMMON_SOURCE_LOAD;
        j.done = &final;
        running = &j;
        if (argc >= 5) {
            int fault = atoi(argv[4]);
            if (fault < 0)
                j.cancel = 1;
            else
                fail_at = fault;
        }
        pm_common_install(&j, &error);
        printf("status %d\n", final.common_status);
        if (!final.common_status) {
            char *text = NULL;
            assert(apkg_update_summary_format(&final.common_catalogue->updates,
                                              NULL, &text, NULL) == APKG_OK);
            puts(text);
            apkg_client_text_free(text);
            assert(final.common_catalogue->observation.size);
            owned_text(final.common_catalogue,
                       (apkg_text){final.common_catalogue->endpoint,
                                   strlen(final.common_catalogue->endpoint)});
        } else
            assert(!final.common_catalogue);
        FreeVec(final.common_catalogue);
        assert(allocations == 0);
        return 0;
    }
    if (atoi(argv[3]) == 19) {
        assert(argc == 6 || argc == 7);
        apkg_snapshot *snapshot = NULL;
        apkg_channel *channel = NULL;
        apkg_update_summary *summary = NULL;
        assert(apkg_client_channel_refresh(argv[1], argv[2], argv[5], 0, NULL,
                                           &snapshot, &channel,
                                           NULL) == APKG_OK);
        assert(apkg_channel_summarize(snapshot, channel, &summary, NULL) ==
               APKG_OK);
        apkg_snapshot_free(snapshot);
        apkg_channel_free(channel);
        char *text = NULL;
        apkg_status status =
            apkg_update_summary_format(apkg_update_summary_get(summary),
                                       argc == 7 ? argv[6] : NULL, &text, NULL);
        printf("status %d\n", (int)status);
        if (!status)
            puts(text);
        else
            assert(!text);
        apkg_client_text_free(text);
        apkg_update_summary_free(summary);
        return 0;
    }
    if (atoi(argv[3]) == 15) {
        apkg_open_options options = {0};
        options.contract_version = APKG_CONTRACT_VERSION;
        options.struct_size = sizeof options;
        options.root = (apkg_text){argv[1], strlen(argv[1])};
        apkg_ctx *ctx = NULL;
        apkg_snapshot *snapshot = NULL;
        apkg_inspection *inspection = NULL;
        assert(apkg_open(&options, &ctx, NULL) == APKG_OK);
        assert(apkg_snapshot_read(ctx, &snapshot, NULL) == APKG_OK);
        for (uint64_t i = 0; i < apkg_snapshot_installed_count(snapshot); i++) {
            const apkg_installed_view *row = NULL;
            assert(apkg_snapshot_installed_at(snapshot, i, &row, NULL) ==
                   APKG_OK);
            if (!strcmp(row->name.value.data, argv[2])) {
                assert(apkg_snapshot_inspect(snapshot, i, &inspection, NULL) ==
                       APKG_OK);
                break;
            }
        }
        assert(inspection);
        apkg_snapshot_free(snapshot);
        apkg_close(ctx);
        const apkg_inspection_view *view = apkg_inspection_get(inspection);
        assert(!strcmp(view->name.data, argv[2]));
        assert(view->descriptor.size && view->signature_envelope.size &&
               view->relations.size);
        assert(view->review == APKG_UNKNOWN &&
               view->runtime_testing == APKG_UNKNOWN);
        char *text = NULL;
        assert(apkg_inspection_format(inspection, argc >= 5 ? argv[4] : NULL,
                                      &text, NULL) == APKG_OK);
        puts("status 0");
        puts(text);
        apkg_client_text_free(text);
        apkg_inspection_free(inspection);
        return 0;
    }
    if (atoi(argv[3]) == 14) {
        char *text = NULL;
        apkg_status code = pm_common_rexx_info(
            argv[1], argv[2], argc >= 5 ? argv[4] : NULL, &text);
        printf("status %d\n", (int)code);
        if (text)
            puts(text);
        if (code)
            assert(!text);
        apkg_client_text_free(text);
        assert(allocations == 0);
        return 0;
    }
    if (argc >= 6) {
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
    if ((choice >= 9 && choice <= 11) || choice == 16 || choice == 17 ||
        choice == 18 || (choice >= 20 && choice <= 23) || choice == 25) {
        assert(argc == 6 || ((choice == 17 || choice == 18 ||
                              (choice >= 20 && choice <= 23) || choice == 25) &&
                             argc == 7));
        j.kind = JOB_COMMON_DISCOVER;
        fail_at = choice == 10 ? 1 : 0;
        assert(apkg_cancel_create(&j.common_cancel) == APKG_OK);
        if (choice == 11)
            apkg_cancel_request(j.common_cancel);
        result = pm_common_install(&j, &error);
        apkg_cancel_free(j.common_cancel);
        j.common_cancel = NULL;
        assert(previews == 0);
        if (choice == 11)
            assert(progress_count == 0);
        if (choice == 9 || choice == 16 || choice == 17 || choice == 18 ||
            (choice >= 20 && choice <= 23) || choice == 25) {
            assert(result == PKG_OK && final.common_status == APKG_OK);
            assert(final.common_catalogue);
            assert(!strcmp(final.common_catalogue->endpoint, argv[2]));
            assert(strstr(final.common_catalogue->summary, argv[5]));
            assert(strstr(final.common_catalogue->summary,
                          "online freshness: unknown"));
            assert(final.common_catalogue->channel.signature ==
                   APKG_SIGNATURE_VALID);
            owned_text(final.common_catalogue,
                       (apkg_text){(const char *)
                                       final.common_catalogue->observation.data,
                                   final.common_catalogue->observation.size});
            owned_text(
                final.common_catalogue,
                (apkg_text){
                    (const char *)
                        final.common_catalogue->observation_signature.data,
                    final.common_catalogue->observation_signature.size});
            for (uint64_t i = 0;
                 i < final.common_catalogue->updates.entry_count; i++) {
                const apkg_update_entry_view *r =
                    &final.common_catalogue->updates.entries[i];
                owned_text(final.common_catalogue, r->authority);
                owned_text(final.common_catalogue, r->name);
                owned_text(final.common_catalogue, r->target);
                owned_text(final.common_catalogue, r->installed_version);
            }
            if (choice == 25) {
                assert(argc == 7);
                apkg_status code = pm_common_source_save(
                    j.root, argv[6], j.common_package, j.common_pool,
                    final.common_catalogue);
                printf("status %d\n", (int)code);
                FreeVec(final.common_catalogue);
                assert(allocations == 0);
                return 0;
            }
            if (choice >= 20 && choice <= 23) {
                pm_common_catalogue *old = final.common_catalogue,
                                    *replacement = NULL;
                size_t old_size = last_public_size;
                void *before = malloc(old_size);
                assert(before);
                memcpy(before, old, old_size);
                char input[2], *text = NULL;
                puts("loaded");
                fflush(stdout);
                assert(fgets(input, sizeof input, stdin));
                apkg_status code;
                if (choice == 21) {
                    code = pm_common_rexx_outdated(
                        j.root, old, argc == 7 ? argv[6] : "newer", &text,
                        &replacement);
                } else {
                    final.common_catalogue = NULL;
                    j.kind = JOB_COMMON_RECOMPARE;
                    j.common_observation = old;
                    if (choice == 22)
                        j.cancel = 1;
                    if (choice == 23)
                        fail_at = allocation_calls + 1;
                    result = pm_common_install(&j, &error);
                    code = final.common_status;
                    assert((result == PKG_OK) == (code == APKG_OK));
                    replacement = final.common_catalogue;
                    if (!code)
                        assert(apkg_update_summary_format(
                                   &replacement->updates,
                                   argc == 7 ? argv[6] : "newer", &text,
                                   NULL) == APKG_OK);
                }
                assert(!memcmp(before, old, old_size));
                free(before);
                printf("status %d\n", (int)code);
                if (!code) {
                    assert(replacement && replacement != old);
                    puts(text);
                    printf("generation %llu\n",
                           (unsigned long long)replacement->root_generation);
                    for (size_t i = 0; i < replacement->count; i++) {
                        const pm_common_row *r = &replacement->rows[i];
                        owned_text(replacement,
                                   (apkg_text){r->name, strlen(r->name)});
                        owned_text(replacement,
                                   (apkg_text){r->detail, strlen(r->detail)});
                        printf("%s\t%s\t%s\t%d\t%d\n", r->selection, r->name,
                               apkg_candidate_state_name(r->comparison),
                               r->author_trust, r->target_matches);
                    }
                } else
                    assert(!replacement && !text);
                FreeVec(
                    old); /* independent replacement must survive its input */
                if (replacement) {
                    assert(replacement->observation.size);
                    assert(strstr(replacement->summary,
                                  "online freshness: unknown"));
                }
                apkg_client_text_free(text);
                FreeVec(replacement);
                assert(allocations == 0);
                return 0;
            }
            if (choice == 17 || choice == 18) {
                if (choice == 18) {
                    char input[2];
                    puts("loaded");
                    fflush(stdout);
                    assert(fgets(input, sizeof input, stdin));
                }
                char *text = NULL;
                apkg_status status = pm_common_rexx_outdated(
                    j.root, final.common_catalogue, argc == 7 ? argv[6] : NULL,
                    &text, NULL);
                printf("status %d\n", (int)status);
                if (status)
                    assert(!text);
                else
                    puts(text);
                apkg_client_text_free(text);
                FreeVec(final.common_catalogue);
                assert(allocations == 0);
                return 0;
            }
            if (choice == 16)
                printf("generation %llu\n",
                       (unsigned long long)
                           final.common_catalogue->root_generation);
            for (size_t i = 0; i < final.common_catalogue->count; i++) {
                pm_common_row *row = &final.common_catalogue->rows[i];
                assert(strlen(row->selection) == 162);
                assert(strstr(row->detail,
                              "Current review approval: not asserted"));
                assert(strstr(row->detail,
                              "Current runtime approval: not asserted"));
                assert(strstr(row->detail, row->name));
                owned_text(final.common_catalogue, (apkg_text){row->summary, strlen(row->summary)});
                owned_text(final.common_catalogue, (apkg_text){row->category, strlen(row->category)});
                assert(strstr(row->detail, "Summary: "));
                assert(strstr(row->detail, "Category: "));
                if (*row->summary)
                    assert(pm_common_row_matches(row, PM_CATALOGUE_ALL, row->summary));
                if (*row->category)
                    assert(pm_common_row_matches(row, PM_CATALOGUE_ALL, row->category));
                owned_text(final.common_catalogue,
                           (apkg_text){row->installed_version,
                                       strlen(row->installed_version)});
                if (choice == 16) {
                    unsigned mask = 0;
                    for (int f = 0; f < PM_CATALOGUE_FILTER_COUNT; f++)
                        if (pm_common_row_matches(row, (pm_catalogue_filter)f,
                                                  NULL))
                            mask |= 1u << f;
                    assert(!pm_common_row_matches(
                        row, PM_CATALOGUE_ALL,
                        "no such package/authority/target"));
                    assert(pm_common_row_matches(row, PM_CATALOGUE_ALL,
                                                 row->name));
                    printf("%s\t%s\t%s\t%s\t%s\t%llu\t%d\t%d\t%u\n",
                           row->selection, row->name, row->version,
                           apkg_candidate_state_name(row->comparison),
                           row->installed_version,
                           (unsigned long long)row->installed_revision,
                           row->installed_revision_state,
                           pm_common_row_reviewable(row), mask);
                    continue;
                }
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
