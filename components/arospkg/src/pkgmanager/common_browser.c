/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#ifdef APKG_FUSION
#include "apkg_client.h"
#include "job.h"
#include "pkg_target.h"
#include <limits.h>
#include <proto/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PM_CATALOGUE_MAX (16u * 1024u * 1024u)
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
/* UI rows are a display projection. Refuse an unrepresentable field rather
 * than truncate an identity and use that truncated name in an operation. */
pkg_status pm_common_query(const char *root, const char *term, pkg_entries *out,
                           pkg_err *error) {
    apkg_open_options o = {0};
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    apkg_status code;
    size_t i;
    pkg_entries fresh = {0};
    memset(out, 0, sizeof *out);
    o.contract_version = 1;
    o.struct_size = sizeof o;
    o.root.data = root;
    o.root.size = strlen(root);
    code = apkg_open(&o, &c, NULL);
    if (code)
        goto done;
    code = apkg_snapshot_read(c, &s, NULL);
    if (code)
        goto done;
    if (apkg_snapshot_root(s)->state != APKG_ROOT_READY) {
        code = APKG_POLICY;
        goto done;
    }
    uint64_t count = apkg_snapshot_installed_count(s);
    if (count > INT_MAX) {
        code = APKG_LIMIT;
        goto done;
    }
    fresh.v = calloc(count ? count : 1, sizeof *fresh.v);
    if (!fresh.v) {
        code = APKG_NOMEM;
        goto done;
    }
    fresh.cap = (int)count;
    for (i = 0; i < count; i++) {
        const apkg_installed_view *v;
        struct pkg_target target;
        pkg_entry *r = &fresh.v[fresh.n];
        code = apkg_snapshot_installed_at(s, i, &v, NULL);
        if (code)
            goto done;
        if (term && *term && !strstr(v->name.value.data, term))
            continue;
        if (v->name.value.size >= sizeof r->id ||
            v->version.value.size >= sizeof r->version ||
            v->revision.value > LONG_MAX ||
            pkg_target_parse(v->target.value.data, &target, NULL, 0)) {
            code = APKG_LIMIT;
            goto done;
        }
        memcpy(r->id, v->name.value.data, v->name.value.size + 1);
        memcpy(r->version, v->version.value.data, v->version.value.size + 1);
        memcpy(r->installed_version, r->version, strlen(r->version) + 1);
        r->revision = r->installed_revision = (long)v->revision.value;
        snprintf(r->arch, sizeof r->arch, "%s", target.cpu);
        snprintf(r->abi, sizeof r->abi, "%s", target.abi);
        snprintf(r->kind, sizeof r->kind, "package");
        snprintf(r->summary, sizeof r->summary,
                 "Installed; qualification not asserted");
        r->installed = 1;
        r->size = -1;
        fresh.n++;
    }
    *out = fresh;
    memset(&fresh, 0, sizeof fresh);
done:
    free(fresh.v);
    apkg_snapshot_free(s);
    apkg_close(c);
    if (error) {
        memset(error, 0, sizeof *error);
        error->status = code ? PKG_E_IO : PKG_OK;
        if (code)
            snprintf(error->summary, sizeof error->summary,
                     "common root query refused (code %d)", (int)code);
    }
    return code ? PKG_E_IO : PKG_OK;
}
#endif
