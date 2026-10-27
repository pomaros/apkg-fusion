/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include <inttypes.h>
#include <stdlib.h>
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
apkg_status apkg_client_channel_open(const char *root, const char *endpoint,
                                     const char *identity, uint64_t floor,
                                     const apkg_observer *observer,
                                     apkg_channel **out, apkg_error **error) {
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
        *out = channel;
    else
        apkg_channel_free(channel);
    apkg_close(ctx);
    return code;
}
apkg_status apkg_client_channel_selection(const apkg_channel *channel,
                                          uint64_t index, char **out,
                                          apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!channel)
        return APKG_INVALID;
    const apkg_channel_entry_view *entry = NULL;
    apkg_status code = apkg_channel_entry_at(channel, index, &entry, error);
    if (code)
        return code;
    const apkg_channel_view *view = apkg_channel_get(channel);
    char *selection = malloc(163);
    if (!selection)
        return APKG_NOMEM;
    ak_tohex(view->id.bytes, 16, selection);
    selection[32] = '/';
    ak_tohex(view->snapshot_digest.bytes, 32, selection + 33);
    selection[97] = '/';
    ak_tohex(entry->descriptor_digest.bytes, 32, selection + 98);
    *out = selection;
    return APKG_OK;
}
apkg_status apkg_client_channel_refresh(const char *root, const char *endpoint,
                                        const char *identity, uint64_t floor,
                                        const apkg_observer *observer,
                                        apkg_snapshot **snapshot_out,
                                        apkg_channel **channel_out,
                                        apkg_error **error) {
    if (error)
        *error = NULL;
    if (snapshot_out)
        *snapshot_out = NULL;
    if (channel_out)
        *channel_out = NULL;
    if (!snapshot_out || !channel_out ||
        (void *)snapshot_out == (void *)channel_out)
        return APKG_INVALID;
    apkg_ctx *ctx = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_channel *channel = NULL;
    apkg_status code =
        ak_cancelled(observer)
            ? APKG_CANCELLED
            : open_source(root, endpoint, identity, floor, APKG_READ, observer,
                          &ctx, &channel, error);
    if (!code)
        code = apkg_snapshot_read(ctx, &snapshot, error);
    if (!code)
        code = ak_channel_snapshot_capture(snapshot, channel);
    if (!code && ak_cancelled(observer))
        code = APKG_CANCELLED;
    if (!code) {
        *snapshot_out = snapshot;
        snapshot = NULL;
        *channel_out = channel;
        channel = NULL;
    }
    apkg_channel_free(channel);
    apkg_snapshot_free(snapshot);
    apkg_close(ctx);
    return code;
}
apkg_status apkg_client_channel_updates(const char *root, const char *endpoint,
                                        const char *identity, uint64_t floor,
                                        const apkg_observer *observer,
                                        char **out, apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_channel *channel = NULL;
    apkg_status code = apkg_client_channel_refresh(
        root, endpoint, identity, floor, observer, &snapshot, &channel, error);
    if (code)
        return code;
    ak_buf b = {0};
    b.max = AK_MAX / 8;
    const apkg_channel_view *source = apkg_channel_get(channel);
    const apkg_root_view *state = apkg_snapshot_root(snapshot);
    char root_sha[65], channel_sha[65], channel_id[33];
    ak_tohex(state->snapshot_digest.bytes, 32, root_sha);
    ak_tohex(source->snapshot_digest.bytes, 32, channel_sha);
    ak_tohex(source->id.bytes, 16, channel_id);
    ak_buf_add(&b,
               "{\"channel_id\":\"%s\",\"channel_sequence\":\"%" PRIu64
               "\",\"channel_snapshot_sha256\":\"%s\",\"entries\":[",
               channel_id, source->sequence, channel_sha);
    for (uint64_t i = 0; i < source->entry_count && !code && !b.bad; i++) {
        const apkg_channel_entry_view *row = NULL;
        apkg_candidate_view comparison;
        char *selection = NULL;
        code = ak_cancelled(observer)
                   ? APKG_CANCELLED
                   : apkg_channel_compare(snapshot, channel, i, &comparison,
                                          error);
        if (!code)
            code = apkg_channel_entry_at(channel, i, &row, error);
        if (!code)
            code = apkg_client_channel_selection(channel, i, &selection, error);
        if (code)
            break;
        ak_buf_add(&b,
                   "%s{\"author_trust\":\"%s\",\"authority\":\"%.*s\","
                   "\"comparison\":\"%s\",\"installed_revision\":",
                   i ? "," : "",
                   row->author_trust == APKG_TRUST_ACCEPTED ? "accepted"
                                                            : "rejected",
                   (int)row->authority.size, row->authority.data,
                   apkg_candidate_state_name(comparison.state));
        if (comparison.installed &&
            comparison.installed->revision.state == APKG_KNOWN)
            ak_buf_add(&b, "\"%" PRIu64 "\"",
                       comparison.installed->revision.value);
        else
            ak_buf_add(&b, "null");
        ak_buf_add(&b, ",\"installed_version\":");
        if (comparison.installed &&
            comparison.installed->version.state == APKG_KNOWN)
            ak_buf_add(&b, "\"%.*s\"",
                       (int)comparison.installed->version.value.size,
                       comparison.installed->version.value.data);
        else
            ak_buf_add(&b, "null");
        ak_buf_add(
            &b,
            ",\"installed_authority\":\"%.*s\",\"installed_target\":\"%.*s\","
            "\"name\":\"%.*s\",\"plan_readiness\":\"not-checked\",\"review\":"
            "\"not-asserted\",\"revision\":\"%" PRIu64
            "\",\"runtime_tests\":\"not-asserted\",\"selection\":\"%s\","
            "\"target\":\"%.*s\",\"target_matches\":%s,\"version\":\"%.*s\"",
            comparison.installed
                ? (int)comparison.installed->authority.value.size
                : 0,
            comparison.installed ? comparison.installed->authority.value.data
                                 : "",
            comparison.installed ? (int)comparison.installed->target.value.size
                                 : 0,
            comparison.installed ? comparison.installed->target.value.data : "",
            (int)row->name.size, row->name.data, row->revision, selection,
            (int)row->target.size, row->target.data,
            row->target_matches ? "true" : "false", (int)row->version.size,
            row->version.data);
        ak_buf_add(&b, ",\"attestations\":");
        ak_channel_claims_json(channel, i, &b);
        ak_buf_add(&b, "}");
        apkg_client_text_free(selection);
    }
    ak_buf_add(&b,
               "],\"format\":\"apkg-channel-updates-1\",\"freshness\":"
               "\"unknown\",\"root_generation\":\"%" PRIu64
               "\",\"root_snapshot_sha256\":\"%s\"}",
               state->generation.value, root_sha);
    if (!code)
        code = b.bad;
    if (!code) {
        *out = b.data;
        b.data = NULL;
    }
    free(b.data);
    apkg_snapshot_free(snapshot);
    apkg_channel_free(channel);
    return code;
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
    apkg_channel *channel = NULL;
    apkg_status code = apkg_client_channel_open(root, endpoint, identity, floor,
                                                observer, &channel, error);
    if (!code)
        code = ak_channel_format(channel, out);
    apkg_channel_free(channel);
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
