/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
/* Released APKG command spelling over common contexts only. No old writer. */
#include "apkg_client.h"
#include "internal.h"
#include "pkg_target.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int yes, dry, json, asked;
    const char *digest;
} legacy_approval;
static void string(ak_buf *b, apkg_text text) {
    ak_buf_add(b, "\"");
    for (size_t i = 0; i < text.size; i++) {
        unsigned char c = (unsigned char)text.data[i];
        if (c == '"' || c == '\\')
            ak_buf_add(b, "\\%c", c);
        else if (c < 32)
            ak_buf_add(b, "\\u%04x", c);
        else
            ak_buf_add(b, "%c", c);
    }
    ak_buf_add(b, "\"");
}
static int approval(void *user, const apkg_plan *plan,
                    const char *presentation) {
    legacy_approval *choice = user;
    choice->asked = 1;
    if (choice->dry && choice->json) {
        apkg_bytes raw = apkg_plan_get(plan)->canonical;
        fwrite(raw.data, 1, raw.size, stdout);
        putchar('\n');
    } else
        fputs(presentation, stdout);
    if (choice->dry)
        return 0;
    if (choice->yes)
        return 1;
    char digest[65];
    ak_tohex(apkg_plan_get(plan)->plan_digest.bytes, 32, digest);
    if (choice->digest && !strcmp(choice->digest, digest))
        return 1;
    fprintf(stderr,
            "Confirmation required: repeat with --confirm %s or --yes\n",
            digest);
    return 0;
}
static int compare_names(const void *a, const void *b) {
    const apkg_installed_view *x = *(const apkg_installed_view *const *)a;
    const apkg_installed_view *y = *(const apkg_installed_view *const *)b;
    return strcmp(x->name.value.data, y->name.value.data);
}
static apkg_status rows(const apkg_snapshot *snapshot, const char *name,
                        int json, char **out, apkg_error **error) {
    *out = NULL;
    size_t count = (size_t)apkg_snapshot_installed_count(snapshot), found = 0;
    const apkg_installed_view **sorted =
        calloc(count ? count : 1, sizeof *sorted);
    if (!sorted)
        return APKG_NOMEM;
    apkg_status code = APKG_OK;
    for (size_t i = 0; !code && i < count; i++)
        code = apkg_snapshot_installed_at(snapshot, i, &sorted[i], error);
    if (!code && count > 1)
        qsort(sorted, count, sizeof *sorted, compare_names);
    ak_buf b = {0};
    b.max = 16u * 1024u * 1024u;
    if (json && !name)
        ak_buf_add(&b, "{\"rows\":[");
    for (size_t k = 0; !code && k < count; k++) {
        const apkg_installed_view *row = sorted[k];
        if (name && strcmp(row->name.value.data, name))
            continue;
        if (k &&
            !strcmp(sorted[k - 1]->name.value.data, row->name.value.data)) {
            code = APKG_CONFLICT;
            break;
        }
        if (!json) {
            ak_buf_add(&b, "%s\n", row->name.value.data);
            found++;
            continue;
        }
        apkg_inspection *inspection = NULL;
        /* Snapshot order is independent of the alphabetic output order. */
        size_t index = 0;
        while (index < count) {
            const apkg_installed_view *candidate = NULL;
            code =
                apkg_snapshot_installed_at(snapshot, index, &candidate, error);
            if (code || candidate == row)
                break;
            index++;
        }
        if (!code)
            code = apkg_snapshot_inspect(snapshot, index, &inspection, error);
        if (code) {
            apkg_inspection_free(inspection);
            break;
        }
        const apkg_inspection_view *v = apkg_inspection_get(inspection);
        struct pkg_target target;
        char why[160];
        if (pkg_target_parse(v->target.data, &target, why, sizeof why))
            code = APKG_TARGET;
        char *summary = NULL, *category = NULL;
        if (!code) {
            apkg_status metadata = apkg_presentation_field(v->presentation, "summary", &summary, NULL);
            if (metadata && metadata != APKG_NOT_FOUND) code = metadata;
        }
        if (!code) {
            apkg_status metadata = apkg_presentation_field(v->presentation, "category", &category, NULL);
            if (metadata && metadata != APKG_NOT_FOUND) code = metadata;
        }
        if (!code) {
            ak_buf_add(&b, "%s{\"id\":", found ? "," : "");
            string(&b, v->name);
            if (name) {
                ak_buf_add(&b, ",\"schema\":1,\"name\":");
                string(&b, v->name);
                ak_buf_add(&b, ",\"dir\":");
                string(&b, v->name);
            }
            ak_buf_add(&b, ",\"version\":");
            string(&b, v->version);
            ak_buf_add(&b,
                       ",\"revision\":%" PRIu64
                       ",\"arch\":\"%s\",\"abi\":\"%s\",\"kind\":",
                       v->revision, target.cpu, target.abi);
            string(&b, v->kind);
            ak_buf_add(&b, ",\"summary\":");
            if (summary) string(&b, (apkg_text){summary, strlen(summary)});
            else ak_buf_add(&b, "null");
            ak_buf_add(&b, ",\"category\":");
            if (category) string(&b, (apkg_text){category, strlen(category)});
            else ak_buf_add(&b, "null");
            ak_buf_add(&b,
                       ",\"size\":-1,"
                       "\"ours\":%s,\"installed\":true,\"authority\":",
                       v->target_matches ? "true" : "false");
            string(&b, v->authority);
            ak_buf_add(&b,
                       ",\"signature\":\"%s\",\"signature_scope\":"
                       "\"descriptor\",\"authortrust\":\"%s\","
                       "\"unrecorded\":[%s%s\"size\"]}",
                       v->signature == APKG_SIGNATURE_VALID ? "verified"
                                                            : "invalid",
                       v->author_trust == APKG_TRUST_ACCEPTED ? "accepted"
                                                              : "rejected",
                       summary ? "" : "\"summary\",",
                       category ? "" : "\"category\",");
            found++;
        }
        apkg_client_text_free(summary);
        apkg_client_text_free(category);
        apkg_inspection_free(inspection);
        if (b.bad)
            code = (apkg_status)b.bad;
    }
    free(sorted);
    if (!code && name && !found)
        code = APKG_NOT_FOUND;
    if (!code && json && !name)
        ak_buf_add(
            &b, "],\"hidden\":0,\"compatibility\":\"apkg-legacy-compat-1\"}\n");
    if (!code && !json && !found)
        ak_buf_add(&b, "nothing installed\n");
    if (!code && b.bad)
        code = (apkg_status)b.bad;
    if (!code) {
        *out = b.data;
        b.data = NULL;
    }
    free(b.data);
    return code;
}
int ak_legacy_main(int argc, char **argv) {
    const char *root = "SYS:Packages", *command = NULL, *name = NULL;
    legacy_approval choice = {0};
    int retry = 0, fetch = 0, bad = 0;
    for (int i = 1; i < argc; i++) {
        const char *word = argv[i];
        if (!strcmp(word, "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(word, "--confirm") && i + 1 < argc)
            choice.digest = argv[++i];
        else if (!strcmp(word, "--yes"))
            choice.yes = 1;
        else if (!strcmp(word, "--dry-run"))
            choice.dry = 1;
        else if (!strcmp(word, "--json"))
            choice.json = 1;
        else if (!strcmp(word, "--fetch"))
            fetch = 1;
        else if (!strcmp(word, "--retry"))
            retry = 1;
        else if (!strcmp(word, "--plain") || !strcmp(word, "--color=never")) {
        } else if (!strcmp(word, "--help") || !strcmp(word, "-h")) {
            puts("Usage: apkg legacy [--root ROOT] list [--json] | info NAME | "
                 "verify NAME | install NAME | upgrade NAME | remove NAME | "
                 "doctor [--retry]\nMutations: --dry-run [--json] "
                 "(install/upgrade also require --fetch), --yes or "
                 "--confirm DIGEST. Requires an initialized or imported common "
                 "root with signed sources configured. Other legacy options "
                 "refuse.");
            return 0;
        } else if (!strcmp(word, "--version")) {
            puts("apkg-fusion legacy compatibility 1");
            return 0;
        } else if (word[0] == '-')
            bad = 1;
        else if (!command)
            command = word;
        else if (!name)
            name = word;
        else
            bad = 1;
    }
    int mutation =
        command && (!strcmp(command, "install") ||
                    !strcmp(command, "upgrade") || !strcmp(command, "remove"));
    int list = command && !strcmp(command, "list"),
        info = command && !strcmp(command, "info");
    int verify = command && !strcmp(command, "verify"),
        doctor = command && !strcmp(command, "doctor");
    if (bad || !command ||
        (!mutation && !list && !info && !verify && !doctor) ||
        ((list || doctor) ? name != NULL : !name) ||
        (name && !ak_valid_name(name)) ||
        (fetch && !(choice.dry && mutation && strcmp(command, "remove"))) ||
        (choice.dry && mutation && strcmp(command, "remove") && !fetch) ||
        (retry && !doctor) || (choice.dry && !mutation) ||
        ((choice.yes || choice.digest) && !mutation) ||
        (choice.json && !(list || info || (mutation && choice.dry))) ||
        (choice.dry && (choice.yes || choice.digest)) ||
        (choice.yes && choice.digest)) {
        fputs("apkg legacy: unsupported command/arguments; use --help. Nothing "
              "started.\n",
              stderr);
        return 5;
    }
    apkg_open_options options = {0};
    options.contract_version = APKG_CONTRACT_VERSION;
    options.struct_size = sizeof options;
    options.root = (apkg_text){root, strlen(root)};
    apkg_ctx *ctx = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_error *error = NULL;
    apkg_result *result = NULL;
    char *text = NULL;
    /* Never open an unmigrated legacy installation for mutation. */
    apkg_status code = apkg_open(&options, &ctx, &error);
    if (!code)
        code = apkg_snapshot_read(ctx, &snapshot, &error);
    const apkg_root_view *view = snapshot ? apkg_snapshot_root(snapshot) : NULL;
    if (!code && view->state != APKG_ROOT_READY &&
        !(doctor && view->state == APKG_ROOT_PENDING))
        code = APKG_POLICY;
    if (!code && (list || info))
        code = rows(snapshot, name, choice.json || info, &text, &error);
    if (!code && mutation &&
        (!strcmp(command, "upgrade") || !strcmp(command, "remove"))) {
        int found = 0;
        for (uint64_t i = 0; i < apkg_snapshot_installed_count(snapshot); i++) {
            const apkg_installed_view *row = NULL;
            code = apkg_snapshot_installed_at(snapshot, i, &row, &error);
            if (code)
                break;
            if (!strcmp(row->name.value.data, name))
                found++;
        }
        if (!code && found != 1)
            code = found ? APKG_CONFLICT : APKG_NOT_FOUND;
    }
    apkg_snapshot_free(snapshot);
    apkg_close(ctx);
    if (!code && verify)
        code = apkg_client_verify(root, name, NULL, &text, &error);
    if (!code && mutation) {
        if (!strcmp(command, "remove"))
            code = apkg_client_apply(APKG_CLIENT_REMOVE, root, name, NULL,
                                     approval, &choice, NULL, &result, &error);
        else
            code = apkg_client_source_install(root, name, 0, approval, &choice,
                                              NULL, &result, &error);
    }
    if (!code && doctor && retry)
        code = apkg_client_apply(APKG_CLIENT_RECOVER, root, "", NULL, approval,
                                 &choice, NULL, &result, &error);
    if (!code && choice.dry && !choice.asked && result &&
        apkg_result_get(result)->outcome == APKG_UNCHANGED) {
        if (choice.json)
            puts("{\"compatibility\":\"apkg-legacy-compat-1\",\"outcome\":"
                 "\"unchanged\"}");
        else
            puts("already current; nothing to change");
    }
    if (!code && text) {
        fputs(text, stdout);
        if (*text && text[strlen(text) - 1] != '\n')
            putchar('\n');
    }
    if (!code && doctor)
        puts(
            retry
                ? "common recovery complete"
                : "common root inspected; use --retry to recover pending work");
    if (code && !(code == APKG_CANCELLED && choice.asked)) {
        if (error) {
            const apkg_error_view *e = apkg_error_get(error);
            fprintf(stderr, "%.*s: %.*s\n", (int)e->message.size,
                    e->message.data ? e->message.data : "",
                    (int)e->subject.size,
                    e->subject.data ? e->subject.data : "");
        } else
            fprintf(stderr, "apkg legacy: common operation refused (code %d)\n",
                    code);
    }
    apkg_client_text_free(text);
    apkg_error_free(error);
    apkg_result_free(result);
    if (code == APKG_CANCELLED && choice.asked)
        return choice.dry ? 0 : 5;
    return code ? 10 : 0;
}
