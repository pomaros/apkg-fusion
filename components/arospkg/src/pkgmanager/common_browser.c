/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#ifdef APKG_FUSION
#include "apkg_client.h"
#include "job.h"
#include "rexxcmd.h"
#include <proto/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PM_CATALOGUE_MAX (16u * 1024u * 1024u)
apkg_status pm_common_rexx_info(const char *root, const char *name,
                                const char *field, char **out) {
    apkg_status code = apkg_client_info(root, name, field, out, NULL);
    if (!code && strlen(*out) > RX_MAXRESULT) {
        apkg_client_text_free(*out);
        *out = NULL;
        return APKG_LIMIT;
    }
    return code;
}
static int channel_summary(char *out, size_t size, const apkg_channel_view *v) {
    const char hex[] = "0123456789abcdef";
    char identity[33], digest[65], floor[32];
    for (size_t i = 0; i < 32; i++) {
        digest[2 * i] = hex[v->snapshot_digest.bytes[i] >> 4];
        digest[2 * i + 1] = hex[v->snapshot_digest.bytes[i] & 15];
        if (i < 16) {
            identity[2 * i] = hex[v->id.bytes[i] >> 4];
            identity[2 * i + 1] = hex[v->id.bytes[i] & 15];
        }
    }
    identity[32] = digest[64] = 0;
    if (v->accepted_sequence.state == APKG_KNOWN)
        snprintf(floor, sizeof floor, "%llu",
                 (unsigned long long)v->accepted_sequence.value);
    else
        snprintf(floor, sizeof floor, "unknown");
    return snprintf(
        out, size,
        "Channel: %s\nSnapshot: %s\nSequence: %llu; accepted floor: %s\n"
        "Channel signature: %s; channel trust: %s; online freshness: unknown",
        identity, digest, (unsigned long long)v->sequence, floor,
        v->signature == APKG_SIGNATURE_VALID ? "verified" : "not verified",
        v->trust == APKG_TRUST_ACCEPTED ? "accepted" : "not accepted");
}
static const char *row_format =
    "Package: %.*s\nAuthority: %.*s\nVersion: %.*s (revision %llu)\n"
    "Target: %.*s\nArchive bytes: %llu\nSignature: %s\nAuthor trust: %s\n"
    "Target matches: %s\nReview: not asserted\nRuntime testing: not "
    "asserted\n\n%.*s";
static int row_detail(char *out, size_t size,
                      const apkg_channel_entry_view *v) {
    return snprintf(
        out, size, row_format, (int)v->name.size, v->name.data,
        (int)v->authority.size, v->authority.data, (int)v->version.size,
        v->version.data, (unsigned long long)v->revision, (int)v->target.size,
        v->target.data, (unsigned long long)v->archive_bytes,
        v->signature == APKG_SIGNATURE_VALID ? "verified" : "not verified",
        v->author_trust == APKG_TRUST_ACCEPTED ? "accepted" : "not accepted",
        v->target_matches ? "yes" : "no", (int)v->descriptor.size,
        (const char *)v->descriptor.data);
}
static char *copy_text(char **cursor, apkg_text text) {
    char *out = *cursor;
    if (text.size)
        memcpy(out, text.data, text.size);
    out[text.size] = 0;
    *cursor += text.size + 1;
    return out;
}
apkg_status pm_common_catalogue_copy(const apkg_channel *channel,
                                     const char *endpoint,
                                     pm_common_catalogue **out) {
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!channel || !endpoint || strlen(endpoint) >= 4096)
        return APKG_INVALID;
    const apkg_channel_view *view = apkg_channel_get(channel);
    if (view->entry_count > 4096)
        return APKG_LIMIT;
    size_t total = sizeof(pm_common_catalogue) +
                   (size_t)view->entry_count * sizeof(pm_common_row) +
                   strlen(endpoint) + 1;
    int summary_size = channel_summary(NULL, 0, view);
    if (summary_size < 0)
        return APKG_INVALID;
    total += (size_t)summary_size + 1;
    if (view->accepted_sequence.evidence.size > PM_CATALOGUE_MAX - total - 1)
        return APKG_LIMIT;
    total += view->accepted_sequence.evidence.size + 1;
    for (uint64_t i = 0; i < view->entry_count; i++) {
        const apkg_channel_entry_view *v = NULL;
        apkg_status code = apkg_channel_entry_at(channel, i, &v, NULL);
        if (code)
            return code;
        int n = row_detail(NULL, 0, v);
        if (n < 0)
            return APKG_INVALID;
        size_t extra = v->name.size + v->version.size + v->target.size +
                       v->authority.size + 4 + (size_t)n + 1;
        if (extra > PM_CATALOGUE_MAX || total > PM_CATALOGUE_MAX - extra)
            return APKG_LIMIT;
        total += extra;
    }
    pm_common_catalogue *model = AllocVec(total, MEMF_PUBLIC | MEMF_CLEAR);
    if (!model)
        return APKG_NOMEM;
    model->count = (size_t)view->entry_count;
    model->channel = *view;
    char *cursor = (char *)(model->rows + model->count);
    model->channel.accepted_sequence.evidence.data =
        copy_text(&cursor, view->accepted_sequence.evidence);
    model->summary = cursor;
    channel_summary(cursor, (size_t)summary_size + 1, view);
    cursor += (size_t)summary_size + 1;
    model->endpoint =
        copy_text(&cursor, (apkg_text){endpoint, strlen(endpoint)});
    for (size_t i = 0; i < model->count; i++) {
        const apkg_channel_entry_view *v = NULL;
        apkg_status code = apkg_channel_entry_at(channel, i, &v, NULL);
        char *selection = NULL;
        if (!code)
            code = apkg_client_channel_selection(channel, i, &selection, NULL);
        if (code) {
            FreeVec(model);
            return code;
        }
        pm_common_row *r = &model->rows[i];
        memcpy(r->selection, selection, sizeof r->selection);
        apkg_client_text_free(selection);
        r->name = copy_text(&cursor, v->name);
        r->version = copy_text(&cursor, v->version);
        r->target = copy_text(&cursor, v->target);
        r->authority = copy_text(&cursor, v->authority);
        r->revision = v->revision;
        r->archive_bytes = v->archive_bytes;
        r->signature = v->signature;
        r->author_trust = v->author_trust;
        r->target_matches = v->target_matches;
        r->detail = cursor;
        int n = row_detail(cursor, total - (size_t)(cursor - (char *)model), v);
        if (n < 0) {
            FreeVec(model);
            return APKG_INVALID;
        }
        cursor += (size_t)n + 1;
    }
    *out = model;
    return APKG_OK;
}
/* Full discovery facts stay in owned JSON; no projection can truncate a
 * channel/package identity. Caller releases it with apkg_client_text_free. */
pkg_status pm_common_channel_query(const char *root, const char *endpoint,
                                   const char *identity, char **out,
                                   pkg_err *error) {
    apkg_status code =
        apkg_client_channel_query(root, endpoint, identity, 0, NULL, out, NULL);
    if (error) {
        memset(error, 0, sizeof *error);
        error->status = code ? PKG_E_IO : PKG_OK;
        if (code)
            snprintf(error->summary, sizeof error->summary,
                     "common channel query refused (code %d)", (int)code);
    }
    return code ? PKG_E_IO : PKG_OK;
}
static int contains(apkg_text text, const char *term) {
    size_t n = term ? strlen(term) : 0;
    if (!n)
        return 1;
    for (size_t i = 0; n <= text.size && i <= text.size - n; i++)
        if (!memcmp(text.data + i, term, n))
            return 1;
    return 0;
}
static int installed_matches(const apkg_installed_view *v, const char *term) {
    return contains(v->name.value, term) ||
           contains(v->authority.value, term) ||
           contains(v->target.value, term);
}
static int installed_version(char *out, size_t size,
                             const apkg_installed_view *v) {
    const char *version =
        v->version.state == APKG_KNOWN ? v->version.value.data : "unknown";
    int n = v->version.state == APKG_KNOWN ? (int)v->version.value.size : 7;
    if (v->revision.state == APKG_KNOWN)
        return snprintf(out, size, "%.*s (revision %llu)", n, version,
                        (unsigned long long)v->revision.value);
    return snprintf(out, size, "%.*s (revision unknown)", n, version);
}
static size_t installed_texts(const apkg_installed_view *v,
                              apkg_text parts[10]) {
    parts[0] = v->authority.value;
    parts[1] = v->authority.evidence;
    parts[2] = v->name.value;
    parts[3] = v->name.evidence;
    parts[4] = v->version.value;
    parts[5] = v->version.evidence;
    parts[6] = v->target.value;
    parts[7] = v->target.evidence;
    parts[8] = v->revision.evidence;
    parts[9] = (apkg_text){(const char *)v->record.data, v->record.size};
    size_t size = 0;
    for (size_t i = 0; i < 10; i++)
        size += parts[i].size + 1;
    return size;
}
apkg_status pm_common_installed_read(const char *root, const char *term,
                                     pm_common_installed **out,
                                     apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!root)
        return APKG_INVALID;
    apkg_open_options options = {0};
    options.contract_version = APKG_CONTRACT_VERSION;
    options.struct_size = sizeof options;
    options.root = (apkg_text){root, strlen(root)};
    apkg_ctx *ctx = NULL;
    apkg_snapshot *snapshot = NULL;
    pm_common_installed *model = NULL;
    apkg_status code = apkg_open(&options, &ctx, error);
    if (code)
        goto done;
    code = apkg_snapshot_read(ctx, &snapshot, error);
    if (code)
        goto done;
    apkg_root_state state = apkg_snapshot_root(snapshot)->state;
    if (state != APKG_ROOT_READY) {
        code =
            state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED : APKG_POLICY;
        goto done;
    }
    uint64_t count = apkg_snapshot_installed_count(snapshot);
    if (count > 4096) {
        code = APKG_LIMIT;
        goto done;
    }
    size_t total = sizeof *model + (size_t)count * sizeof *model->rows;
    for (uint64_t i = 0; i < count; i++) {
        const apkg_installed_view *v = NULL;
        code = apkg_snapshot_installed_at(snapshot, i, &v, error);
        if (code)
            goto done;
        if (!installed_matches(v, term))
            continue;
        apkg_text parts[10];
        int n = installed_version(NULL, 0, v);
        if (n < 0) {
            code = APKG_INVALID;
            goto done;
        }
        size_t extra = installed_texts(v, parts) + (size_t)n + 1;
        if (extra > PM_CATALOGUE_MAX || total > PM_CATALOGUE_MAX - extra) {
            code = APKG_LIMIT;
            goto done;
        }
        total += extra;
    }
    model = AllocVec(total, MEMF_PUBLIC | MEMF_CLEAR);
    if (!model) {
        code = APKG_NOMEM;
        goto done;
    }
    char *cursor = (char *)(model->rows + count);
    for (uint64_t i = 0; i < count; i++) {
        const apkg_installed_view *v = NULL;
        code = apkg_snapshot_installed_at(snapshot, i, &v, error);
        if (code)
            goto done;
        if (!installed_matches(v, term))
            continue;
        pm_common_installed_row *row = &model->rows[model->count++];
        row->view = *v;
        apkg_text parts[10];
        installed_texts(v, parts);
        apkg_text *targets[9] = {
            &row->view.authority.value,  &row->view.authority.evidence,
            &row->view.name.value,       &row->view.name.evidence,
            &row->view.version.value,    &row->view.version.evidence,
            &row->view.target.value,     &row->view.target.evidence,
            &row->view.revision.evidence};
        for (size_t k = 0; k < 9; k++)
            targets[k]->data = copy_text(&cursor, parts[k]);
        row->view.record.data =
            (const unsigned char *)copy_text(&cursor, parts[9]);
        row->version_label = cursor;
        int n = installed_version(cursor,
                                  total - (size_t)(cursor - (char *)model), v);
        if (n < 0) {
            code = APKG_INVALID;
            goto done;
        }
        cursor += (size_t)n + 1;
    }
    *out = model;
    model = NULL;
done:
    FreeVec(model);
    apkg_snapshot_free(snapshot);
    apkg_close(ctx);
    return code;
}
#endif
