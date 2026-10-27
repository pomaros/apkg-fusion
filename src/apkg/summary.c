/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include <inttypes.h>
#include <string.h>

struct apkg_update_summary {
    apkg_update_summary_view view;
    apkg_update_entry_view *entries;
};
static int text_order(apkg_text a, apkg_text b) {
    size_t size = a.size < b.size ? a.size : b.size;
    int order = size ? memcmp(a.data, b.data, size) : 0;
    return order ? order : a.size < b.size ? -1 : a.size > b.size;
}
static int qualified_order(const void *a, const void *b) {
    const apkg_installed_view *x = *(const apkg_installed_view *const *)a;
    const apkg_installed_view *y = *(const apkg_installed_view *const *)b;
    int order = text_order(x->authority.value, y->authority.value);
    if (!order)
        order = text_order(x->name.value, y->name.value);
    return order ? order : text_order(x->target.value, y->target.value);
}
static int name_order(const void *a, const void *b) {
    const apkg_installed_view *x = *(const apkg_installed_view *const *)a;
    const apkg_installed_view *y = *(const apkg_installed_view *const *)b;
    return text_order(x->name.value, y->name.value);
}
void apkg_update_summary_free(apkg_update_summary *summary) {
    if (!summary)
        return;
    for (uint64_t i = 0; i < summary->view.entry_count; i++) {
        apkg_update_entry_view *r = &summary->entries[i];
        free((void *)r->authority.data);
        free((void *)r->name.data);
        free((void *)r->target.data);
        free((void *)r->installed_version.data);
    }
    free(summary->entries);
    free(summary);
}
const apkg_update_summary_view *
apkg_update_summary_get(const apkg_update_summary *s) {
    return s ? &s->view : NULL;
}
apkg_status apkg_channel_summarize(const apkg_snapshot *snapshot,
                                   const apkg_channel *channel,
                                   apkg_update_summary **out,
                                   apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    apkg_status code = ak_channel_snapshot_check(snapshot, channel);
    if (code)
        return code;
    if (snapshot->nrows > 4096)
        return APKG_LIMIT;
    apkg_update_summary *summary = calloc(1, sizeof *summary);
    const apkg_installed_view **ordered =
        calloc(snapshot->nrows ? snapshot->nrows : 1, sizeof *ordered);
    unsigned char *flags = calloc(snapshot->nrows ? snapshot->nrows : 1, 1);
    uint64_t *counts =
        calloc(snapshot->nrows ? snapshot->nrows * 2 : 2, sizeof *counts);
    if (!summary || !ordered || !flags || !counts) {
        code = APKG_NOMEM;
        goto done;
    }
    uint64_t *eligible = counts + snapshot->nrows;
    apkg_update_summary_view *v = &summary->view;
    const apkg_channel_view *source = apkg_channel_get(channel);
    v->installed = snapshot->nrows;
    v->generation = snapshot->view.generation.value;
    v->root_snapshot = snapshot->view.snapshot_digest;
    v->channel_snapshot = source->snapshot_digest;
    v->channel_id = source->id;
    v->sequence = source->sequence;
    for (size_t i = 0; i < snapshot->nrows; i++) {
        const apkg_installed_view *r = &snapshot->rows[i];
        if (r->authority.state != APKG_KNOWN || r->name.state != APKG_KNOWN ||
            r->target.state != APKG_KNOWN) {
            code = APKG_POLICY;
            goto done;
        }
        ordered[i] = r;
    }
    if (snapshot->nrows > 1)
        qsort(ordered, snapshot->nrows, sizeof *ordered, qualified_order);
    for (size_t i = 1; i < snapshot->nrows; i++)
        if (!qualified_order(&ordered[i - 1], &ordered[i])) {
            code = APKG_CONFLICT;
            goto done;
        }
    for (uint64_t i = 0; i < source->entry_count; i++) {
        const apkg_channel_entry_view *r = NULL;
        apkg_candidate_view comparison;
        code = apkg_channel_compare(snapshot, channel, i, &comparison, error);
        if (!code)
            code = apkg_channel_entry_at(channel, i, &r, error);
        if (code)
            goto done;
        if (comparison.state == APKG_CANDIDATE_INDETERMINATE)
            v->indeterminate_candidates++;
        if (comparison.state == APKG_CANDIDATE_CONTENT_CONFLICT ||
            comparison.state == APKG_CANDIDATE_OTHER_AUTHORITY ||
            comparison.state == APKG_CANDIDATE_OTHER_TARGET)
            v->conflict_candidates++;
        if (!comparison.installed ||
            comparison.state == APKG_CANDIDATE_OTHER_AUTHORITY ||
            comparison.state == APKG_CANDIDATE_OTHER_TARGET)
            continue;
        size_t old = (size_t)(comparison.installed - snapshot->rows);
        flags[old] |= 1;
        if (comparison.state != APKG_CANDIDATE_NEWER)
            continue;
        counts[old]++;
        v->newer_candidates++;
        if (r->signature == APKG_SIGNATURE_VALID &&
            r->author_trust == APKG_TRUST_ACCEPTED && r->target_matches)
            eligible[old]++;
        else
            v->blocked_candidates++;
    }
    for (size_t i = 0; i < snapshot->nrows; i++) {
        if (!(flags[i] & 1))
            v->not_listed++;
        if (counts[i]) {
            v->newer++;
            if (eligible[i])
                v->metadata_eligible++;
            else
                v->blocked++;
        }
    }
    if (v->newer) {
        summary->entries = calloc((size_t)v->newer, sizeof *summary->entries);
        if (!summary->entries) {
            code = APKG_NOMEM;
            goto done;
        }
    }
    v->entries = summary->entries;
    for (size_t i = 0; i < snapshot->nrows; i++) {
        const apkg_installed_view *old = ordered[i];
        size_t index = (size_t)(old - snapshot->rows);
        if (!counts[index])
            continue;
        apkg_update_entry_view *r = &summary->entries[v->entry_count++];
        apkg_text source_text[] = {old->authority.value, old->name.value,
                                   old->target.value, old->version.value};
        apkg_text *dest[] = {&r->authority, &r->name, &r->target,
                             &r->installed_version};
        for (size_t k = 0; k < 4; k++) {
            dest[k]->data = ak_copy(source_text[k].data, source_text[k].size);
            dest[k]->size = source_text[k].size;
            if (!dest[k]->data) {
                code = APKG_NOMEM;
                goto done;
            }
        }
        r->installed_revision = old->revision.value;
        r->newer_candidates = counts[index];
        r->eligible_candidates = eligible[index];
    }
    if (snapshot->nrows > 1)
        qsort(ordered, snapshot->nrows, sizeof *ordered, name_order);
    for (size_t begin = 0; begin < snapshot->nrows;) {
        size_t end = begin + 1;
        while (end < snapshot->nrows && !text_order(ordered[begin]->name.value,
                                                    ordered[end]->name.value))
            end++;
        if (end - begin > 1)
            for (size_t i = begin; i < end; i++)
                if (counts[ordered[i] - snapshot->rows])
                    v->names_ambiguous = 1;
        begin = end;
    }
    *out = summary;
    summary = NULL;
    code = APKG_OK;
done:
    apkg_update_summary_free(summary);
    free(ordered);
    free(flags);
    free(counts);
    return code;
}

static const char fields[] =
    "catalogue updated stale installed newer ready blocked packages "
    "metadataeligible "
    "newercandidates blockedcandidates indeterminate conflicts notlisted "
    "qualifiedpackages generation snapshot "
    "channel sequence channelsnapshot planreadiness freshness review runtime";
const char *apkg_outdated_fields(void) { return fields; }
static void summary_value(ak_buf *b, const apkg_update_summary_view *v,
                          const char *f) {
    if (!strcmp(f, "catalogue"))
        ak_buf_add(b, "ok");
    else if (!strcmp(f, "installed"))
        ak_buf_add(b, "%" PRIu64, v->installed);
    else if (!strcmp(f, "newer"))
        ak_buf_add(b, "%" PRIu64, v->newer);
    else if (!strcmp(f, "blocked"))
        ak_buf_add(b, "%" PRIu64, v->blocked);
    else if (!strcmp(f, "metadataeligible"))
        ak_buf_add(b, "%" PRIu64, v->metadata_eligible);
    else if (!strcmp(f, "newercandidates"))
        ak_buf_add(b, "%" PRIu64, v->newer_candidates);
    else if (!strcmp(f, "blockedcandidates"))
        ak_buf_add(b, "%" PRIu64, v->blocked_candidates);
    else if (!strcmp(f, "indeterminate"))
        ak_buf_add(b, "%" PRIu64, v->indeterminate_candidates);
    else if (!strcmp(f, "conflicts"))
        ak_buf_add(b, "%" PRIu64, v->conflict_candidates);
    else if (!strcmp(f, "notlisted"))
        ak_buf_add(b, "%" PRIu64, v->not_listed);
    else if (!strcmp(f, "generation"))
        ak_buf_add(b, "%" PRIu64, v->generation);
    else if (!strcmp(f, "sequence"))
        ak_buf_add(b, "%" PRIu64, v->sequence);
    else if (!strcmp(f, "packages") || !strcmp(f, "qualifiedpackages")) {
        if (!strcmp(f, "packages") && v->names_ambiguous) {
            b->bad = APKG_CONFLICT;
            return;
        }
        for (uint64_t i = 0; i < v->entry_count; i++) {
            const apkg_update_entry_view *r = &v->entries[i];
            if (i)
                ak_buf_add(b, " ");
            if (!strcmp(f, "qualifiedpackages"))
                ak_buf_add(b, "%.*s/", (int)r->authority.size,
                           r->authority.data);
            ak_buf_add(b, "%.*s", (int)r->name.size, r->name.data);
            if (!strcmp(f, "qualifiedpackages"))
                ak_buf_add(b, "/%.*s", (int)r->target.size, r->target.data);
        }
    } else if (!strcmp(f, "snapshot") || !strcmp(f, "channelsnapshot") ||
               !strcmp(f, "channel")) {
        char text[65];
        if (!strcmp(f, "channel"))
            ak_tohex(v->channel_id.bytes, 16, text);
        else
            ak_tohex(!strcmp(f, "snapshot") ? v->root_snapshot.bytes
                                            : v->channel_snapshot.bytes,
                     32, text);
        ak_buf_add(b, "%s", text);
    } else if (!strcmp(f, "planreadiness"))
        ak_buf_add(b, "not-checked");
    else if (!strcmp(f, "review") || !strcmp(f, "runtime"))
        ak_buf_add(b, "not-asserted");
    else
        ak_buf_add(b, "unknown");
}
apkg_status apkg_update_summary_format(const apkg_update_summary_view *view,
                                       const char *field, char **out,
                                       apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!view || view->entry_count != view->newer ||
        view->newer > view->installed ||
        view->metadata_eligible > view->newer ||
        view->blocked != view->newer - view->metadata_eligible ||
        view->entry_count > 4096 || (view->entry_count && !view->entries))
        return APKG_INVALID;
    if (view->plan_readiness != APKG_UNKNOWN || view->freshness != APKG_UNKNOWN)
        return APKG_UNSUPPORTED;
    for (uint64_t i = 0; i < view->entry_count; i++) {
        const apkg_update_entry_view *r = &view->entries[i];
        apkg_text t[] = {r->authority, r->name, r->target,
                         r->installed_version};
        for (size_t k = 0; k < 4; k++)
            if (!t[k].data || t[k].size > 255)
                return APKG_INVALID;
    }
    char wanted[32] = "";
    if (field) {
        if (strlen(field) >= sizeof wanted)
            return APKG_INVALID;
        for (size_t i = 0; field[i]; i++)
            wanted[i] = field[i] >= 'A' && field[i] <= 'Z'
                            ? field[i] - 'A' + 'a'
                            : field[i];
    }
    ak_buf b = {0};
    b.max = AK_MAX / 8;
    int found = 0;
    for (const char *p = fields; *p && !b.bad;) {
        char f[32];
        size_t n = strcspn(p, " ");
        memcpy(f, p, n);
        f[n] = 0;
        p += n;
        if (*p)
            p++;
        if (*wanted && strcmp(wanted, f))
            continue;
        if (!*wanted)
            ak_buf_add(&b, "%s%s ", found ? "\n" : "", f);
        summary_value(&b, view, f);
        found = 1;
    }
    if (!found && !b.bad)
        b.bad = ak_fail(error, APKG_INVALID, "unknown outdated field",
                        field ? field : "");
    if (b.bad) {
        free(b.data);
        return b.bad;
    }
    /* An empty package list is still a successful owned empty string. */
    if (!b.data)
        b.data = ak_copy("", 0);
    if (!b.data)
        return APKG_NOMEM;
    *out = b.data;
    return APKG_OK;
}
apkg_status apkg_client_outdated(const char *root, const char *endpoint,
                                 const char *identity, const char *field,
                                 const apkg_observer *observer, char **out,
                                 apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_channel *channel = NULL;
    apkg_update_summary *summary = NULL;
    apkg_status code = apkg_client_channel_refresh(
        root, endpoint, identity, 0, observer, &snapshot, &channel, error);
    if (!code)
        code = apkg_channel_summarize(snapshot, channel, &summary, error);
    if (!code && ak_cancelled(observer))
        code = APKG_CANCELLED;
    if (!code)
        code = apkg_update_summary_format(apkg_update_summary_get(summary),
                                          field, out, error);
    apkg_update_summary_free(summary);
    apkg_channel_free(channel);
    apkg_snapshot_free(snapshot);
    return code;
}
