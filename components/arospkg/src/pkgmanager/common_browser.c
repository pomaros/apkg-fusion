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
int pm_common_row_reviewable(const pm_common_row *row) {
    return row && row->target_matches &&
           row->signature == APKG_SIGNATURE_VALID &&
           row->author_trust == APKG_TRUST_ACCEPTED &&
           (row->comparison == APKG_CANDIDATE_NOT_INSTALLED ||
            row->comparison == APKG_CANDIDATE_CURRENT ||
            row->comparison == APKG_CANDIDATE_NEWER);
}
int pm_common_row_matches(const pm_common_row *row, pm_catalogue_filter filter,
                          const char *term) {
    if (!row || (unsigned)filter >= PM_CATALOGUE_FILTER_COUNT)
        return 0;
    if (term && *term && !strstr(row->name, term) &&
        !strstr(row->authority, term) && !strstr(row->target, term))
        return 0;
    switch (filter) {
    case PM_CATALOGUE_ALL:
        return 1;
    case PM_CATALOGUE_UPDATES:
        return row->comparison != APKG_CANDIDATE_NOT_INSTALLED &&
               row->comparison != APKG_CANDIDATE_CURRENT &&
               row->comparison != APKG_CANDIDATE_OLDER;
    case PM_CATALOGUE_NOT_INSTALLED:
        return row->comparison == APKG_CANDIDATE_NOT_INSTALLED;
    case PM_CATALOGUE_OTHER_TARGET:
        return !row->target_matches ||
               row->comparison == APKG_CANDIDATE_OTHER_TARGET;
    case PM_CATALOGUE_UNTRUSTED:
        return row->author_trust != APKG_TRUST_ACCEPTED ||
               row->signature != APKG_SIGNATURE_VALID;
    case PM_CATALOGUE_CONTENT_CONFLICT:
        return row->comparison == APKG_CANDIDATE_CONTENT_CONFLICT;
    case PM_CATALOGUE_INDETERMINATE:
        return row->comparison == APKG_CANDIDATE_INDETERMINATE;
    default:
        return 0;
    }
}
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
/* A copied signed observation is revalidated in the current read context.
 * The UI keeps its old AllocVec alive until the replacement is complete. */
static apkg_status catalogue_open(const char *root,
                                  const pm_common_catalogue *model,
                                  apkg_ctx **ctx, apkg_snapshot **snapshot,
                                  apkg_channel **channel) {
    *ctx = NULL;
    *snapshot = NULL;
    *channel = NULL;
    if (!root)
        return APKG_INVALID;
    if (!model)
        return APKG_NOT_FOUND;
    apkg_open_options options = {0};
    options.contract_version = APKG_CONTRACT_VERSION;
    options.struct_size = sizeof options;
    options.root = (apkg_text){root, strlen(root)};
    apkg_status code = apkg_open(&options, ctx, NULL);
    if (!code)
        code =
            apkg_channel_read(*ctx, &model->channel.id, 0, model->observation,
                              model->observation_signature, channel, NULL);
    if (!code && memcmp(apkg_channel_get(*channel)->snapshot_digest.bytes,
                        model->channel.snapshot_digest.bytes, 32))
        code = APKG_CONFLICT;
    if (!code)
        code = apkg_snapshot_read(*ctx, snapshot, NULL);
    return code;
}
apkg_status pm_common_catalogue_refresh(const char *root,
                                        const pm_common_catalogue *model,
                                        pm_common_catalogue **out) {
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    apkg_ctx *ctx = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_channel *channel = NULL;
    apkg_status code = catalogue_open(root, model, &ctx, &snapshot, &channel);
    if (!code)
        code =
            pm_common_catalogue_copy(channel, snapshot, model->endpoint, out);
    apkg_channel_free(channel);
    apkg_snapshot_free(snapshot);
    apkg_close(ctx);
    return code;
}
apkg_status pm_common_source_load(const char *root, const char *profile,
                                  pm_common_catalogue **out) {
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    apkg_source_config *config = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_channel *channel = NULL;
    apkg_status code = apkg_source_config_read(profile, &config, NULL);
    if (!code)
        code = apkg_source_config_open(config, root, &snapshot, &channel, NULL);
    if (!code)
        code = pm_common_catalogue_copy(
            channel, snapshot, apkg_source_config_get(config)->endpoint.data,
            out);
    apkg_channel_free(channel);
    apkg_snapshot_free(snapshot);
    apkg_source_config_free(config);
    return code;
}
apkg_status pm_common_source_save(const char *root, const char *profile,
                                  const char *endpoint, const char *identity,
                                  const pm_common_catalogue *model) {
    if (!identity || strlen(identity) != 32 || !endpoint)
        return APKG_INVALID;
    apkg_id id;
    for (size_t i = 0; i < 16; i++) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; j++) {
            char c = identity[2 * i + j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return APKG_INVALID;
            value = 16 * value + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        id.bytes[i] = (unsigned char)value;
    }
    apkg_ctx *ctx = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_channel *channel = NULL;
    apkg_status code = APKG_OK;
    if (model && !strcmp(endpoint, model->endpoint) &&
        !memcmp(id.bytes, model->channel.id.bytes, 16))
        code = catalogue_open(root, model, &ctx, &snapshot, &channel);
    if (!code)
        code = apkg_source_config_save(profile, endpoint, &id, channel, NULL);
    apkg_channel_free(channel);
    apkg_snapshot_free(snapshot);
    apkg_close(ctx);
    return code;
}
apkg_status pm_common_rexx_outdated(const char *root,
                                    const pm_common_catalogue *model,
                                    const char *field, char **out,
                                    pm_common_catalogue **refreshed) {
    if (refreshed)
        *refreshed = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    apkg_ctx *ctx = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_channel *channel = NULL;
    apkg_update_summary *updates = NULL;
    pm_common_catalogue *replacement = NULL;
    apkg_status code;
    if (refreshed) {
        code = pm_common_catalogue_refresh(root, model, &replacement);
        if (!code)
            code = apkg_update_summary_format(&replacement->updates, field, out,
                                              NULL);
    } else {
        code = catalogue_open(root, model, &ctx, &snapshot, &channel);
        if (!code)
            code = apkg_channel_summarize(snapshot, channel, &updates, NULL);
        if (!code)
            code = apkg_update_summary_format(apkg_update_summary_get(updates),
                                              field, out, NULL);
    }
    if (!code && strlen(*out) > RX_MAXRESULT) {
        apkg_client_text_free(*out);
        *out = NULL;
        code = APKG_LIMIT;
    }
    if (!code && refreshed) {
        *refreshed = replacement;
        replacement = NULL;
    }
    FreeVec(replacement);
    apkg_update_summary_free(updates);
    apkg_channel_free(channel);
    apkg_snapshot_free(snapshot);
    apkg_close(ctx);
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
    "asserted\nComparison: %s\nInstalled version: %.*s\nInstalled revision: "
    "%s\n"
    "Installed authority: %.*s\nInstalled target: %.*s\n"
    "Plan readiness: not checked\n\n%.*s";
static int row_detail(char *out, size_t size, const apkg_channel_entry_view *v,
                      const apkg_candidate_view *comparison) {
    apkg_text installed = {"not installed", 13};
    char revision[32] = "not installed";
    if (comparison->installed) {
        installed = comparison->installed->version.state == APKG_KNOWN
                        ? comparison->installed->version.value
                        : (apkg_text){"unknown", 7};
        if (comparison->installed->revision.state == APKG_KNOWN)
            snprintf(revision, sizeof revision, "%llu",
                     (unsigned long long)comparison->installed->revision.value);
        else
            snprintf(revision, sizeof revision, "unknown");
    }
    return snprintf(
        out, size, row_format, (int)v->name.size, v->name.data,
        (int)v->authority.size, v->authority.data, (int)v->version.size,
        v->version.data, (unsigned long long)v->revision, (int)v->target.size,
        v->target.data, (unsigned long long)v->archive_bytes,
        v->signature == APKG_SIGNATURE_VALID ? "verified" : "not verified",
        v->author_trust == APKG_TRUST_ACCEPTED ? "accepted" : "not accepted",
        v->target_matches ? "yes" : "no",
        apkg_candidate_state_name(comparison->state), (int)installed.size,
        installed.data, revision,
        comparison->installed ? (int)comparison->installed->authority.value.size
                              : 0,
        comparison->installed ? comparison->installed->authority.value.data
                              : "",
        comparison->installed ? (int)comparison->installed->target.value.size
                              : 0,
        comparison->installed ? comparison->installed->target.value.data : "",
        (int)v->descriptor.size, (const char *)v->descriptor.data);
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
                                     const apkg_snapshot *snapshot,
                                     const char *endpoint,
                                     pm_common_catalogue **out) {
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!channel || !snapshot || !endpoint || strlen(endpoint) >= 4096)
        return APKG_INVALID;
    apkg_bytes object = {0}, signature = {0};
    apkg_status observation_code =
        apkg_channel_observation(channel, &object, &signature);
    if (observation_code)
        return observation_code;
    const apkg_channel_view *view = apkg_channel_get(channel);
    if (view->entry_count > 4096)
        return APKG_LIMIT;
    size_t total = sizeof(pm_common_catalogue) +
                   (size_t)view->entry_count * sizeof(pm_common_row) +
                   strlen(endpoint) + 1;
    if (object.size > PM_CATALOGUE_MAX - total - 2 ||
        signature.size > PM_CATALOGUE_MAX - total - object.size - 2)
        return APKG_LIMIT;
    total += object.size + signature.size + 2;
    int summary_size = channel_summary(NULL, 0, view);
    if (summary_size < 0)
        return APKG_INVALID;
    if ((size_t)summary_size >= PM_CATALOGUE_MAX - total)
        return APKG_LIMIT;
    total += (size_t)summary_size + 1;
    if (view->accepted_sequence.evidence.size >= PM_CATALOGUE_MAX - total)
        return APKG_LIMIT;
    total += view->accepted_sequence.evidence.size + 1;
    for (uint64_t i = 0; i < view->entry_count; i++) {
        const apkg_channel_entry_view *v = NULL;
        apkg_status code = apkg_channel_entry_at(channel, i, &v, NULL);
        if (code)
            return code;
        apkg_candidate_view comparison;
        code = apkg_channel_compare(snapshot, channel, i, &comparison, NULL);
        if (code)
            return code;
        int n = row_detail(NULL, 0, v, &comparison);
        if (n < 0)
            return APKG_INVALID;
        size_t extra =
            v->name.size + v->version.size + v->target.size +
            v->authority.size + 4 + (size_t)n + 1 +
            (comparison.installed ? comparison.installed->version.value.size
                                  : 0) +
            1;
        if (extra > PM_CATALOGUE_MAX || total > PM_CATALOGUE_MAX - extra)
            return APKG_LIMIT;
        total += extra;
    }
    apkg_update_summary *updates = NULL;
    apkg_status code =
        apkg_channel_summarize(snapshot, channel, &updates, NULL);
    if (code)
        return code;
    const apkg_update_summary_view *summary = apkg_update_summary_get(updates);
    size_t extra =
        (size_t)summary->entry_count * sizeof(apkg_update_entry_view);
    for (uint64_t i = 0; i < summary->entry_count; i++) {
        const apkg_update_entry_view *r = &summary->entries[i];
        extra += r->authority.size + r->name.size + r->target.size +
                 r->installed_version.size + 4;
    }
    if (extra > PM_CATALOGUE_MAX || total > PM_CATALOGUE_MAX - extra) {
        apkg_update_summary_free(updates);
        return APKG_LIMIT;
    }
    total += extra;
    pm_common_catalogue *model = AllocVec(total, MEMF_PUBLIC | MEMF_CLEAR);
    if (!model) {
        apkg_update_summary_free(updates);
        return APKG_NOMEM;
    }
    model->count = (size_t)view->entry_count;
    model->channel = *view;
    model->root_generation = apkg_snapshot_root(snapshot)->generation.value;
    model->root_snapshot = apkg_snapshot_root(snapshot)->snapshot_digest;
    apkg_update_entry_view *entries =
        (apkg_update_entry_view *)(model->rows + model->count);
    char *cursor = (char *)(entries + summary->entry_count);
    model->observation = (apkg_bytes){
        (const unsigned char *)copy_text(
            &cursor, (apkg_text){(const char *)object.data, object.size}),
        object.size};
    model->observation_signature = (apkg_bytes){
        (const unsigned char *)copy_text(
            &cursor, (apkg_text){(const char *)signature.data, signature.size}),
        signature.size};
    model->updates = *summary;
    model->updates.entries = entries;
    for (uint64_t i = 0; i < summary->entry_count; i++) {
        entries[i] = summary->entries[i];
        entries[i].authority.data = copy_text(&cursor, entries[i].authority);
        entries[i].name.data = copy_text(&cursor, entries[i].name);
        entries[i].target.data = copy_text(&cursor, entries[i].target);
        entries[i].installed_version.data =
            copy_text(&cursor, entries[i].installed_version);
    }
    apkg_update_summary_free(updates);
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
        apkg_candidate_view comparison;
        if (!code)
            code =
                apkg_channel_compare(snapshot, channel, i, &comparison, NULL);
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
        r->comparison = comparison.state;
        r->installed_version = copy_text(
            &cursor, comparison.installed ? comparison.installed->version.value
                                          : (apkg_text){"", 0});
        if (comparison.installed) {
            r->installed_revision = comparison.installed->revision.value;
            r->installed_revision_state = comparison.installed->revision.state;
        }
        r->detail = cursor;
        int n = row_detail(cursor, total - (size_t)(cursor - (char *)model), v,
                           &comparison);
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
