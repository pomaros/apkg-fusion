/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int append(char *out, size_t cap, size_t *used, const char *format, ...) {
    int n;
    va_list ap;
    va_start(ap, format);
    n = vsnprintf(out + *used, cap - *used, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *used)
        return -1;
    *used += (size_t)n;
    return 0;
}
apkg_status apkg_plan_format(const apkg_plan *p, char **out) {
    size_t cap, used = 0, i;
    char digest[65], root[33];
    ak_json j = {0};
    char target[32];
    int b;
    char *text;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!p)
        return APKG_INVALID;
    if (p->view.effect_count > (AK_MAX - 4096) / (AK_PATH + 512))
        return APKG_LIMIT;
    cap = 4096 + (size_t)p->view.effect_count * (AK_PATH + 512);
    text = malloc(cap);
    if (!text)
        return APKG_NOMEM;
    ak_tohex(p->view.plan_digest.bytes, 32, digest);
    ak_tohex(p->view.root_id.bytes, 16, root);
    {
        int parsed = ak_json_parse(p->root_record, strlen(p->root_record), &j);
        if (parsed) {
            free(text);
            return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        }
    }
    b = ak_member(&j, 0, "body");
    if (ak_string(&j, ak_member(&j, b, "target"), target, sizeof target)) {
        ak_json_free(&j);
        free(text);
        return APKG_INVALID;
    }
    if (append(text, cap, &used,
               "%s\nRoot folder: %s\nRoot identity: %s\nTarget: %s\nApproved plan: %s\n\n",
               p->absent ? "Initialize package root" : "Install signed package", p->root, root,
               target, digest))
        goto limit;
    if (!p->absent) {
        int arr = ak_member(&j, b, "installed");
        size_t last = 0;
        char authority[33], key[65];
        for (i = (size_t)arr + 1; i < j.tokens[arr].next; i = j.tokens[i].next)
            last = i;
        {
            int d = ak_member(&j, (int)last, "descriptor"), body = ak_member(&j, d, "body"),
                sig = ak_member(&j, (int)last, "signature");
            if (ak_string(&j, ak_member(&j, body, "authority"), authority, sizeof authority) ||
                ak_string(&j, ak_member(&j, sig, "public_key"), key, sizeof key))
                goto limit;
        }
        if (append(text, cap, &used,
                   "Package: %s\nPublisher: %s\nSignature: verified, authorized key %s\nReview: no "
                   "independent review asserted here\nRuntime test: no runtime qualification "
                   "asserted here\n\n",
                   p->name, authority, key))
            goto limit;
    } else {
        int policy = ak_member(&j, b, "policy"),
            authorities = ak_member(&j, ak_member(&j, policy, "body"), "authorities");
        for (i = (size_t)authorities + 1; i < j.tokens[authorities].next; i = j.tokens[i].next) {
            char authority[33];
            int keys = ak_member(&j, (int)i, "keys");
            size_t k;
            if (ak_string(&j, ak_member(&j, (int)i, "id"), authority, sizeof authority))
                goto limit;
            for (k = (size_t)keys + 1; k < j.tokens[keys].next; k = j.tokens[k].next) {
                char key[65];
                if (ak_string(&j, ak_member(&j, (int)k, "public_key"), key, sizeof key) ||
                    append(text, cap, &used, "Authorize publisher %s with key %s\n", authority,
                           key))
                    goto limit;
            }
        }
    }
    if (append(text, cap, &used, "All planned effects:\n"))
        goto limit;
    for (i = 0; i < p->view.effect_count; i++) {
        const apkg_effect_view *effect = &p->effects[i];
        if (append(text, cap, &used, "%s %.*s\n",
                   effect->kind == APKG_EFFECT_TRUST      ? "Trust grants"
                   : effect->kind == APKG_EFFECT_DATABASE ? "Commit database"
                                                          : "Create",
                   (int)effect->destination.size, effect->destination.data))
            goto limit;
        if (i >= 2) {
            ak_json f = {0};
            char hash[65], size[21], mode[21];
            {
                int parsed = ak_json_parse(effect->record.data, effect->record.size, &f);
                if (parsed) {
                    ak_json_free(&j);
                    free(text);
                    return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
                }
            }
            if (ak_equal(&f, ak_member(&f, 0, "type"), "file")) {
                if (ak_string(&f, ak_member(&f, 0, "sha256"), hash, sizeof hash) ||
                    ak_string(&f, ak_member(&f, 0, "size"), size, sizeof size) ||
                    ak_string(&f, ak_member(&f, 0, "mode"), mode, sizeof mode) ||
                    append(text, cap, &used, "  %s bytes, mode %s (decimal), SHA-256 %s\n", size,
                           mode, hash)) {
                    ak_json_free(&f);
                    goto limit;
                }
            }
            ak_json_free(&f);
        }
    }
    ak_json_free(&j);
    *out = text;
    return APKG_OK;
limit:
    ak_json_free(&j);
    free(text);
    return APKG_LIMIT;
}
void apkg_client_text_free(char *s) { free(s); }
apkg_status apkg_client_install(const char *root, const char *dir, apkg_confirm_fn confirm,
                                void *user, const apkg_observer *observer, apkg_result **out,
                                apkg_error **e) {
    apkg_open_options options = {0};
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    apkg_package *package = NULL;
    apkg_plan *plan = NULL;
    apkg_approval *approval = NULL;
    apkg_status code;
    unsigned char *descriptor = NULL, *signature = NULL;
    size_t dn = 0, sn = 0;
    char path[AK_PATH], *presentation = NULL;
    apkg_bytes d, sg;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!root || !dir || !confirm)
        return APKG_INVALID;
    options.contract_version = 1;
    options.struct_size = sizeof options;
    options.mode = APKG_WRITE;
    options.root.data = root;
    options.root.size = strlen(root);
    code = apkg_open(&options, &c, e);
    if (code)
        goto done;
    if (ak_path(path, dir, "descriptor.json") || ak_read(path, &descriptor, &dn, AK_MAX) ||
        ak_path(path, dir, "signature.json") || ak_read(path, &signature, &sn, AK_MAX)) {
        code = ak_fail(e,
                       errno == ENOMEM  ? APKG_NOMEM
                       : errno == EFBIG ? APKG_LIMIT
                                        : APKG_IO,
                       "cannot read local signed package", dir);
        goto done;
    }
    d.data = descriptor;
    d.size = dn;
    sg.data = signature;
    sg.size = sn;
    code = apkg_snapshot_read(c, &s, e);
    if (code)
        goto done;
    code = apkg_package_read(c, d, sg, &package, e);
    if (code)
        goto done;
    code = apkg_plan_install(c, s, package, observer, &plan, e);
    if (code)
        goto done;
    code = apkg_plan_format(plan, &presentation);
    if (code)
        goto done;
    if (!confirm(user, plan, presentation)) {
        code = APKG_CANCELLED;
        goto done;
    }
    code = apkg_plan_approve(c, plan, &plan->view.plan_digest, &approval, e);
    if (code)
        goto done;
    code = apkg_execute(c, plan, approval, observer, out, e);
done:
    free(descriptor);
    free(signature);
    apkg_client_text_free(presentation);
    apkg_approval_free(approval);
    apkg_plan_free(plan);
    apkg_package_free(package);
    apkg_snapshot_free(s);
    apkg_close(c);
    return code;
}
