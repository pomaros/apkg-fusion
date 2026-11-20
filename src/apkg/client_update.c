/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Client requests that carry the takeover planning input, and the unattended
 * update of one named package from one channel. */
#include "apkg_client.h"
#include "internal.h"
#include <string.h>

apkg_status ak_client_apply(int, const char *, const char *, const char *, int,
                            ak_confirm_fn, void *, const apkg_observer *,
                            apkg_result **, apkg_error **);

apkg_status apkg_client_request_apply(const apkg_client_request *request,
                                      apkg_result **out, apkg_error **e) {
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!request || request->contract_version != APKG_CONTRACT_VERSION ||
        request->struct_size != sizeof *request)
        return ak_fail(e, APKG_UNSUPPORTED, "API version/size mismatch", "");
    if (!request->root || !request->argument || !request->confirm ||
        (request->system_takeover != 0 && request->system_takeover != 1) ||
        (request->recommendations != 0 && request->recommendations != 1))
        return APKG_INVALID;
    switch (request->action) {
    case APKG_CLIENT_INSTALL:
    case APKG_CLIENT_UPGRADE:
    case APKG_CLIENT_RESOLVE:
    case APKG_CLIENT_REPAIR:
        if (request->recommendations)
            return APKG_INVALID;
        return ak_client_apply(request->action, request->root,
                               request->argument, request->pool,
                               request->system_takeover, request->confirm,
                               request->user, request->observer, out, e);
    case APKG_CLIENT_CHANNEL:
        return ak_client_channel_apply(
            request->root, request->argument, request->pool,
            request->recommendations, request->system_takeover,
            request->confirm, request->user, request->observer, out, e);
    case APKG_CLIENT_SOURCE_INSTALL:
        return ak_client_source_install(
            request->root, request->argument, request->recommendations,
            request->system_takeover, request->confirm, request->user,
            request->observer, out, e);
    default:
        return ak_fail(e, APKG_UNSUPPORTED,
                       "this action takes no request options", "");
    }
}
static apkg_status run(apkg_ctx *c, apkg_plan *p, const apkg_client_update *u,
                       apkg_result **out, apkg_error **e) {
    apkg_approval *approval = NULL;
    char *text = NULL;
    apkg_status code = apkg_plan_format(p, &text);
    if (!code && u->confirm &&
        (!u->confirm(u->user, p, text) || ak_cancelled(u->observer)))
        code = APKG_CANCELLED;
    if (!code)
        code = apkg_plan_approve(c, p, &apkg_plan_get(p)->plan_digest,
                                 &approval, e);
    if (!code)
        code = apkg_execute(c, p, approval, u->observer, out, e);
    apkg_approval_free(approval);
    apkg_client_text_free(text);
    return code;
}
apkg_status apkg_client_channel_update(const apkg_client_update *u,
                                       apkg_result **out, apkg_error **e) {
    apkg_open_options options = {0};
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    apkg_channel *channel = NULL;
    apkg_plan *p = NULL;
    apkg_id id;
    apkg_status code;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!u || u->contract_version != APKG_CONTRACT_VERSION ||
        u->struct_size != sizeof *u)
        return ak_fail(e, APKG_UNSUPPORTED, "API version/size mismatch", "");
    if (!u->root || !u->endpoint || !u->identity || !u->name ||
        !ak_valid_name(u->name) || strlen(u->identity) != 32 ||
        ak_hex(u->identity, id.bytes, 16) || !u->policy.data ||
        !u->policy.size || (u->takeover != 0 && u->takeover != 1) ||
        (u->recommendations != 0 && u->recommendations != 1))
        return APKG_INVALID;
    options.contract_version = APKG_CONTRACT_VERSION;
    options.struct_size = sizeof options;
    options.mode = APKG_WRITE;
    options.root = (apkg_text){u->root, strlen(u->root)};
    options.max_object_bytes = u->max_object_bytes;
    options.max_memory_bytes = u->max_memory_bytes;
    code = apkg_open(&options, &c, e);
    if (!code)
        code = apkg_plan_system_takeover(c, u->takeover);
    if (!code)
        code = apkg_snapshot_read(c, &s, e);
    if (!code && s->view.state == APKG_ROOT_PENDING) {
        /* An interrupted transaction is finished first; nothing new is
         * planned over a root that has not settled. */
        apkg_result *recovered = NULL;
        code = apkg_recover(c, u->observer, &recovered, e);
        if (code) {
            *out = recovered;
            goto done;
        }
        apkg_result_free(recovered);
        apkg_snapshot_free(s);
        s = NULL;
        code = apkg_snapshot_read(c, &s, e);
    }
    if (!code && s->view.state == APKG_ROOT_ABSENT) {
        apkg_result *initialized = NULL;
        if (!u->target) {
            code = ak_fail(e, APKG_NOT_FOUND,
                           "the root is not initialized and no target was "
                           "given",
                           u->root);
            goto done;
        }
        code = apkg_plan_initialize(
            c, s, (apkg_text){u->target, strlen(u->target)}, u->policy, &p, e);
        if (!code)
            code = run(c, p, u, &initialized, e);
        apkg_plan_free(p);
        p = NULL;
        if (code) {
            *out = initialized;
            goto done;
        }
        apkg_result_free(initialized);
        apkg_snapshot_free(s);
        s = NULL;
        code = apkg_snapshot_read(c, &s, e);
    }
    if (!code && s->view.state != APKG_ROOT_READY)
        code = s->view.state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED
                                                  : APKG_POLICY;
    if (!code && (strlen(s->policy) != u->policy.size ||
                  memcmp(s->policy, u->policy.data, u->policy.size)))
        code = ak_fail(e, APKG_POLICY,
                       "the root's trust policy is not the pinned policy",
                       u->root);
    if (!code) {
        apkg_text endpoint = {u->endpoint, strlen(u->endpoint)};
        code = ak_http_url(u->endpoint)
                   ? apkg_channel_open_http(c, endpoint, &id, 0, u->observer,
                                            &channel, e)
                   : apkg_channel_open_file(c, endpoint, &id, 0, &channel, e);
    }
    if (!code)
        code = ak_channels_install_named(c, s, &channel, 1, u->name,
                                         u->recommendations, u->confirm,
                                         u->user, u->observer, out, e);
done:
    apkg_channel_free(channel);
    apkg_snapshot_free(s);
    apkg_close(c);
    if (code && e && !*e)
        ak_fail(e, code, "channel update refused", u->name);
    return code;
}
