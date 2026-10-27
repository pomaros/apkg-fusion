/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int confirm(void *user, const apkg_plan *plan, const char *text) {
    const char *expected = user;
    char digest[65];
    ak_tohex(apkg_plan_get(plan)->plan_digest.bytes, 32, digest);
    puts(text);
    if (expected && !strcmp(expected, digest))
        return 1;
    fprintf(stderr, "Confirmation required: repeat with --confirm %s\n",
            digest);
    return 0;
}
static int json_preview(void *user, const apkg_plan *plan,
                        const char *presentation) {
    (void)user;
    (void)presentation;
    apkg_bytes bytes = apkg_plan_get(plan)->canonical;
    fwrite(bytes.data, 1, bytes.size, stdout);
    putchar('\n');
    return 0;
}
static void report(apkg_status code, apkg_result *result, apkg_error *error) {
    if (error) {
        const apkg_error_view *e = apkg_error_get(error);
        fprintf(stderr, "%.*s: %.*s\n", (int)e->message.size,
                e->message.data ? e->message.data : "", (int)e->subject.size,
                e->subject.data ? e->subject.data : "");
    }
    printf("status=%d outcome=%d\n", (int)code,
           result ? (int)apkg_result_get(result)->outcome : APKG_UNCHANGED);
    if (result && apkg_result_get(result)->record.size)
        printf("record=%.*s\n", (int)apkg_result_get(result)->record.size,
               apkg_result_get(result)->record.data);
}
static int apkg_main(int argc, char **argv) {
    apkg_open_options o = {0};
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    apkg_plan *p = NULL;
    apkg_approval *a = NULL;
    apkg_result *r = NULL;
    apkg_error *e = NULL;
    apkg_status code = APKG_INVALID;
    unsigned char *policy = NULL;
    size_t size = 0;
    char *text = NULL;
    const char *confirmation = NULL;
    int initialize_yes = 0;
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts("apkg-fusion 0.1.0-alpha (common engine, contract 1)");
        return APKG_OK;
    }
    if (argc < 3) {
        fprintf(
            argc == 2 && !strcmp(argv[1], "--help") ? stdout : stderr,
            "Usage: apkg inspect ROOT | recover ROOT | init ROOT TARGET "
            "POLICY "
            "[--yes] | install ROOT "
            "PACKAGE-DIRECTORY [--confirm "
            "DIGEST]\nadopt-set ROOT MANIFEST [--json | --confirm DIGEST]\nadopt ROOT ARTIFACT --evidence FILE [--confirm DIGEST]\nremove|repair ROOT NAME | upgrade ROOT PACKAGE-DIRECTORY "
            "| rollback ROOT HISTORY\nresolve ROOT PACKAGE-DIRECTORY [--pool "
            "DIRECTORY] [--confirm DIGEST]\nlist|show ROOT [MATCH] | info ROOT NAME [FIELD] | requirements ROOT ENDPOINT CHANNEL-ID NAME | verify ROOT NAME | "
            "bundle-check|run-bundle FOLDER TARGET\nsource-inventory FOLDER | "
            "source-entry PATH | "
            "channel-inspect|channel-open|channel-updates ROOT ENDPOINT CHANNEL-ID "
            "[MIN-SEQUENCE]\nsource-save PROFILE ENDPOINT CHANNEL-ID\n"
            "source-cache ROOT PROFILE ENDPOINT CHANNEL-ID\nsource-show PROFILE\n"
            "channel-validate JSONFILE\nattestation-check ROOT FOLDER\nsource-outdated ROOT PROFILE [FIELD]\noutdated ROOT ENDPOINT CHANNEL-ID [FIELD]\n"
            "keygen|key-public KEYFILE | sign-object KEYFILE "
            "descriptor|channel|attestation "
            "JSONFILE\nverify-object descriptor|channel|attestation JSONFILE SIGNATURE\n"
            "archive-check ROOT ARCHIVE\nchannel-fetch ROOT ENDPOINT "
            "CHANNEL-ID INDEX [MIN-SEQUENCE]\nchannel-install ROOT ENDPOINT "
            "CHANNEL-ID SNAPSHOT-SHA256 DESCRIPTOR-SHA256 [--recommends] "
            "[--confirm DIGEST]\nchannel-accept ROOT ENDPOINT "
            "CHANNEL-ID [--confirm DIGEST]\nself-update ROOT ENDPOINT "
            "CHANNEL-ID EXECUTABLE [--check] [--confirm DIGEST]\npolicy ROOT "
            "POLICY-FILE [--confirm DIGEST]\nsource-add ROOT NAME ENDPOINT "
            "CHANNEL-ID | source-remove ROOT NAME | source-install ROOT NAME "
            "[--recommends], each [--confirm DIGEST]\nsource-list ROOT\n");
        return argc == 2 && !strcmp(argv[1], "--help") ? APKG_OK : APKG_INVALID;
    }
    if (!strcmp(argv[1], "channel-install")) {
        if (argc < 7 || argc > 10 || strlen(argv[4]) != 32 ||
            strlen(argv[5]) != 64 || strlen(argv[6]) != 64)
            return APKG_INVALID;
        int recommendations = 0;
        for (int i = 7; i < argc; i++) {
            if (!strcmp(argv[i], "--recommends") && !recommendations)
                recommendations = 1;
            else if (!strcmp(argv[i], "--confirm") && !confirmation &&
                     i + 1 < argc)
                confirmation = argv[++i];
            else
                return APKG_INVALID;
        }
        char selection[163];
        snprintf(selection, sizeof selection, "%s/%s/%s", argv[4], argv[5],
                 argv[6]);
        code = apkg_client_channel_apply(argv[2], argv[3], selection,
                                         recommendations, confirm,
                                         (void *)confirmation, NULL, &r, &e);
        goto done;
    }
    if (!strcmp(argv[1], "source-list")) {
        char *list = NULL;
        if (argc != 3)
            return APKG_INVALID;
        code = apkg_client_source_list(argv[2], &list, &e);
        if (!code)
            fputs(list, stdout);
        apkg_client_text_free(list);
        goto done;
    }
    if (!strcmp(argv[1], "source-add") || !strcmp(argv[1], "source-remove") ||
        !strcmp(argv[1], "source-install")) {
        /* source-add ROOT NAME ENDPOINT CHANNEL-ID | source-remove ROOT NAME |
         * source-install ROOT NAME [--recommends]; then [--confirm DIGEST] */
        int add = !strcmp(argv[1], "source-add"),
            install = !strcmp(argv[1], "source-install"), recommends = 0;
        int fixed = add ? 6 : 4;
        if (argc < fixed)
            return APKG_INVALID;
        for (int i = fixed; i < argc; i++) {
            if (install && !strcmp(argv[i], "--recommends") && !recommends)
                recommends = 1;
            else if (!strcmp(argv[i], "--confirm") && !confirmation &&
                     i + 1 < argc)
                confirmation = argv[++i];
            else
                return APKG_INVALID;
        }
        code = add ? apkg_client_source_add(argv[2], argv[3], argv[4], argv[5],
                                            confirm, (void *)confirmation, NULL,
                                            &r, &e)
               : install
                   ? apkg_client_source_install(argv[2], argv[3], recommends,
                                                confirm, (void *)confirmation,
                                                NULL, &r, &e)
                   : apkg_client_source_remove(argv[2], argv[3], confirm,
                                               (void *)confirmation, NULL, &r,
                                               &e);
        goto done;
    }
    if (!strcmp(argv[1], "self-update")) {
        /* self-update ROOT ENDPOINT CHANNEL-ID EXECUTABLE [--check]
         *             [--confirm PLAN-DIGEST] */
        int check = 0;
        char *text = NULL;
        if (argc < 6 || strlen(argv[4]) != 32)
            return APKG_INVALID;
        for (int i = 6; i < argc; i++) {
            if (!strcmp(argv[i], "--check") && !check)
                check = 1;
            else if (!strcmp(argv[i], "--confirm") && !confirmation &&
                     i + 1 < argc)
                confirmation = argv[++i];
            else
                return APKG_INVALID;
        }
        code = apkg_client_self_update(argv[2], argv[3], argv[4], argv[5],
                                       check, confirm, (void *)confirmation,
                                       NULL, &text, check ? NULL : &r, &e);
        if (text)
            fputs(text, stdout);
        apkg_client_text_free(text);
        if (check) {
            report(code, NULL, e);
            apkg_error_free(e);
            return code;
        }
        goto done;
    }
    if (!strcmp(argv[1], "adopt-set")) {
        int json = argc == 5 && !strcmp(argv[4], "--json");
        if (argc != 4 && !json &&
            !(argc == 6 && !strcmp(argv[4], "--confirm")))
            return APKG_INVALID;
        code = apkg_client_apply(APKG_CLIENT_ADOPT_SET, argv[2], argv[3], NULL,
                                 json ? json_preview : confirm,
                                 argc == 6 ? argv[5] : NULL, NULL, &r, &e);
        if (json) {
            if (code != APKG_CANCELLED)
                fprintf(stderr, "adoption preview status=%d\n", code);
            apkg_error_free(e);
            apkg_result_free(r);
            return code;
        }
        goto done;
    }
    if (!strcmp(argv[1], "channel-accept")) {
        if (argc != 5 && argc != 7)
            return APKG_INVALID;
        if (argc == 7) {
            if (strcmp(argv[5], "--confirm"))
                return APKG_INVALID;
            confirmation = argv[6];
        }
        apkg_channel *channel = NULL;
        apkg_id id;
        if (ak_hex(argv[4], id.bytes, 16))
            return APKG_INVALID;
        o.contract_version = 1;
        o.struct_size = sizeof o;
        o.root.data = argv[2];
        o.root.size = strlen(argv[2]);
        o.mode = APKG_WRITE;
        code = apkg_open(&o, &c, &e);
        apkg_text source = {argv[3], strlen(argv[3])};
        if (!code)
            code =
                ak_http_url(argv[3])
                    ? apkg_channel_open_http(c, source, &id, 0, NULL, &channel,
                                             &e)
                    : apkg_channel_open_file(c, source, &id, 0, &channel, &e);
        if (!code)
            code = apkg_snapshot_read(c, &s, &e);
        if (!code)
            code = apkg_plan_channel_accept(c, s, channel, &p, &e);
        apkg_channel_free(channel);
        if (!code)
            code = apkg_plan_format(p, &text);
        if (!code && !confirm((void *)confirmation, p, text))
            code = APKG_CANCELLED;
        if (!code)
            code =
                apkg_plan_approve(c, p, &apkg_plan_get(p)->plan_digest, &a, &e);
        if (!code)
            code = apkg_execute(c, p, a, NULL, &r, &e);
        goto done;
    }
    if (!strcmp(argv[1], "channel-fetch")) {
        if (argc != 6 && argc != 7)
            return APKG_INVALID;
        uint64_t index = 0, floor = 0;
        char *end;
        for (int i = 5; i < argc; i++) {
            for (const char *q = argv[i]; *q; q++)
                if (*q < '0' || *q > '9')
                    return APKG_INVALID;
            errno = 0;
            unsigned long long value = strtoull(argv[i], &end, 10);
            if (errno || !*argv[i] || *end)
                return APKG_INVALID;
            if (i == 5)
                index = value;
            else
                floor = value;
        }
        apkg_channel *channel = NULL;
        apkg_package *package = NULL;
        apkg_id id;
        if (ak_hex(argv[4], id.bytes, 16))
            return APKG_INVALID;
        o.contract_version = 1;
        o.struct_size = sizeof o;
        o.root.data = argv[2];
        o.root.size = strlen(argv[2]);
        o.mode = APKG_WRITE;
        code = apkg_open(&o, &c, &e);
        apkg_text endpoint = {argv[3], strlen(argv[3])};
        if (!code)
            code = ak_http_url(argv[3])
                       ? apkg_channel_open_http(c, endpoint, &id, floor, NULL,
                                                &channel, &e)
                       : apkg_channel_open_file(c, endpoint, &id, floor,
                                                &channel, &e);
        if (!code)
            code = apkg_channel_fetch(c, channel, index, NULL, &package, &e);
        if (!code) {
            const apkg_channel_entry_view *row = NULL;
            char archive[65], descriptor[65];
            apkg_channel_entry_at(channel, index, &row, NULL);
            ak_tohex(row->archive_digest.bytes, 32, archive);
            ak_tohex(package->digest.bytes, 32, descriptor);
            printf(
                "{\"archive_sha256\":\"%s\",\"descriptor_sha256\":\"%s\","
                "\"managed_commit\":false,\"name\":\"%s\",\"prepared\":true}\n",
                archive, descriptor, package->name);
        } else
            report(code, NULL, e);
        apkg_package_free(package);
        apkg_channel_free(channel);
        apkg_error_free(e);
        apkg_close(c);
        return (int)code;
    }
    if (!strcmp(argv[1], "verify-object")) {
        code = ak_verify_command(argc, argv, &text);
        if (!code)
            puts(text);
        else
            fprintf(stderr, "verification status=%d\n", (int)code);
        free(text);
        return (int)code;
    }
    if (!strcmp(argv[1], "archive-check")) {
        apkg_package *package = NULL;
        if (argc != 4)
            return APKG_INVALID;
        o.contract_version = 1;
        o.struct_size = sizeof o;
        o.root.data = argv[2];
        o.root.size = strlen(argv[2]);
        o.mode = APKG_WRITE;
        code = apkg_open(&o, &c, &e);
        if (!code) {
            apkg_text archive = {argv[3], strlen(argv[3])};
            code = apkg_package_import(c, archive, NULL, &package, &e);
        }
        if (!code)
            printf("{\"descriptor\":%s,\"signature\":%s}\n", package->raw,
                   package->signature);
        else
            report(code, NULL, e);
        apkg_package_free(package);
        apkg_error_free(e);
        apkg_close(c);
        return (int)code;
    }
    if (!strcmp(argv[1], "channel-validate")) {
        if (argc!=3) return APKG_INVALID;
        unsigned char *bytes=NULL; size_t size=0; ak_json object={0};
        if (ak_read(argv[2],&bytes,&size,AK_MAX/8)) code=errno==ENOMEM ? APKG_NOMEM : errno==EFBIG ? APKG_LIMIT : APKG_IO;
        else { int parsed=ak_json_parse(bytes,size,&object); code=parsed ? (parsed==-2 ? APKG_NOMEM : APKG_INVALID) : ak_channel_validate(&object); }
        ak_json_free(&object); free(bytes); report(code,NULL,NULL); return code;
    }
    if (!strcmp(argv[1], "attestation-check")) {
        if (argc != 4) return APKG_INVALID;
        code=apkg_client_attestation(argv[2],argv[3],&text,&e);
        if (!code) puts(text); else report(code,NULL,e);
        apkg_client_text_free(text); apkg_error_free(e); return code;
    }
    if (!strcmp(argv[1], "source-save") || !strcmp(argv[1], "source-cache")) {
        int cached=!strcmp(argv[1],"source-cache");
        if (argc!=(cached ? 6 : 5)) return APKG_INVALID;
        const char *path=argv[cached ? 3 : 2], *endpoint=argv[cached ? 4 : 3], *identity=argv[cached ? 5 : 4];
        apkg_id id; if (ak_hex(identity,id.bytes,16)) return APKG_INVALID;
        apkg_snapshot *snapshot=NULL; apkg_channel *channel=NULL;
        code=cached ? apkg_client_channel_refresh(argv[2],endpoint,identity,0,NULL,&snapshot,&channel,&e) : APKG_OK;
        if (!code) code=apkg_source_config_save(path,endpoint,&id,channel,&e);
        apkg_snapshot_free(snapshot); apkg_channel_free(channel);
        report(code,NULL,e); apkg_error_free(e); return code;
    }
    if (!strcmp(argv[1], "source-show") || !strcmp(argv[1], "source-outdated")) {
        int outdated=!strcmp(argv[1],"source-outdated");
        if ((!outdated && argc!=3) || (outdated && argc!=4 && argc!=5)) return APKG_INVALID;
        apkg_source_config *config=NULL;
        code=apkg_source_config_read(argv[outdated ? 3 : 2],&config,&e);
        if (!code && !outdated) {
            const apkg_source_config_view *v=apkg_source_config_get(config);
            char id[33]; ak_tohex(v->id.bytes,16,id);
            printf("endpoint %.*s\nchannel %s\nobservation %s\nfreshness unknown\n",(int)v->endpoint.size,v->endpoint.data,id,v->observation.size ? "retained" : "absent");
        } else if (!code) {
            apkg_snapshot *snapshot=NULL; apkg_channel *channel=NULL; apkg_update_summary *summary=NULL;
            code=apkg_source_config_open(config,argv[2],&snapshot,&channel,&e);
            if (!code) code=apkg_channel_summarize(snapshot,channel,&summary,&e);
            if (!code) code=apkg_update_summary_format(apkg_update_summary_get(summary),argc==5 ? argv[4] : NULL,&text,&e);
            if (!code) puts(text);
            apkg_client_text_free(text); apkg_update_summary_free(summary);
            apkg_snapshot_free(snapshot); apkg_channel_free(channel);
        }
        if (code) report(code,NULL,e);
        apkg_error_free(e); apkg_source_config_free(config); return code;
    }
    if (!strcmp(argv[1], "outdated")) {
        if (argc != 5 && argc != 6) return APKG_INVALID;
        code = apkg_client_outdated(argv[2], argv[3], argv[4], argc == 6 ? argv[5] : NULL, NULL, &text, &e);
        if (!code) puts(text); else report(code, NULL, e);
        apkg_client_text_free(text); apkg_error_free(e);
        return (int)code;
    }
    if (!strcmp(argv[1], "channel-inspect") ||
        !strcmp(argv[1], "channel-open") || !strcmp(argv[1], "channel-updates")) {
        if (argc != 5 && argc != 6)
            return APKG_INVALID;
        uint64_t floor = 0;
        if (argc == 6) {
            char *end;
            for (const char *q = argv[5]; *q; q++)
                if (*q < '0' || *q > '9')
                    return APKG_INVALID;
            errno = 0;
            floor = strtoull(argv[5], &end, 10);
            if (errno || !*argv[5] || *end)
                return APKG_INVALID;
        }
        code = !strcmp(argv[1], "channel-updates")
                   ? apkg_client_channel_updates(argv[2], argv[3], argv[4], floor, NULL, &text, &e)
                   : !strcmp(argv[1], "channel-open")
                   ? apkg_client_channel_query(argv[2], argv[3], argv[4], floor,
                                               NULL, &text, &e)
                   : ak_channel_inspect(argv[2], argv[3], argv[4], floor, 0,
                                        &text, &e);
        if (!code)
            puts(text);
        else
            report(code, NULL, e);
        free(text);
        apkg_error_free(e);
        return (int)code;
    }
    if (!strcmp(argv[1], "keygen") || !strcmp(argv[1], "key-public") ||
        !strcmp(argv[1], "sign-object")) {
        code = ak_author_command(argc, argv, &text);
        if (!code)
            puts(text);
        else
            fprintf(stderr, "authoring status=%d\n", (int)code);
        free(text);
        return (int)code;
    }
    if (!strcmp(argv[1], "source-inventory") ||
        !strcmp(argv[1], "source-entry")) {
        if (argc != 3)
            return APKG_INVALID;
        code = !strcmp(argv[1], "source-entry")
                   ? ak_source_entry(argv[2], &text, &e)
                   : ak_source_inventory(argv[2], &text, &e);
        if (!code)
            puts(text);
        else
            report(code, NULL, e);
        free(text);
        apkg_error_free(e);
        return (int)code;
    }
    if (!strcmp(argv[1], "folder") || !strcmp(argv[1], "run")) {
        if (argc != 4)
            goto done;
        if (!strcmp(argv[1], "folder")) {
            code = apkg_client_folder(argv[2], argv[3], &text, &e);
            if (!code)
                puts(text);
        } else
            code = apkg_client_run(argv[2], argv[3], &e);
        goto done;
    }
    if (!strcmp(argv[1], "verify")) {
        if (argc != 4) goto done;
        code = apkg_client_verify(argv[2], argv[3], NULL, &text, &e);
        if (!code) puts(text);
        goto done;
    }
    if (!strcmp(argv[1], "requirements")) {
        if (argc != 6) goto done;
        code = apkg_client_requirements(argv[2], argv[3], argv[4], argv[5], &text, &e);
        if (!code) puts(text);
        goto done;
    }
    if (!strcmp(argv[1], "info")) {
        if (argc != 4 && argc != 5) goto done;
        code = apkg_client_info(argv[2], argv[3], argc == 5 ? argv[4] : NULL, &text, &e);
        if (!code) puts(text);
        goto done;
    }
    if (!strcmp(argv[1], "list") || !strcmp(argv[1], "show")) {
        if (argc != 3 && argc != 4)
            goto done;
        code =
            apkg_client_query(argv[2], argc == 4 ? argv[3] : NULL, &text, &e);
        if (!code)
            puts(text);
        goto done;
    }
    if (!strcmp(argv[1], "bundle-check") || !strcmp(argv[1], "run-bundle")) {
        if (argc != 4)
            goto done;
        if (!strcmp(argv[1], "bundle-check")) {
            code = apkg_bundle_check(argv[2], argv[3], &text, &e);
            if (!code)
                puts(text);
        } else {
            code = apkg_bundle_run(argv[2], argv[3], &e);
            if (!code)
                puts("launch_request=accepted");
        }
        goto done;
    }
    if (!strcmp(argv[1], "remove") || !strcmp(argv[1], "repair") ||
        !strcmp(argv[1], "upgrade") || !strcmp(argv[1], "rollback") ||
        !strcmp(argv[1], "resolve") || !strcmp(argv[1], "adopt") ||
        !strcmp(argv[1], "policy")) {
        if (argc < 4)
            goto done;
        const char *pool = NULL;
        for (int i = 4; i < argc; i += 2) {
            if (i + 1 >= argc)
                goto done;
            if (!strcmp(argv[i], "--confirm") && !confirmation)
                confirmation = argv[i + 1];
            else if (!pool &&
                     ((!strcmp(argv[i], "--pool") && !strcmp(argv[1], "resolve")) ||
                      (!strcmp(argv[i], "--evidence") && !strcmp(argv[1], "adopt"))))
                pool = argv[i + 1];
            else
                goto done;
        }
        apkg_client_action action =
            !strcmp(argv[1], "remove")     ? APKG_CLIENT_REMOVE
            : !strcmp(argv[1], "repair")   ? APKG_CLIENT_REPAIR
            : !strcmp(argv[1], "upgrade")  ? APKG_CLIENT_UPGRADE
            : !strcmp(argv[1], "rollback") ? APKG_CLIENT_ROLLBACK
                                           : !strcmp(argv[1], "adopt") ? APKG_CLIENT_ADOPT
                                           : !strcmp(argv[1], "policy") ? APKG_CLIENT_POLICY
                                                                        : APKG_CLIENT_RESOLVE;
        code = apkg_client_apply(action, argv[2], argv[3], pool, confirm,
                                 (void *)confirmation, NULL, &r, &e);
        goto done;
    }
    if (!strcmp(argv[1], "install")) {
        if (argc != 4 && argc != 6)
            goto done;
        if (argc == 6) {
            if (strcmp(argv[4], "--confirm"))
                goto done;
            confirmation = argv[5];
        }
        code = apkg_client_install(argv[2], argv[3], confirm,
                                   (void *)confirmation, NULL, &r, &e);
        goto done;
    }
    o.contract_version = 1;
    o.struct_size = sizeof o;
    o.root.data = argv[2];
    o.root.size = strlen(argv[2]);
    o.mode = !strcmp(argv[1], "inspect") ? APKG_READ : APKG_WRITE;
    code = apkg_open(&o, &c, &e);
    if (code)
        goto done;
    if (!strcmp(argv[1], "recover")) {
        if (argc != 3) {
            code = APKG_INVALID;
            goto done;
        }
        code = apkg_recover(c, NULL, &r, &e);
        goto done;
    }
    code = apkg_snapshot_read(c, &s, &e);
    if (code)
        goto done;
    if (!strcmp(argv[1], "inspect")) {
        const apkg_root_view *v = apkg_snapshot_root(s);
        if (argc != 3) {
            code = APKG_INVALID;
            goto done;
        }
        printf("root_state=%d installed=%llu generation=%llu\n", (int)v->state,
               (unsigned long long)apkg_snapshot_installed_count(s),
               (unsigned long long)v->generation.value);
        goto done;
    }
    if (strcmp(argv[1], "init") || (argc != 5 && argc != 6 && argc != 7)) {
        code = APKG_INVALID;
        goto done;
    }
    if (ak_read(argv[4], &policy, &size, AK_MAX)) {
        code = APKG_IO;
        goto done;
    }
    {
        apkg_text target = {argv[3], strlen(argv[3])};
        apkg_bytes seed = {policy, size};
        code = apkg_plan_initialize(c, s, target, seed, &p, &e);
    }
    if (code)
        goto done;
    code = apkg_plan_format(p, &text);
    if (code)
        goto done;
    if (argc == 6) {
        if (strcmp(argv[5], "--yes")) {
            code = APKG_INVALID;
            goto done;
        }
        initialize_yes = 1;
    }
    if (argc == 7) {
        fprintf(stderr, "Initialization confirmation stays in one process; use "
                        "terminal review or explicit --yes.\n");
        code = APKG_UNSUPPORTED;
        goto done;
    }
    puts(text);
    if (!initialize_yes) {
        if (!isatty(STDIN_FILENO)) {
            fprintf(stderr,
                    "Initialization preview only. Review in a terminal, or use "
                    "--yes for this explicit target and trust policy.\n");
            code = APKG_CANCELLED;
            goto done;
        }
        char expected[65], input[130];
        ak_tohex(apkg_plan_get(p)->plan_digest.bytes, 32, expected);
        fflush(stdout);
        fprintf(stderr, "Type the displayed plan digest to initialize: ");
        if (!fgets(input, sizeof input, stdin)) {
            code = APKG_CANCELLED;
            goto done;
        }
        input[strcspn(input, "\r\n")] = 0;
        if (strcmp(input, expected)) {
            code = APKG_CANCELLED;
            goto done;
        }
    }
    code = apkg_plan_approve(c, p, &apkg_plan_get(p)->plan_digest, &a, &e);
    if (!code)
        code = apkg_execute(c, p, a, NULL, &r, &e);
done:
    report(code, r, e);
    apkg_client_text_free(text);
    free(policy);
    apkg_error_free(e);
    apkg_result_free(r);
    apkg_approval_free(a);
    apkg_plan_free(p);
    apkg_snapshot_free(s);
    apkg_close(c);
    return (int)code;
}

#ifdef __AROS__
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/exec.h>
/* The Shell gives a command a small stack unless someone typed Stack, and an
 * overrun on AROS is a Software Failure, not a refusal. The engine's frames
 * reach tens of KiB, so apkg runs on a stack of its own whenever the one it
 * was given is smaller, as the earlier pkg client does. */
#define APKG_STACK_BYTES (1024ul * 1024ul)
struct entry_args {
    int argc;
    char **argv;
};
static IPTR on_own_stack(struct entry_args *a) {
    return (IPTR)apkg_main(a->argc, a->argv);
}
int main(int argc, char **argv) {
    struct Task *me = FindTask(NULL);
    struct StackSwapStruct sss;
    struct StackSwapArgs ssa;
    struct entry_args a;
    UBYTE *stack;
    int rc;
    if ((IPTR)me->tc_SPUpper - (IPTR)me->tc_SPLower >= APKG_STACK_BYTES)
        return apkg_main(argc, argv);
    stack = AllocVec(APKG_STACK_BYTES, MEMF_ANY);
    if (!stack) {
        fputs("apkg: not enough memory for its stack\n", stderr);
        return APKG_NOMEM;
    }
    a.argc = argc;
    a.argv = argv;
    sss.stk_Lower = stack;
    sss.stk_Upper = stack + APKG_STACK_BYTES;
    sss.stk_Pointer = sss.stk_Upper;
    ssa.Args[0] = (IPTR)&a;
    rc = (int)NewStackSwap(&sss, (LONG_FUNC)on_own_stack, &ssa);
    FreeVec(stack);
    return rc;
}
#else
int main(int argc, char **argv) { return apkg_main(argc, argv); }
#endif
