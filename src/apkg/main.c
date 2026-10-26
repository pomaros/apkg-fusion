/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int confirm(void *user, const apkg_plan *plan, const char *text) {
    const char *expected = user;
    char digest[65];
    ak_tohex(apkg_plan_get(plan)->plan_digest.bytes, 32, digest);
    puts(text);
    if (expected && !strcmp(expected, digest))
        return 1;
    fprintf(stderr, "Confirmation required: repeat with --confirm %s\n", digest);
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
int main(int argc, char **argv) {
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
    if (argc < 3) {
        fprintf(stderr, "Usage: apkg-core inspect ROOT | recover ROOT | init ROOT TARGET POLICY "
                        "[--confirm DIGEST] | install ROOT PACKAGE-DIRECTORY [--confirm DIGEST]\n");
        return APKG_INVALID;
    }
    if (!strcmp(argv[1], "install")) {
        if (argc != 4 && argc != 6)
            goto done;
        if (argc == 6) {
            if (strcmp(argv[4], "--confirm"))
                goto done;
            confirmation = argv[5];
        }
        code = apkg_client_install(argv[2], argv[3], confirm, (void *)confirmation, NULL, &r, &e);
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
        if (strcmp(argv[5], "--confirm")) {
            code = APKG_INVALID;
            goto done;
        }
        confirmation = argv[6];
    }
    if (initialize_yes)
        puts(text);
    if (!initialize_yes && !confirm((void *)confirmation, p, text)) {
        code = APKG_CANCELLED;
        goto done;
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
