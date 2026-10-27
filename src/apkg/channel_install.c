/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include <stdint.h>
#include <string.h>
/* The catalogue is bounded independently from the resolver's relevant pool.
 * Explore all matching versions of dependencies, including installed packages'
 * needs, without downloading unrelated candidates or duplicating the solver. */
#define AK_CANDIDATES 128u
#define AK_EDGE_CHECKS 1000000u
static int copy_text(apkg_text text, char *out, size_t size) {
    if (text.size >= size)
        return -1;
    memcpy(out, text.data, text.size);
    out[text.size] = 0;
    return 0;
}
static int row_matches(const ak_json *j, int requirement,
                       const apkg_channel_entry_view *row) {
    char authority[33], name[256], target[32], version[256];
    if (copy_text(row->authority, authority, sizeof authority) ||
        copy_text(row->name, name, sizeof name) ||
        copy_text(row->target, target, sizeof target) ||
        copy_text(row->version, version, sizeof version))
        return -1;
    apkg_package candidate = {0};
    candidate.authority = authority;
    candidate.name = name;
    candidate.target = target;
    candidate.version = version;
    candidate.revision = row->revision;
    return ak_requirement_matches(j, requirement, &candidate);
}
/* A candidate and the channel that offers it, in source precedence order. */
typedef struct {
    apkg_package *package;
    size_t channel;
} candidate;
static int preference(const void *a, const void *b) {
    const candidate *cx = a, *cy = b;
    const apkg_package *x = cx->package, *y = cy->package;
    int result = strcmp(x->authority, y->authority);
    if (!result)
        result = strcmp(x->name, y->name);
    if (!result)
        result = strcmp(x->target, y->target);
    if (!result) {
        int version = ak_version_compare(x->version, y->version);
        result = version == 1    ? -1
                 : version == -1 ? 1
                 : version == 0  ? 0
                                 : strcmp(x->version, y->version);
    }
    if (!result)
        result = x->revision > y->revision ? -1 : x->revision < y->revision;
    if (!result) /* the earlier source wins an equal version */
        result = cx->channel < cy->channel ? -1 : cx->channel > cy->channel;
    if (!result)
        result = memcmp(x->digest.bytes, y->digest.bytes, 32);
    return result;
}
static int find_digest(const apkg_channel *channel, const apkg_digest *d,
                       uint64_t *index) {
    const apkg_channel_view *view = apkg_channel_get(channel);
    for (uint64_t i = 0; i < view->entry_count; i++) {
        const apkg_channel_entry_view *row;
        if (apkg_channel_entry_at(channel, i, &row, NULL))
            return -1;
        if (!memcmp(row->descriptor_digest.bytes, d->bytes, 32)) {
            *index = i;
            return 0;
        }
    }
    return -1;
}
apkg_status ak_plan_channels_install(apkg_ctx *ctx, const apkg_snapshot *s,
                                     const apkg_channel *const *channels,
                                     size_t nchannels, size_t requested_channel,
                                     const apkg_digest *wanted,
                                     int recommendations,
                                     const apkg_observer *observer,
                                     apkg_plan **out, apkg_error **error) {
    candidate pool[AK_CANDIDATES + 1];
    apkg_package *installed[128] = {0}, *chosen[AK_CANDIDATES + 1];
    unsigned char *seen[AK_SOURCES] = {0}, used[AK_SOURCES] = {0};
    ak_resolution resolution = {0};
    char *heads = NULL, *qualifications = NULL;
    size_t count = 0, checks = 0;
    apkg_status code = APKG_INVALID;
    memset(pool, 0, sizeof pool);
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!ctx || !s || !channels || !nchannels || nchannels > AK_SOURCES ||
        requested_channel >= nchannels || !wanted ||
        (recommendations != 0 && recommendations != 1))
        return APKG_INVALID;
    for (size_t k = 0; k < nchannels; k++)
        if (!channels[k])
            return APKG_INVALID;
    if (ctx->mode != APKG_WRITE)
        return APKG_POLICY;
    if (ak_cancelled(observer))
        return APKG_CANCELLED;
    if (s->view.state != APKG_ROOT_READY)
        return s->view.state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED
                                                  : APKG_POLICY;
    if (s->nrows > 128)
        return APKG_LIMIT;
    uint64_t requested;
    if (find_digest(channels[requested_channel], wanted, &requested)) {
        code = APKG_NOT_FOUND;
        goto done;
    }
    code = ak_channel_select(ctx, channels[requested_channel], requested,
                             observer, &pool[0].package, error);
    if (code)
        goto done;
    pool[0].channel = requested_channel;
    count = 1;
    for (size_t k = 0; k < nchannels; k++) {
        uint64_t entries = apkg_channel_get(channels[k])->entry_count;
        seen[k] = calloc(entries ? (size_t)entries : 1, 1);
        if (!seen[k]) {
            code = APKG_NOMEM;
            goto done;
        }
    }
    seen[requested_channel][requested] = 1;
    for (size_t i = 0; i < s->nrows; i++) {
        code = ak_verified_entry(ctx, s->records[i], &installed[i]);
        if (code)
            goto done;
    }
    for (size_t visit = 0; visit < s->nrows + count; visit++) {
        apkg_package *p =
            visit < s->nrows ? installed[visit] : pool[visit - s->nrows].package;
        int relations =
            ak_member(&p->json, ak_member(&p->json, 0, "body"), "relations");
        const char *families[] = {"depends", "recommends"};
        for (size_t family = 0; family < (recommendations ? 2u : 1u);
             family++) {
            int array = ak_member(&p->json, relations, families[family]);
            if (array < 0)
                continue;
            for (size_t item = (size_t)array + 1;
                 item < p->json.tokens[array].next;
                 item = p->json.tokens[item].next)
                for (size_t k = 0; k < nchannels; k++) {
                    uint64_t entries = apkg_channel_get(channels[k])->entry_count;
                    for (uint64_t index = 0; index < entries; index++) {
                        if (ak_cancelled(observer)) {
                            code = APKG_CANCELLED;
                            goto done;
                        }
                        if (++checks > AK_EDGE_CHECKS) {
                            code = APKG_LIMIT;
                            goto done;
                        }
                        if (seen[k][index])
                            continue;
                        const apkg_channel_entry_view *row;
                        code = apkg_channel_entry_at(channels[k], index, &row,
                                                     error);
                        if (code)
                            goto done;
                        if (row->author_trust != APKG_TRUST_ACCEPTED ||
                            !row->target_matches)
                            continue;
                        int matches = row_matches(&p->json, (int)item, row);
                        if (matches < 0) {
                            code = APKG_LIMIT;
                            goto done;
                        }
                        if (!matches)
                            continue;
                        seen[k][index] = 1;
                        /* The same exact package from a later source is the
                         * one already pooled from an earlier source. */
                        int duplicate = 0;
                        for (size_t x = 0; x < count && !duplicate; x++)
                            duplicate = !memcmp(pool[x].package->digest.bytes,
                                                row->descriptor_digest.bytes,
                                                32);
                        if (duplicate)
                            continue;
                        if (count == AK_CANDIDATES + 1) {
                            code = APKG_LIMIT;
                            goto done;
                        }
                        code = ak_channel_metadata(ctx, channels[k], index,
                                                   &pool[count].package, error);
                        if (code)
                            goto done;
                        pool[count++].channel = k;
                    }
                }
        }
    }
    if (count > 2)
        qsort(pool + 1, count - 1, sizeof *pool, preference);
    for (size_t i = 0; i < count; i++)
        chosen[i] = pool[i].package;
    code = ak_resolve_metadata(ctx, s, chosen, 1, chosen + 1, count - 1,
                               recommendations, &resolution, error);
    if (code)
        goto done;
    /* Candidate order sorts incomparable versions only to stay deterministic.
     * A dependency chosen over a same-identity alternative whose order is
     * indeterminate, or equal with different content from the same source or
     * from an earlier one, is a decision the resolver cannot make; source
     * order decides only in favour of the earlier source. The explicit
     * selection is the caller's own decision. */
    for (size_t i = 0; i < resolution.count; i++) {
        const apkg_package *r = resolution.packages[i];
        size_t rc = 0;
        if (!memcmp(r->digest.bytes, pool[0].package->digest.bytes, 32))
            continue;
        for (size_t k = 1; k < count; k++)
            if (!memcmp(pool[k].package->digest.bytes, r->digest.bytes, 32))
                rc = pool[k].channel;
        for (size_t k = 1; k < count; k++) {
            const apkg_package *q = pool[k].package;
            if (!memcmp(q->digest.bytes, r->digest.bytes, 32) ||
                strcmp(q->authority, r->authority) ||
                strcmp(q->name, r->name) || strcmp(q->target, r->target))
                continue;
            int order = ak_version_compare(r->version, q->version);
            if (order == 2 || (order == 0 && r->revision == q->revision &&
                               pool[k].channel <= rc)) {
                code = APKG_CONFLICT;
                ak_fail(error, code,
                        "dependency candidates have no defined order; "
                        "select one explicitly",
                        r->name);
                goto done;
            }
        }
    }
    /* Prepare every selected archive from the source that offers it. */
    for (size_t i = 0; i < resolution.count; i++) {
        size_t from = SIZE_MAX;
        for (size_t k = 0; k < count && from == SIZE_MAX; k++)
            if (!memcmp(pool[k].package->digest.bytes,
                        resolution.packages[i]->digest.bytes, 32))
                from = pool[k].channel;
        uint64_t index;
        if (from == SIZE_MAX ||
            find_digest(channels[from], &resolution.packages[i]->digest,
                        &index)) {
            code = APKG_INTEGRITY;
            goto done;
        }
        used[from] = 1;
        apkg_package *actual = NULL;
        code = apkg_channel_fetch(ctx, channels[from], index, observer,
                                  &actual, error);
        apkg_package_free(actual);
        if (code)
            goto done;
    }
    used[requested_channel] = 1;
    /* Each source that supplies a package advances its own replay floor and
     * contributes its observations, chained in source order. */
    {
        apkg_snapshot step = *s;
        for (size_t k = 0; k < nchannels; k++) {
            if (!used[k])
                continue;
            const apkg_channel_view *view = apkg_channel_get(channels[k]);
            char *next_heads = NULL, *next_records = NULL;
            code = ak_channel_heads_add(&step, &view->id, view->sequence,
                                        &view->snapshot_digest, &next_heads);
            if (!code)
                code = ak_qualification_records_add(&step, channels[k],
                                                    &next_records);
            if (code) {
                free(next_heads);
                free(next_records);
                goto done;
            }
            free(heads);
            free(qualifications);
            step.channel_heads = heads = next_heads;
            step.qualification_records = qualifications = next_records;
        }
    }
    code = ak_plan_records_heads(ctx, s, resolution.entries, APKG_PLAN_RESOLVE,
                                 NULL, heads, qualifications, NULL, NULL, out,
                                 error);
    if (!code) {
        code = ak_plan_bind(ctx, *out,
                            (const apkg_package *const *)resolution.packages,
                            resolution.count);
        if (code) {
            apkg_plan_free(*out);
            *out = NULL;
        }
    }
done:
    free(heads);
    free(qualifications);
    for (size_t k = 0; k < AK_SOURCES; k++)
        free(seen[k]);
    for (size_t i = 0; i < count; i++)
        apkg_package_free(pool[i].package);
    for (size_t i = 0; i < 128; i++)
        apkg_package_free(installed[i]);
    ak_resolution_free(&resolution);
    if (code && error && !*error)
        ak_fail(error, code, "channel installation plan refused", "");
    return code;
}
apkg_status apkg_plan_channel_install(apkg_ctx *ctx, const apkg_snapshot *s,
                                      const apkg_channel *channel,
                                      const apkg_digest *wanted,
                                      int recommendations,
                                      const apkg_observer *observer,
                                      apkg_plan **out, apkg_error **error) {
    const apkg_channel *one[] = {channel};
    return ak_plan_channels_install(ctx, s, one, 1, 0, wanted, recommendations,
                                    observer, out, error);
}
