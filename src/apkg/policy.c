/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
/* Trust administration: an explicit, journaled replacement of a root's trust
 * policy, for key rotation, reset and revocation.
 *
 * Trust changes only through a plan the operator approves after seeing every
 * grant added and removed, role by role, and every installed package whose
 * signing key loses its grant. The installed set, channel floors and retained
 * observations are unchanged; the plan rewrites root state only. A policy
 * change is never undone by a rollback: reverting it is another policy plan,
 * and rollback of earlier package history refuses once the policy differs. */
#include "internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *const roles[] = {"authorities", "channels", "reviewers",
                                    "testers"};
static const char *const labels[] = {"package authority", "channel",
                                     "reviewer", "runtime tester"};

/* Grants of `role` in `from` that `in` lacks, one line each. */
static int missing(const ak_json *from, const ak_json *in, size_t role,
                   const char *sign, ak_buf *out) {
    int arr = ak_member(from, ak_member(from, 0, "body"), roles[role]);
    if (arr < 0)
        return 0;
    for (size_t i = (size_t)arr + 1; i < from->tokens[arr].next;
         i = from->tokens[i].next) {
        char id[65];
        int keys = ak_member(from, (int)i, "keys");
        if (ak_string(from, ak_member(from, (int)i, "id"), id, sizeof id) ||
            keys < 0)
            return -1;
        for (size_t k = (size_t)keys + 1; k < from->tokens[keys].next;
             k = from->tokens[k].next) {
            char key[65];
            if (ak_string(from, ak_member(from, (int)k, "public_key"), key,
                          sizeof key))
                return -1;
            if (!ak_attestation_policy(in, roles[role], id, key))
                ak_buf_add(out, "%s %s %s key %s\n", sign, labels[role], id,
                           key);
        }
    }
    return 0;
}
static int policy_of(const char *state, ak_json *root, ak_json *policy) {
    int b, p;
    if (ak_json_parse(state, strlen(state), root))
        return -1;
    b = ak_member(root, 0, "body");
    p = ak_member(root, b, "policy");
    if (p < 0 || ak_json_parse(root->text + root->tokens[p].start,
                               root->tokens[p].end - root->tokens[p].start,
                               policy))
        return -1;
    return 0;
}
apkg_status ak_policy_delta(const char *before_state, const char *after_state,
                            ak_buf *out) {
    ak_json before = {0}, after = {0}, old = {0}, new = {0};
    apkg_status code = APKG_INVALID;
    if (policy_of(before_state, &before, &old) ||
        policy_of(after_state, &after, &new))
        goto done;
    for (size_t role = 0; role < 4; role++)
        if (missing(&new, &old, role, "Grant", out) ||
            missing(&old, &new, role, "Revoke", out))
            goto done;
    /* Installed packages whose signer the new policy no longer grants. */
    int rows = ak_member(&after, ak_member(&after, 0, "body"), "installed");
    if (rows < 0)
        goto done;
    for (size_t i = (size_t)rows + 1; i < after.tokens[rows].next;
         i = after.tokens[i].next) {
        int d = ak_member(&after, (int)i, "descriptor"),
            g = ak_member(&after, (int)i, "signature");
        int body = ak_member(&after, d, "body");
        char authority[33], name[256], key[65];
        if (ak_string(&after, ak_member(&after, body, "authority"), authority,
                      sizeof authority) ||
            ak_string(&after, ak_member(&after, body, "name"), name,
                      sizeof name) ||
            ak_string(&after, ak_member(&after, g, "public_key"), key,
                      sizeof key))
            continue; /* adopted records carry no original signature */
        if (ak_policy(&old, 0, authority, key) &&
            !ak_policy(&new, 0, authority, key))
            ak_buf_add(out,
                       "Installed package %s loses author trust (authority %s "
                       "key %s); it stays installed and verifiable offline "
                       "only as untrusted\n",
                       name, authority, key);
    }
    code = out->bad ? (apkg_status)out->bad : APKG_OK;
done:
    ak_json_free(&before);
    ak_json_free(&after);
    ak_json_free(&old);
    ak_json_free(&new);
    return code;
}
apkg_status apkg_plan_policy(apkg_ctx *c, const apkg_snapshot *s,
                             apkg_bytes policy, apkg_plan **out,
                             apkg_error **e) {
    ak_json j = {0}, root = {0};
    char *text = NULL, *entries = NULL;
    apkg_status code;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c || !s || !policy.data || !policy.size)
        return APKG_INVALID;
    if (c->mode != APKG_WRITE)
        return APKG_POLICY;
    if (s->view.state != APKG_ROOT_READY)
        return s->view.state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED
                                                  : APKG_POLICY;
    if (policy.size > c->object_max)
        return APKG_LIMIT;
    {
        int parsed = ak_json_parse(policy.data, policy.size, &j);
        if (parsed)
            return parsed == -2 ? APKG_NOMEM
                                : ak_fail(e, APKG_POLICY,
                                          "trust policy is not canonical "
                                          "apkg-json-1",
                                          "");
    }
    if (!ak_policy_valid(&j)) {
        ak_json_free(&j);
        return ak_fail(e, APKG_POLICY, "invalid trust policy", "");
    }
    ak_json_free(&j);
    text = ak_copy(policy.data, policy.size);
    if (!text)
        return APKG_NOMEM;
    if (!strcmp(text, s->policy)) {
        free(text);
        return ak_fail(e, APKG_INVALID, "the trust policy is unchanged", "");
    }
    {
        int parsed = ak_json_parse(s->raw, s->view.source_record.size, &root);
        if (parsed) {
            free(text);
            return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        }
    }
    entries =
        ak_slice(&root, ak_member(&root, ak_member(&root, 0, "body"), "installed"));
    ak_json_free(&root);
    if (!entries) {
        free(text);
        return APKG_NOMEM;
    }
    code = ak_plan_records_heads(c, s, entries, APKG_PLAN_POLICY, NULL,
                                 s->channel_heads, s->qualification_records,
                                 text, NULL, out, e);
    free(entries);
    free(text);
    if (!code) {
        apkg_plan *p = *out;
        p->effects[0].kind = APKG_EFFECT_TRUST;
        p->effects[0].destination.data = ".apkg/state#policy";
        p->effects[0].destination.size = strlen(p->effects[0].destination.data);
        p->effects[0].record.data = (unsigned char *)p->root_record;
        p->effects[0].record.size = strlen(p->root_record);
    }
    if (code && e && !*e)
        ak_fail(e, code, "trust policy change refused", "");
    return code;
}
