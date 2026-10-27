/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include "pkg_target.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
/* Dependencies name an authority, target and minimum version/revision. A
 * package match does not imply that a shared library is visible to a loader. */
int ak_relations_valid(const ak_json *j, int rel) {
    const char *const fields[] = {"conflicts", "depends", "recommends",
                                  "replaces"};
    size_t k, i;
    char last[300], key[300], authority[33], name[256], version[256],
        target[32];
    unsigned char id[16];
    uint64_t revision;
    if (rel < 0)
        return 1;
    if (!ak_fields(j, rel, fields, 4))
        return 0;
    for (k = 0; k < 4; k++) {
        int a = ak_member(j, rel, fields[k]);
        last[0] = 0;
        if (a < 0 || j->tokens[a].type != '[')
            return 0;
        for (i = (size_t)a + 1; i < j->tokens[a].next; i = j->tokens[i].next) {
            const char *const f[] = {"authority", "min_revision", "min_version",
                                     "name", "target"};
            if (!ak_fields(j, (int)i, f, 5) ||
                ak_string(j, ak_member(j, (int)i, "authority"), authority,
                          sizeof authority) ||
                ak_hex(authority, id, 16) ||
                ak_string(j, ak_member(j, (int)i, "name"), name, sizeof name) ||
                !ak_valid_name(name) ||
                ak_string(j, ak_member(j, (int)i, "min_version"), version,
                          sizeof version) ||
                ak_version_compare(version, version) != 0 ||
                ak_u64(j, ak_member(j, (int)i, "min_revision"), &revision) ||
                !revision ||
                ak_string(j, ak_member(j, (int)i, "target"), target,
                          sizeof target))
                return 0;
            /* Target syntax and equality are verified against actual packages.
             */
            snprintf(key, sizeof key, "%s/%s", authority, name);
            if (*last && strcmp(last, key) >= 0)
                return 0;
            strcpy(last, key);
        }
    }
    return 1;
}
int ak_requirement_matches(const ak_json *j, int requirement,
                           const apkg_package *p) {
    char version[256];
    uint64_t revision;
    int cmp;
    if (!ak_equal(j, ak_member(j, requirement, "authority"), p->authority) ||
        !ak_equal(j, ak_member(j, requirement, "name"), p->name) ||
        !ak_equal(j, ak_member(j, requirement, "target"), p->target) ||
        ak_string(j, ak_member(j, requirement, "min_version"), version,
                  sizeof version) ||
        ak_u64(j, ak_member(j, requirement, "min_revision"), &revision))
        return 0;
    cmp = ak_version_compare(p->version, version);
    return cmp == 1 || (cmp == 0 && p->revision >= revision);
}
static int relations(const apkg_package *p, const char *family) {
    return ak_member(
        &p->json,
        ak_member(&p->json, ak_member(&p->json, 0, "body"), "relations"),
        family);
}
static int satisfies(const apkg_package *p, const char *family,
                     const apkg_package *other) {
    int a = relations(p, family);
    size_t i;
    if (a < 0)
        return 0;
    for (i = (size_t)a + 1; i < p->json.tokens[a].next;
         i = p->json.tokens[i].next)
        if (ak_requirement_matches(&p->json, (int)i, other))
            return 1;
    return 0;
}
apkg_status ak_relations_check(apkg_package *const *p, size_t n) {
    size_t i, k, x;
    for (i = 0; i < n; i++) {
        int a = relations(p[i], "depends");
        if (a >= 0)
            for (k = (size_t)a + 1; k < p[i]->json.tokens[a].next;
                 k = p[i]->json.tokens[k].next) {
                for (x = 0; x < n; x++)
                    if (ak_requirement_matches(&p[i]->json, (int)k, p[x]))
                        break;
                if (x == n)
                    return APKG_DEPENDENCY;
            }
        for (x = 0; x < n; x++)
            if (i != x && satisfies(p[i], "conflicts", p[x]))
                return APKG_CONFLICT;
    }
    return APKG_OK;
}
/* Deterministic bounded backtracking. Installed suitable versions are tried
 * first, then exact caller-order candidates; failure never mutates a root. */
typedef struct {
    apkg_package **pool;
    size_t count, installed;
    unsigned char *selected, *explicit;
    unsigned decisions;
    int recommends;
    apkg_status error;
} solver;
static int selected_ok(solver *s) {
    size_t i, x;
    for (i = 0; i < s->count; i++)
        if (s->selected[i])
            for (x = 0; x < i; x++)
                if (s->selected[x]) {
                    if (!strcmp(s->pool[i]->name, s->pool[x]->name) ||
                        satisfies(s->pool[i], "conflicts", s->pool[x]) ||
                        satisfies(s->pool[x], "conflicts", s->pool[i]))
                        return 0;
                }
    return 1;
}
static int solve(solver *s, unsigned depth) {
    size_t i, k, x;
    int family;
    if (depth > 64 || ++s->decisions > 4096) {
        s->error = APKG_LIMIT;
        return 0;
    }
    if (!selected_ok(s))
        return 0;
    for (i = 0; i < s->count; i++)
        if (s->selected[i])
            for (family = 0; family < (s->recommends ? 2 : 1); family++) {
                int a =
                    relations(s->pool[i], family ? "recommends" : "depends");
                if (a < 0)
                    continue;
                for (k = (size_t)a + 1; k < s->pool[i]->json.tokens[a].next;
                     k = s->pool[i]->json.tokens[k].next) {
                    for (x = 0; x < s->count; x++)
                        if (s->selected[x] &&
                            ak_requirement_matches(&s->pool[i]->json, (int)k,
                                                   s->pool[x]))
                            break;
                    if (x < s->count)
                        continue;
                    for (x = 0; x < s->count; x++)
                        if (!s->selected[x] &&
                            ak_requirement_matches(&s->pool[i]->json, (int)k,
                                                   s->pool[x])) {
                            unsigned char *saved =
                                malloc(s->count ? s->count : 1);
                            int allowed = 1;
                            size_t y;
                            if (!saved) {
                                s->error = APKG_NOMEM;
                                return 0;
                            }
                            memcpy(saved, s->selected, s->count);
                            for (y = 0; y < s->count; y++)
                                if (s->selected[y] &&
                                    !strcmp(s->pool[y]->name,
                                            s->pool[x]->name)) {
                                    if (s->explicit[y] ||
                                        strcmp(s->pool[y]->authority,
                                               s->pool[x]->authority)) {
                                        allowed = 0;
                                        break;
                                    }
                                    int cmp =
                                        ak_version_compare(s->pool[y]->version,
                                                           s->pool[x]->version);
                                    if (cmp != -1 &&
                                        !(cmp == 0 &&
                                          s->pool[x]->revision >
                                              s->pool[y]->revision)) {
                                        allowed = 0;
                                        break;
                                    }
                                    s->selected[y] = 0;
                                }
                            s->selected[x] = 1;
                            if (allowed && solve(s, depth + 1)) {
                                free(saved);
                                return 1;
                            }
                            memcpy(s->selected, saved, s->count);
                            free(saved);
                            if (s->error)
                                return 0;
                        }
                    if (family)
                        continue; /* Recommendations are optional, never
                                     blockers. */
                    return 0;
                }
            }
    return 1;
}
apkg_status ak_resolve_metadata(apkg_ctx *c, const apkg_snapshot *snapshot,
                                apkg_package *const *requested,
                                size_t nrequested,
                                apkg_package *const *candidates,
                                size_t ncandidates, int recommends,
                                ak_resolution *out, apkg_error **e) {
    solver s = {0};
    size_t i, k, n;
    apkg_status code = APKG_INVALID;
    ak_buf entries = {0};
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    memset(out, 0, sizeof *out);
    if (!c || !snapshot || !requested || !nrequested ||
        (ncandidates && !candidates) || nrequested > 128 || ncandidates > 128 ||
        snapshot->nrows > 128)
        return APKG_INVALID;
    n = snapshot->nrows + nrequested + ncandidates;
    s.count = n;
    s.installed = snapshot->nrows;
    s.recommends = !!recommends;
    s.pool = calloc(n, sizeof *s.pool);
    s.selected = calloc(n, 1);
    s.explicit = calloc(n, 1);
    if (!s.pool || !s.selected || !s.explicit) {
        code = APKG_NOMEM;
        goto done;
    }
    for (i = 0; i < snapshot->nrows; i++) {
        code = ak_verified_entry(c, snapshot->records[i], &s.pool[i]);
        if (code)
            goto done;
        s.selected[i] = 1;
    }
    /* Copy/reverify every candidate in this root's current policy. */
    for (i = 0; i < nrequested + ncandidates; i++) {
        apkg_package *p =
            i < nrequested ? requested[i] : candidates[i - nrequested];
        ak_buf record = {0};
        record.max = c->object_max;
        if (!p) {
            code = APKG_INVALID;
            goto done;
        }
        ak_buf_add(&record, "{\"descriptor\":%s,\"signature\":%s}", p->raw,
                   p->signature);
        code = record.bad ? (apkg_status)record.bad
                          : ak_verified_entry(c, record.data,
                                              &s.pool[s.installed + i]);
        free(record.data);
        if (code)
            goto done;
        if (p->origin) {
            s.pool[s.installed + i]->origin =
                ak_copy(p->origin, strlen(p->origin));
            if (!s.pool[s.installed + i]->origin) {
                code = APKG_NOMEM;
                goto done;
            }
        }
    }
    for (i = s.installed; i < s.installed + nrequested; i++) {
        for (k = 0; k < i; k++)
            if (s.selected[k] && !strcmp(s.pool[k]->name, s.pool[i]->name)) {
                if (s.explicit[k] ||
                    strcmp(s.pool[k]->authority, s.pool[i]->authority)) {
                    code = APKG_CONFLICT;
                    goto done;
                }
                if (strcmp(s.pool[k]->raw, s.pool[i]->raw)) {
                    int cmp = ak_version_compare(s.pool[k]->version,
                                                 s.pool[i]->version);
                    if (cmp != -1 && !(cmp == 0 && s.pool[i]->revision >
                                                       s.pool[k]->revision)) {
                        code = APKG_POLICY;
                        goto done;
                    }
                }
                s.selected[k] = 0;
            }
        s.selected[i] = s.explicit[i] = 1;
        /* Replacement authorizes removal only through an explicit requested
         * package's signed declaration, and never overrides another request. */
        for (k = 0; k < s.installed; k++)
            if (s.selected[k] && satisfies(s.pool[i], "replaces", s.pool[k]))
                s.selected[k] = 0;
    }
    if (!selected_ok(&s)) {
        code = APKG_CONFLICT;
        goto done;
    }
    if (!solve(&s, 0)) {
        code = s.error ? s.error : APKG_DEPENDENCY;
        goto done;
    }
    entries.max = c->object_max;
    ak_buf_add(&entries, "[");
    for (i = 0; i < n; i++)
        if (s.selected[i]) {
            const char *reason = "dependency";
            if (i < s.installed) {
                ak_buf_add(&entries, "%s%s", entries.size > 1 ? "," : "",
                           snapshot->records[i]);
                continue;
            }
            if (s.explicit[i])
                reason = "explicit";
            else if (s.recommends) {
                reason = "recommendation";
                for (k = 0; k < n; k++)
                    if (s.selected[k] &&
                        satisfies(s.pool[k], "depends", s.pool[i]))
                        reason = "dependency";
            }
            ak_buf_add(
                &entries,
                "%s{\"descriptor\":%s,\"reason\":\"%s\",\"signature\":%s}",
                entries.size > 1 ? "," : "", s.pool[i]->raw, reason,
                s.pool[i]->signature);
        }
    ak_buf_add(&entries, "]");
    code = entries.bad;
    if (!code) {
        out->packages = calloc(nrequested + ncandidates, sizeof *out->packages);
        if (!out->packages) {
            code = APKG_NOMEM;
            goto done;
        }
        for (i = s.installed; i < s.count; i++)
            if (s.selected[i]) {
                out->packages[out->count++] = s.pool[i];
                s.pool[i] = NULL;
            }
        out->entries = entries.data;
        entries.data = NULL;
    }
done:
    if (s.pool)
        for (i = 0; i < n; i++)
            apkg_package_free(s.pool[i]);
    free(s.pool);
    free(s.selected);
    free(s.explicit);
    free(entries.data);
    return code;
}

void ak_resolution_free(ak_resolution *r) {
    for (size_t i = 0; i < r->count; i++)
        apkg_package_free(r->packages[i]);
    free(r->packages);
    free(r->entries);
    memset(r, 0, sizeof *r);
}
apkg_status apkg_plan_resolve(apkg_ctx *c, const apkg_snapshot *snapshot,
                              apkg_package *const *requested, size_t nrequested,
                              apkg_package *const *candidates,
                              size_t ncandidates, int recommends,
                              apkg_plan **out, apkg_error **e) {
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    ak_resolution resolution = {0};
    apkg_status code =
        ak_resolve_metadata(c, snapshot, requested, nrequested, candidates,
                            ncandidates, recommends, &resolution, e);
    if (!code)
        code = ak_plan_records(c, snapshot, resolution.entries,
                               APKG_PLAN_RESOLVE, NULL, out, e);
    if (!code) {
        code = ak_plan_bind(c, *out,
                            (const apkg_package *const *)resolution.packages,
                            resolution.count);
        if (code) {
            apkg_plan_free(*out);
            *out = NULL;
        }
    }
    ak_resolution_free(&resolution);
    return code;
}
