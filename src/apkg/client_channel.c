/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include <string.h>
static apkg_status open_source(const char *root, const char *endpoint,
                               const char *identity, uint64_t floor,
                               apkg_mode mode, const apkg_observer *observer,
                               apkg_ctx **ctx, apkg_channel **channel,
                               apkg_error **error) {
    apkg_id id;
    if (!root || !endpoint || !identity || ak_hex(identity, id.bytes, 16))
        return APKG_INVALID;
    apkg_open_options options = {0};
    options.contract_version = 1;
    options.struct_size = sizeof options;
    options.mode = mode;
    options.root.data = root;
    options.root.size = strlen(root);
    apkg_status code = apkg_open(&options, ctx, error);
    if (code)
        return code;
    apkg_text source = {endpoint, strlen(endpoint)};
    return ak_http_url(endpoint)
               ? apkg_channel_open_http(*ctx, source, &id, floor, observer,
                                        channel, error)
               : apkg_channel_open_file(*ctx, source, &id, floor, channel,
                                        error);
}
apkg_status apkg_client_channel_query(const char *root, const char *endpoint,
                                      const char *identity, uint64_t floor,
                                      const apkg_observer *observer, char **out,
                                      apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    apkg_ctx *ctx = NULL;
    apkg_channel *channel = NULL;
    apkg_status code =
        ak_cancelled(observer)
            ? APKG_CANCELLED
            : open_source(root, endpoint, identity, floor, APKG_READ, observer,
                          &ctx, &channel, error);
    if (!code)
        code = ak_channel_format(channel, out);
    apkg_channel_free(channel);
    apkg_close(ctx);
    return code;
}
apkg_status apkg_client_channel_apply(const char *root, const char *endpoint,
                                      const char *selection,
                                      int recommendations,
                                      apkg_confirm_fn confirm, void *user,
                                      const apkg_observer *observer,
                                      apkg_result **out, apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!selection || strlen(selection) != 162 || selection[97] != '/' ||
        selection[32] != '/' || !confirm ||
        (recommendations != 0 && recommendations != 1))
        return APKG_INVALID;
    char identity[33];
    memcpy(identity, selection, 32);
    identity[32] = 0;
    char selected_snapshot[65];
    memcpy(selected_snapshot, selection + 33, 64);
    selected_snapshot[64] = 0;
    apkg_digest wanted, observed;
    if (ak_hex(selected_snapshot, observed.bytes, 32) ||
        ak_hex(selection + 98, wanted.bytes, 32))
        return APKG_INVALID;
    apkg_ctx *ctx = NULL;
    apkg_channel *channel = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_plan *plan = NULL;
    apkg_approval *approval = NULL;
    char *text = NULL;
    apkg_status code =
        ak_cancelled(observer)
            ? APKG_CANCELLED
            : open_source(root, endpoint, identity, 0, APKG_WRITE, observer,
                          &ctx, &channel, error);
    if (!code && memcmp(observed.bytes,
                        apkg_channel_get(channel)->snapshot_digest.bytes, 32))
        code = APKG_STALE;
    if (!code)
        code = apkg_snapshot_read(ctx, &snapshot, error);
    if (!code)
        code =
            apkg_plan_channel_install(ctx, snapshot, channel, &wanted,
                                      recommendations, observer, &plan, error);
    if (!code)
        code = apkg_plan_format(plan, &text);
    if (!code && (!confirm(user, plan, text) || ak_cancelled(observer)))
        code = APKG_CANCELLED;
    if (!code)
        code = apkg_plan_approve(ctx, plan, &apkg_plan_get(plan)->plan_digest,
                                 &approval, error);
    if (!code)
        code = apkg_execute(ctx, plan, approval, observer, out, error);
    apkg_client_text_free(text);
    apkg_approval_free(approval);
    apkg_plan_free(plan);
    apkg_snapshot_free(snapshot);
    apkg_channel_free(channel);
    apkg_close(ctx);
    return code;
}
