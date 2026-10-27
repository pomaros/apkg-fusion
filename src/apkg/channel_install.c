/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
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
static int preference(const void *a, const void *b) {
    const apkg_package *x = *(apkg_package *const *)a,
                       *y = *(apkg_package *const *)b;
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
    if (!result)
        result = memcmp(x->digest.bytes, y->digest.bytes, 32);
    return result;
}
apkg_status apkg_plan_channel_install(apkg_ctx *ctx, const apkg_snapshot *s,
                                      const apkg_channel *channel,
                                      const apkg_digest *wanted,
                                      int recommendations,
                                      const apkg_observer *observer,
                                      apkg_plan **out, apkg_error **error) {
    apkg_package *pool[AK_CANDIDATES + 1] = {0}, *installed[128] = {0};
    ak_resolution resolution = {0};
    unsigned char *seen = NULL;
    char *heads = NULL;
    size_t count = 0, checks = 0;
    apkg_status code = APKG_INVALID;
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!ctx || !s || !channel || !wanted ||
        (recommendations != 0 && recommendations != 1))
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
    const apkg_channel_view *view = apkg_channel_get(channel);
    uint64_t requested = view->entry_count;
    for (uint64_t i = 0; i < view->entry_count; i++) {
        const apkg_channel_entry_view *row;
        code = apkg_channel_entry_at(channel, i, &row, error);
        if (code)
            goto done;
        if (!memcmp(row->descriptor_digest.bytes, wanted->bytes, 32)) {
            requested = i;
            break;
        }
    }
    if (requested == view->entry_count) {
        code = APKG_NOT_FOUND;
        goto done;
    }
    code =
        ak_channel_select(ctx, channel, requested, observer, &pool[0], error);
    if (code)
        goto done;
    count = 1;
    code = ak_channel_heads_add(s, &view->id, view->sequence,
                                &view->snapshot_digest, &heads);
    if (code)
        goto done;
    seen = calloc(view->entry_count ? (size_t)view->entry_count : 1, 1);
    if (!seen) {
        code = APKG_NOMEM;
        goto done;
    }
    seen[requested] = 1;
    for (size_t i = 0; i < s->nrows; i++) {
        code = ak_verified_entry(ctx, s->records[i], &installed[i]);
        if (code)
            goto done;
    }
    for (size_t visit = 0; visit < s->nrows + count; visit++) {
        apkg_package *p =
            visit < s->nrows ? installed[visit] : pool[visit - s->nrows];
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
                for (uint64_t index = 0; index < view->entry_count; index++) {
                    if (ak_cancelled(observer)) {
                        code = APKG_CANCELLED;
                        goto done;
                    }
                    if (++checks > AK_EDGE_CHECKS) {
                        code = APKG_LIMIT;
                        goto done;
                    }
                    if (seen[index])
                        continue;
                    const apkg_channel_entry_view *row;
                    code = apkg_channel_entry_at(channel, index, &row, error);
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
                    if (count == AK_CANDIDATES + 1) {
                        code = APKG_LIMIT;
                        goto done;
                    }
                    code = ak_channel_metadata(ctx, channel, index,
                                               &pool[count], error);
                    if (code)
                        goto done;
                    seen[index] = 1;
                    count++;
                }
        }
    }
    if (count > 2)
        qsort(pool + 1, count - 1, sizeof *pool, preference);
    code = ak_resolve_metadata(ctx, s, pool, 1, pool + 1, count - 1,
                               recommendations, &resolution, error);
    if (code)
        goto done;
    for (size_t i = 0; i < resolution.count; i++) {
        uint64_t index;
        for (index = 0; index < view->entry_count; index++) {
            const apkg_channel_entry_view *row;
            apkg_channel_entry_at(channel, index, &row, NULL);
            if (!memcmp(row->descriptor_digest.bytes,
                        resolution.packages[i]->digest.bytes, 32))
                break;
        }
        if (index == view->entry_count) {
            code = APKG_INTEGRITY;
            goto done;
        }
        apkg_package *actual = NULL;
        code =
            apkg_channel_fetch(ctx, channel, index, observer, &actual, error);
        apkg_package_free(actual);
        if (code)
            goto done;
    }
    if (!code)
        code =
            ak_plan_records_heads(ctx, s, resolution.entries, APKG_PLAN_RESOLVE,
                                  NULL, heads, out, error);
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
    free(seen);
    for (size_t i = 0; i < count; i++)
        apkg_package_free(pool[i]);
    for (size_t i = 0; i < 128; i++)
        apkg_package_free(installed[i]);
    ak_resolution_free(&resolution);
    if (code && error && !*error)
        ak_fail(error, code, "channel installation plan refused", "");
    return code;
}
