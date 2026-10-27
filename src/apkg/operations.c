/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include "pkg_fs.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
static int files(const ak_json *j) {
    int b = ak_member(j, 0, "body");
    return ak_member(j, b < 0 ? 0 : b, "files");
}
static int find_file(const ak_json *j, const char *path) {
    int a = files(j);
    size_t i;
    if (a < 0)
        return -1;
    for (i = (size_t)a + 1; i < j->tokens[a].next; i = j->tokens[i].next)
        if (ak_equal(j, ak_member(j, (int)i, "path"), path))
            return (int)i;
    return -1;
}
static int same_file(const ak_json *a, int x, const ak_json *b, int y) {
    char type[20], hash[65], mode[24];
    if (x < 0 || y < 0 ||
        ak_string(a, ak_member(a, x, "type"), type, sizeof type) ||
        !ak_equal(b, ak_member(b, y, "type"), type))
        return 0;
    if (!strcmp(type, "directory")) {
        char other[24] = "448";
        if (ak_string(a, ak_member(a, x, "mode"), mode, sizeof mode))
            strcpy(mode, "448");
        if (ak_member(b, y, "mode") >= 0)
            ak_string(b, ak_member(b, y, "mode"), other, sizeof other);
        return !strcmp(mode, other);
    }
    return !ak_string(a, ak_member(a, x, "sha256"), hash, sizeof hash) &&
           ak_equal(b, ak_member(b, y, "sha256"), hash) &&
           !ak_string(a, ak_member(a, x, "mode"), mode, sizeof mode) &&
           ak_equal(b, ak_member(b, y, "mode"), mode);
}
static int pathcmp(const void *a, const void *b) {
    return strcmp(((const ak_tree_row *)a)->path,
                  ((const ak_tree_row *)b)->path);
}
/* Three-way merge: signed old, signed new, actual tree. Unknown/edited bytes
 * survive uninstall/update; repair replaces non-config damage from signed data.
 */
static apkg_status desired(const char *observed, const ak_json *old,
                           const ak_json *nw, apkg_plan_kind kind, size_t max,
                           char **out) {
    ak_json live = {0};
    ak_tree_row *rows = NULL;
    size_t count = 0, k = 0, i;
    int arrays[2], pass, code = APKG_OK;
    ak_buf b = {0};
    char mode[24] = "448";
    *out = NULL;
    b.max = max;
    if (ak_json_parse(observed, strlen(observed), &live))
        return errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
    if (strcmp(observed, "null") &&
        ak_string(&live, ak_member(&live, 0, "mode"), mode, sizeof mode)) {
        code = APKG_INVALID;
        goto done;
    }
    arrays[0] = files(nw);
    arrays[1] = files(&live);
    for (pass = 0; pass < 2; pass++) {
        const ak_json *j = pass ? &live : nw;
        int arr = arrays[pass];
        if (arr < 0)
            continue;
        for (i = (size_t)arr + 1; i < j->tokens[arr].next;
             i = j->tokens[i].next)
            count++;
    }
    if (count > max / sizeof *rows) {
        code = APKG_LIMIT;
        goto done;
    }
    rows = calloc(count ? count : 1, sizeof *rows);
    if (!rows) {
        code = APKG_NOMEM;
        goto done;
    }
    for (pass = 0; pass < 2; pass++) {
        const ak_json *j = pass ? &live : nw;
        int arr = arrays[pass];
        if (arr < 0)
            continue;
        for (i = (size_t)arr + 1; i < j->tokens[arr].next;
             i = j->tokens[i].next) {
            char path[AK_PATH];
            int o, l, n, keep = 0;
            if (ak_string(j, ak_member(j, (int)i, "path"), path, sizeof path)) {
                code = APKG_INVALID;
                goto done;
            }
            o = find_file(old, path);
            l = find_file(&live, path);
            n = find_file(nw, path);
            if (pass && n >= 0)
                continue;
            if (!pass) {
                if (l >= 0 && o < 0 && !same_file(&live, l, nw, n)) {
                    code = APKG_CONFLICT;
                    goto done;
                }
                if (l >= 0 && o >= 0 && !same_file(&live, l, old, o) &&
                    (kind != APKG_PLAN_REPAIR ||
                     ak_equal(old, ak_member(old, o, "config"), "preserve"))) {
                    /* Type changes with edited content need an explicit
                     * takeover. */
                    char type[20];
                    ak_string(&live, ak_member(&live, l, "type"), type,
                              sizeof type);
                    if (!ak_equal(nw, ak_member(nw, n, "type"), type)) {
                        code = APKG_CONFLICT;
                        goto done;
                    }
                    keep = 1;
                }
                rows[k].token = keep ? -l - 2 : n;
            } else {
                if (o >= 0 && same_file(&live, l, old, o)) {
                    int equal = ak_attributes_equal(&live, l, old, o);
                    if (equal < 0) {
                        code = APKG_NOMEM;
                        goto done;
                    }
                    if (equal)
                        continue;
                }
                rows[k].token = -l - 2;
            }
            rows[k].path = ak_copy(path, strlen(path));
            if (!rows[k].path) {
                code = APKG_NOMEM;
                goto done;
            }
            k++;
        }
    }
    /* Add old directories needed by preserved descendants. */
    {
        int arr = files(&live);
        if (arr >= 0)
            for (i = (size_t)arr + 1; i < live.tokens[arr].next;
                 i = live.tokens[i].next) {
                char path[AK_PATH];
                size_t x, len;
                int present = 0, needed = 0;
                if (!ak_equal(&live, ak_member(&live, (int)i, "type"),
                              "directory"))
                    continue;
                ak_string(&live, ak_member(&live, (int)i, "path"), path,
                          sizeof path);
                len = strlen(path);
                for (x = 0; x < k; x++) {
                    if (!strcmp(rows[x].path, path))
                        present = 1;
                    if (!strncmp(rows[x].path, path, len) &&
                        rows[x].path[len] == '/')
                        needed = 1;
                }
                if (!present && needed) {
                    rows[k].path = ak_copy(path, strlen(path));
                    if (!rows[k].path) {
                        code = APKG_NOMEM;
                        goto done;
                    }
                    rows[k++].token = -(int)i - 2;
                }
            }
    }
    if (k > 1)
        qsort(rows, k, sizeof *rows, pathcmp);
    for (size_t x = 0; x < k; x++)
        for (size_t y = 0; y < x; y++) {
            if (!strcasecmp(rows[x].path, rows[y].path)) {
                code = APKG_CONFLICT;
                goto done;
            }
            size_t len = strlen(rows[y].path);
            if (!strncmp(rows[x].path, rows[y].path, len) &&
                rows[x].path[len] == '/') {
                int t = rows[y].token;
                const ak_json *j = t < 0 ? &live : nw;
                if (t < 0)
                    t = -t - 2;
                if (!ak_equal(j, ak_member(j, t, "type"), "directory")) {
                    code = APKG_CONFLICT;
                    goto done;
                }
            }
        }

    if (!k && kind == APKG_PLAN_REMOVE) {
        *out = ak_copy("null", 4);
        code = *out ? APKG_OK : APKG_NOMEM;
        goto done;
    }
    ak_buf_add(&b, "{\"files\":[");
    for (i = 0; i < k; i++) {
        int t = rows[i].token;
        const ak_json *j = t < 0 ? &live : nw;
        if (t < 0)
            t = -t - 2;
        uint64_t entrymode = 0700;
        if (ak_member(j, t, "mode") >= 0)
            ak_u64(j, ak_member(j, t, "mode"), &entrymode);
        char *attributes = NULL;
        code = ak_attributes_merge(
            old, find_file(old, rows[i].path), nw, find_file(nw, rows[i].path),
            &live, find_file(&live, rows[i].path), entrymode, &attributes);
        if (code)
            goto done;
        ak_buf_add(&b, "%s{", i ? "," : "");
        if (attributes)
            ak_buf_add(&b, "\"amiga\":%s,", attributes);
        free(attributes);
        if (ak_equal(j, ak_member(j, t, "type"), "directory")) {
            char dm[24] = "448";
            int mt = ak_member(j, t, "mode");
            if (mt >= 0)
                ak_string(j, mt, dm, sizeof dm);
            ak_buf_add(
                &b, "\"mode\":\"%s\",\"path\":\"%s\",\"type\":\"directory\"}",
                dm, rows[i].path);
        } else {
            /* Configuration policy belongs to signed metadata, not observed
             * bytes. */
            char sha[65], sz[24], fm[24];
            ak_string(j, ak_member(j, t, "sha256"), sha, sizeof sha);
            ak_string(j, ak_member(j, t, "size"), sz, sizeof sz);
            ak_string(j, ak_member(j, t, "mode"), fm, sizeof fm);
            int data = ak_member(j, t, "data");
            if (data >= 0) {
                char *bytes = ak_slice(j, data);
                if (!bytes) {
                    code = APKG_NOMEM;
                    goto done;
                }
                ak_buf_add(
                    &b,
                    "\"data\":%s,\"mode\":\"%s\",\"path\":\"%s\",\"sha256\":"
                    "\"%s\",\"size\":\"%s\",\"type\":\"file\"}",
                    bytes, fm, rows[i].path, sha, sz);
                free(bytes);
            } else
                ak_buf_add(
                    &b,
                    "\"blob\":\"%s\",\"mode\":\"%s\",\"path\":\"%s\","
                    "\"sha256\":\"%s\",\"size\":\"%s\",\"type\":\"file\"}",
                    sha, fm, rows[i].path, sha, sz);
        }
    }
    ak_buf_add(&b, "],\"mode\":\"%s\"}", mode);
    code = b.bad;
    if (!code) {
        *out = b.data;
        b.data = NULL;
    }
done:
    if (rows)
        for (i = 0; i < count; i++)
            free(rows[i].path);
    free(rows);
    free(b.data);
    ak_json_free(&live);
    return code;
}
apkg_status ak_verified_entry(apkg_ctx *c, const char *entry,
                              apkg_package **out) {
    ak_json j = {0};
    char *d = NULL, *s = NULL;
    apkg_status code;
    if (ak_json_parse(entry, strlen(entry), &j))
        return errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
    d = ak_slice(&j, ak_member(&j, 0, "descriptor"));
    s = ak_slice(&j, ak_member(&j, 0, "signature"));
    if (!d || !s)
        code = APKG_NOMEM;
    else {
        apkg_bytes db = {(unsigned char *)d, strlen(d)},
                   sb = {(unsigned char *)s, strlen(s)};
        code = apkg_package_read(c, db, sb, out, NULL);
    }
    free(d);
    free(s);
    ak_json_free(&j);
    return code;
}
/* Candidate records are signed entries. This is also the multi-package planner
 * used by dependency resolution: one generation, one approval, one journal. */
apkg_status ak_plan_records_heads(apkg_ctx *c, const apkg_snapshot *s,
                                  const char *entries, apkg_plan_kind kind,
                                  const char *focus, const char *heads,
                                  apkg_plan **out, apkg_error **e) {
    apkg_snapshot *current = NULL;
    ak_json list = {0};
    ak_buf state = {0}, changes = {0}, raw = {0};
    char id[33], roothex[AK_PATH * 2 + 1], before[65];
    apkg_plan *p = NULL;
    apkg_package **packages = NULL;
    size_t n = 0, i, k;
    apkg_status code = APKG_INVALID;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c || !s || !entries)
        return APKG_INVALID;
    if (!pkg_fs_atomic_replace_supported())
        return APKG_UNSUPPORTED;
    if (s->view.state != APKG_ROOT_READY)
        return s->view.state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED
                                                  : APKG_POLICY;
    if (!c->binding.st_ino || strcmp(c->anchor, s->anchor) ||
        c->binding.st_dev != s->binding.st_dev ||
        c->binding.st_ino != s->binding.st_ino ||
        !ak_physical(c->anchor, &c->binding))
        return APKG_STALE;
    code = apkg_snapshot_read(c, &current, e);
    if (code)
        return code;
    if (current->view.state != APKG_ROOT_READY ||
        memcmp(current->view.snapshot_digest.bytes,
               s->view.snapshot_digest.bytes, 32))
        code = APKG_STALE;
    apkg_snapshot_free(current);
    if (code)
        return code;
    if (s->view.generation.value == UINT64_MAX)
        return APKG_LIMIT;
    if (strlen(entries) > c->object_max || strlen(entries) > c->memory_max / 32)
        return APKG_LIMIT;
    if (ak_json_parse(entries, strlen(entries), &list) ||
        list.tokens[0].type != '[') {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    for (i = 1; i < list.tokens[0].next; i = list.tokens[i].next)
        n++;
    packages = calloc(n ? n : 1, sizeof *packages);
    if (!packages) {
        code = APKG_NOMEM;
        goto done;
    }
    k = 0;
    for (i = 1; i < list.tokens[0].next; i = list.tokens[i].next) {
        char *entry = ak_slice(&list, (int)i);
        if (!entry) {
            code = APKG_NOMEM;
            goto done;
        }
        code = ak_verified_entry(c, entry, &packages[k]);
        free(entry);
        if (code)
            goto done;
        for (size_t x = 0; x < k; x++)
            if (!strcmp(packages[x]->name, packages[k]->name)) {
                code = APKG_CONFLICT;
                goto done;
            }
        k++;
    }
    code = ak_relations_check(packages, n);
    if (code)
        goto done;
    state.max = changes.max = raw.max =
        c->object_max < c->memory_max / 32 ? c->object_max : c->memory_max / 32;
    ak_tohex(s->view.root_id.bytes, 16, id);
    ak_buf_add(&state,
               "{\"body\":{\"channel_heads\":%s,\"generation\":\"%" PRIu64
               "\",\"installed\":%s,\"package_generation\":\"%" PRIu64
               "\",\"policy\":%s,\"root_id\":\"%s\",\"target\":"
               "\"%s\"},\"format\":\"apkg-json-1\",\"requires\":[],\"schema\":"
               "\"1\",\"type\":\"root\"}",
               heads, s->view.generation.value + 1, entries,
               s->package_generation + (kind != APKG_PLAN_CHANNEL_ACCEPT),
               s->policy, id, s->target);
    ak_buf_add(&changes, "[");
    k = 0;
    /* Existing entries first, followed by newly selected packages. */
    for (i = 0; i < s->nrows + n; i++) {
        const char *name;
        const apkg_package *nw = NULL;
        apkg_package *old = NULL;
        char *live = NULL, *next = NULL;
        char path[AK_PATH];
        struct stat binding = {0};
        size_t x;
        if (i < s->nrows) {
            name = s->rows[i].name.value.data;
            for (x = 0; x < n; x++)
                if (!strcmp(packages[x]->name, name))
                    nw = packages[x];
            if (nw && !strcmp(s->records[i], "")) {
                code = APKG_INVALID;
                goto done;
            }
            /* Unchanged signed entry needs no payload operation, except repair.
             */
            if (nw &&
                (kind != APKG_PLAN_REPAIR || (focus && strcmp(name, focus)))) {
                ak_json oldentry = {0};
                char *d, *sg;
                int identical = 0;
                if (ak_json_parse(s->records[i], strlen(s->records[i]),
                                  &oldentry)) {
                    code = APKG_INVALID;
                    goto done;
                }
                d = ak_slice(&oldentry, ak_member(&oldentry, 0, "descriptor"));
                sg = ak_slice(&oldentry, ak_member(&oldentry, 0, "signature"));
                if (d && sg)
                    identical =
                        !strcmp(d, nw->raw) && !strcmp(sg, nw->signature);
                free(d);
                free(sg);
                ak_json_free(&oldentry);
                if (identical)
                    continue;
            }
            code = ak_verified_entry(c, s->records[i], &old);
            if (code)
                goto done;
            if (nw && strcmp(old->authority, nw->authority)) {
                apkg_package_free(old);
                code = APKG_TRUST;
                goto done;
            }
        } else {
            nw = packages[i - s->nrows];
            name = nw->name;
            for (x = 0; x < s->nrows; x++)
                if (!strcmp(s->rows[x].name.value.data, name))
                    break;
            if (x < s->nrows)
                continue;
        }
        if (nw) {
            code = ak_package_payload_check(c, nw);
            if (code) {
                apkg_package_free(old);
                goto done;
            }
        }
        if (ak_path(path, c->root, name)) {
            apkg_package_free(old);
            code = APKG_LIMIT;
            goto done;
        }
        code = ak_tree_read(path, state.max, &live, &binding);
        if (code) {
            apkg_package_free(old);
            goto done;
        }
        if (!old && strcmp(live, "null")) {
            free(live);
            apkg_package_free(old);
            code = APKG_CONFLICT;
            goto done;
        }
        {
            ak_json empty = {0};
            code = desired(live, old ? &old->json : &empty,
                           nw ? &nw->json : &empty,
                           nw ? kind : APKG_PLAN_REMOVE, state.max, &next);
        }
        if (!code)
            ak_buf_add(
                &changes,
                "%s{\"before\":%s,\"dev\":\"%llu\",\"ino\":\"%llu\",\"name\":"
                "\"%s\",\"next\":%s}",
                k++ ? "," : "", live, (unsigned long long)binding.st_dev,
                (unsigned long long)binding.st_ino, name, next);
        free(live);
        free(next);
        apkg_package_free(old);
        if (code)
            goto done;
    }
    ak_buf_add(&changes, "]");
    if (state.bad || changes.bad) {
        code = state.bad ? state.bad : changes.bad;
        goto done;
    }
    code = ak_make_plan(c, s, state.data, NULL, "", &p, e);
    if (code)
        goto done;
    p->before_state = ak_copy(s->raw, s->view.source_record.size);
    if (!p->before_state) {
        code = APKG_NOMEM;
        goto done;
    }
    p->view.kind = kind;
    p->changes = changes.data;
    changes.data = NULL;
    ak_tohex((unsigned char *)c->root, strlen(c->root), roothex);
    ak_tohex(s->view.snapshot_digest.bytes, 32, before);
    ak_buf_add(
        &raw,
        "{\"body\":{\"action\":\"changes\",\"anchor_dev\":\"%llu\",\"anchor_"
        "ino\":\"%llu\",\"before\":\"%s\",\"changes\":%s,\"generation\":"
        "\"%" PRIu64
        "\",\"kind\":\"%u\",\"new_state\":%s,\"root_path\":\"%s\"},\"format\":"
        "\"apkg-json-1\",\"requires\":[],\"schema\":\"1\",\"type\":\"plan\"}",
        (unsigned long long)c->binding.st_dev,
        (unsigned long long)c->binding.st_ino, before, p->changes,
        p->view.generation, (unsigned)kind, p->root_record, roothex);
    if (raw.bad) {
        code = raw.bad;
        goto done;
    }
    free(p->raw);
    p->raw = raw.data;
    raw.data = NULL;
    p->view.canonical.data = (unsigned char *)p->raw;
    p->view.canonical.size = strlen(p->raw);
    ak_hash(p->raw, p->view.canonical.size, &p->view.plan_digest);
    p->effects[0].kind = APKG_EFFECT_REPLACE;
    p->effects[0].destination.data =
        "Package tree changes (exact before/after records)";
    p->effects[0].destination.size = strlen(p->effects[0].destination.data);
    p->effects[0].record.data = (unsigned char *)p->changes;
    p->effects[0].record.size = strlen(p->changes);
    *out = p;
    p = NULL;
    code = APKG_OK;
done:
    for (i = 0; i < n; i++)
        if (packages)
            apkg_package_free(packages[i]);
    free(packages);
    ak_json_free(&list);
    free(state.data);
    free(changes.data);
    free(raw.data);
    apkg_plan_free(p);
    return code;
}
apkg_status ak_plan_records(apkg_ctx *c, const apkg_snapshot *s,
                            const char *entries, apkg_plan_kind kind,
                            const char *focus, apkg_plan **out,
                            apkg_error **e) {
    return ak_plan_records_heads(c, s, entries, kind, focus,
                                 s ? s->channel_heads : NULL, out, e);
}
apkg_status ak_plan_heads(apkg_ctx *c, const apkg_snapshot *s,
                          const char *heads, apkg_plan **out, apkg_error **e) {
    ak_json j = {0};
    int parsed = ak_json_parse(s->raw, s->view.source_record.size, &j);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    char *entries =
        ak_slice(&j, ak_member(&j, ak_member(&j, 0, "body"), "installed"));
    ak_json_free(&j);
    if (!entries)
        return APKG_NOMEM;
    apkg_status code = ak_plan_records_heads(
        c, s, entries, APKG_PLAN_CHANNEL_ACCEPT, NULL, heads, out, e);
    free(entries);
    if (!code) {
        apkg_plan *p = *out;
        p->effects[0].kind = APKG_EFFECT_DATABASE;
        p->effects[0].destination.data = ".apkg/state#channel_heads";
        p->effects[0].destination.size = strlen(p->effects[0].destination.data);
        p->effects[0].record.data = (unsigned char *)p->root_record;
        p->effects[0].record.size = strlen(p->root_record);
    }
    return code;
}
static apkg_status one(apkg_ctx *c, const apkg_snapshot *s, apkg_text name,
                       const apkg_package *nw, apkg_plan_kind kind,
                       apkg_plan **out, apkg_error **e) {
    ak_buf entries = {0};
    size_t i, found = 0;
    apkg_status code;
    char key[256];
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c || !s || !name.data || !name.size || name.size >= sizeof key ||
        memchr(name.data, 0, name.size))
        return APKG_INVALID;
    memcpy(key, name.data, name.size);
    key[name.size] = 0;
    if (!ak_valid_name(key))
        return APKG_INVALID;
    size_t matches = 0;
    for (i = 0; i < s->nrows; i++)
        if (!strcmp(key, s->rows[i].name.value.data) && ++matches > 1)
            return APKG_CONFLICT;
    entries.max = c->object_max;
    ak_buf_add(&entries, "[");
    for (i = 0; i < s->nrows; i++) {
        if (!strcmp(key, s->rows[i].name.value.data)) {
            apkg_package *old = NULL;
            found = 1;
            if (nw) {
                code = ak_verified_entry(c, s->records[i], &old);
                if (code) {
                    free(entries.data);
                    return code;
                }
                int cmp = ak_version_compare(old->version, nw->version);
                int newer =
                    cmp == -1 || (cmp == 0 && nw->revision > old->revision);
                apkg_package_free(old);
                if (!newer) {
                    free(entries.data);
                    return APKG_POLICY;
                }
                ak_buf_add(
                    &entries,
                    "%s{\"descriptor\":%s,\"reason\":\"%s\",\"signature\":%s}",
                    entries.size > 1 ? "," : "", nw->raw,
                    s->rows[i].reason == APKG_REASON_DEPENDENCY ? "dependency"
                    : s->rows[i].reason == APKG_REASON_RECOMMENDATION
                        ? "recommendation"
                        : "explicit",
                    nw->signature);
            } else if (kind == APKG_PLAN_REPAIR)
                ak_buf_add(&entries, "%s%s", entries.size > 1 ? "," : "",
                           s->records[i]);
        } else
            ak_buf_add(&entries, "%s%s", entries.size > 1 ? "," : "",
                       s->records[i]);
    }
    ak_buf_add(&entries, "]");
    if (!found)
        code = APKG_NOT_FOUND;
    else if (entries.bad)
        code = entries.bad;
    else
        code = ak_plan_records(c, s, entries.data, kind, key, out, e);
    free(entries.data);
    if (!code && nw) {
        code = ak_plan_bind(c, *out, &nw, 1);
        if (code) {
            apkg_plan_free(*out);
            *out = NULL;
        }
    }
    return code;
}
apkg_status apkg_plan_remove(apkg_ctx *c, const apkg_snapshot *s,
                             apkg_text name, apkg_plan **out, apkg_error **e) {
    return one(c, s, name, NULL, APKG_PLAN_REMOVE, out, e);
}
apkg_status apkg_plan_repair(apkg_ctx *c, const apkg_snapshot *s,
                             apkg_text name, apkg_plan **out, apkg_error **e) {
    return one(c, s, name, NULL, APKG_PLAN_REPAIR, out, e);
}
apkg_status apkg_plan_upgrade(apkg_ctx *c, const apkg_snapshot *s,
                              const apkg_package *p, apkg_plan **out,
                              apkg_error **e) {
    if (!p) {
        if (out)
            *out = NULL;
        return APKG_INVALID;
    }
    apkg_text n = {p->name, strlen(p->name)};
    return one(c, s, n, p, APKG_PLAN_UPGRADE, out, e);
}
/* Rollback consumes an immutable local archive, never a remote version hint. */
static apkg_status rollback_applicable(const apkg_snapshot *s,
                                       const char *after, size_t size) {
    ak_json j = {0}, current = {0};
    int parsed = ak_json_parse(after, size, &j);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    apkg_status code = APKG_STALE;
    int b = ak_member(&j, 0, "body");
    uint64_t generation, package_generation;
    if (!ak_object(&j, "root") ||
        !ak_fields_optional(
            &j, b,
            (const char *const[]){"generation", "installed", "policy",
                                  "root_id", "target"},
            5, (const char *const[]){"channel_heads", "package_generation"},
            2) ||
        ak_u64(&j, ak_member(&j, b, "generation"), &generation) ||
        !generation || generation > s->view.generation.value ||
        !ak_channel_heads_valid(&j, ak_member(&j, b, "channel_heads")))
        goto done;
    package_generation = generation;
    int pg = ak_member(&j, b, "package_generation");
    if (pg >= 0 && (ak_u64(&j, pg, &package_generation) ||
                    !package_generation || package_generation > generation))
        goto done;
    if (package_generation != s->package_generation)
        goto done;
    parsed = ak_json_parse(s->raw, s->view.source_record.size, &current);
    if (parsed) {
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    int cb = ak_member(&current, 0, "body");
    const char *fields[] = {"installed", "policy", "root_id", "target"};
    for (size_t k = 0; k < 4; k++) {
        int a = ak_member(&j, b, fields[k]),
            x = ak_member(&current, cb, fields[k]);
        size_t n = j.tokens[a].end - j.tokens[a].start;
        if (n != current.tokens[x].end - current.tokens[x].start ||
            memcmp(j.text + j.tokens[a].start,
                   current.text + current.tokens[x].start, n))
            goto done;
    }
    code = APKG_OK;
done:
    ak_json_free(&j);
    ak_json_free(&current);
    return code;
}
apkg_status apkg_plan_rollback(apkg_ctx *c, const apkg_snapshot *s,
                               apkg_text history, apkg_plan **out,
                               apkg_error **e) {
    char path[AK_PATH], prefix[AK_PATH], file[AK_PATH], sha[65], before[65];
    unsigned char *intent = NULL, *digest = NULL, *old = NULL, *after = NULL;
    size_t in = 0, dn = 0, on = 0, an = 0;
    ak_json plan = {0}, root = {0};
    apkg_plan *p = NULL;
    apkg_digest h;
    apkg_status code = APKG_INVALID;
    char *entries = NULL;
    ak_buf changes = {0}, raw = {0};
    size_t i, index = 0;
    int b, arr;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c || !s || !history.data || !history.size ||
        history.size >= sizeof path || memchr(history.data, 0, history.size))
        return APKG_INVALID;
    memcpy(path, history.data, history.size);
    path[history.size] = 0;
    {
        char canonical[AK_PATH];
        if (pkg_fs_path_is_link(path) != 0 ||
            pkg_fs_canonical_dir(path, canonical, sizeof canonical))
            return APKG_INVALID;
        strcpy(path, canonical);
    }
    if (snprintf(prefix, sizeof prefix, "%s.apkg-history.", c->root) >=
        (int)sizeof prefix)
        return APKG_LIMIT;
    if (strncmp(path, prefix, strlen(prefix)) ||
        strlen(path) != strlen(prefix) + 32 ||
        ak_hex(path + strlen(prefix), h.bytes, 16) ||
        pkg_fs_path_is_link(path) != 0)
        return APKG_INVALID;
    size_t max =
        c->object_max < c->memory_max / 32 ? c->object_max : c->memory_max / 32;
    changes.max = raw.max = max;
    if (ak_path(file, path, "plan") || ak_read(file, &intent, &in, max) ||
        ak_path(file, path, "plan-sha") || ak_read(file, &digest, &dn, 64) ||
        dn != 64 || ak_path(file, path, "old-state") ||
        ak_read(file, &old, &on, max) || ak_path(file, path, "new-state") ||
        ak_read(file, &after, &an, max) || ak_path(file, path, "decision") ||
        ak_exists(file) != 1) {
        code = APKG_RECOVERY_REQUIRED;
        goto done;
    }
    code = rollback_applicable(s, (char *)after, an);
    if (code)
        goto done;
    ak_hash(intent, in, &h);
    ak_tohex(h.bytes, 32, sha);
    if (memcmp(digest, sha, 64)) {
        code = APKG_INTEGRITY;
        goto done;
    }
    if (ak_json_parse(intent, in, &plan) || !ak_object(&plan, "plan") ||
        ak_json_parse(old, on, &root) || !ak_object(&root, "root")) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    b = ak_member(&plan, 0, "body");
    uint64_t historical_kind;
    int kind_token = ak_member(&plan, b, "kind");
    if (kind_token >= 0 && !ak_u64(&plan, kind_token, &historical_kind) &&
        historical_kind == APKG_PLAN_CHANNEL_ACCEPT) {
        code = APKG_POLICY;
        goto done;
    }
    int recorded_after = ak_member(&plan, b, "new_state");
    if (recorded_after < 0 ||
        plan.tokens[recorded_after].end - plan.tokens[recorded_after].start !=
            an ||
        memcmp(plan.text + plan.tokens[recorded_after].start, after, an)) {
        code = APKG_INTEGRITY;
        goto done;
    }
    ak_hash(old, on, &h);
    ak_tohex(h.bytes, 32, sha);
    if (ak_string(&plan, ak_member(&plan, b, "before"), before,
                  sizeof before) ||
        strcmp(sha, before)) {
        code = APKG_INTEGRITY;
        goto done;
    }
    {
        char id[33], target[32], *policy = NULL;
        int rb = ak_member(&root, 0, "body");
        ak_tohex(s->view.root_id.bytes, 16, id);
        policy = ak_slice(&root, ak_member(&root, rb, "policy"));
        if (!policy) {
            code = APKG_NOMEM;
            goto done;
        }
        int bad = !ak_equal(&root, ak_member(&root, rb, "root_id"), id) ||
                  ak_string(&root, ak_member(&root, rb, "target"), target,
                            sizeof target) ||
                  strcmp(target, s->target) || strcmp(policy, s->policy);
        free(policy);
        if (bad) {
            code = APKG_POLICY;
            goto done;
        }
        entries = ak_slice(&root, ak_member(&root, rb, "installed"));
    }
    if (!entries) {
        code = APKG_NOMEM;
        goto done;
    }
    code = ak_plan_records(c, s, entries, APKG_PLAN_ROLLBACK, NULL, &p, e);
    if (code)
        goto done;
    arr = ak_member(&plan, b, "changes");
    if (arr >= 0) {
        ak_buf_add(&changes, "[");
        for (i = (size_t)arr + 1; i < plan.tokens[arr].next;
             i = plan.tokens[i].next, index++) {
            char name[256], livepath[AK_PATH], backup[AK_PATH], leaf[64],
                *live = NULL, *was = NULL, *next = NULL, *desired_tree = NULL;
            ak_json wasj = {0}, nextj = {0};
            struct stat binding = {0};
            if (ak_string(&plan, ak_member(&plan, (int)i, "name"), name,
                          sizeof name) ||
                !ak_valid_name(name) || ak_path(livepath, c->root, name)) {
                code = APKG_INVALID;
                goto done;
            }
            was = ak_slice(&plan, ak_member(&plan, (int)i, "before"));
            next = ak_slice(&plan, ak_member(&plan, (int)i, "next"));
            if (!was || !next) {
                code = APKG_NOMEM;
                free(was);
                free(next);
                goto done;
            }
            /* Old payloads must still be retained and match the approved
             * archive. */
            if (strcmp(was, "null")) {
                char *retained = NULL;
                struct stat held;
                snprintf(leaf, sizeof leaf, "old-%zu", index);
                if (ak_path(backup, path, leaf)) {
                    code = APKG_LIMIT;
                } else
                    code = ak_tree_read(backup, max, &retained, &held);
                if (!code && strcmp(retained, was))
                    code = APKG_INTEGRITY;
                free(retained);
                if (code) {
                    free(was);
                    free(next);
                    goto done;
                }
            }
            code = ak_tree_read(livepath, max, &live, &binding);
            if (!code && (ak_json_parse(was, strlen(was), &wasj) ||
                          ak_json_parse(next, strlen(next), &nextj)))
                code = errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
            if (!code)
                code = desired(live, &nextj, &wasj,
                               !strcmp(was, "null") ? APKG_PLAN_REMOVE
                                                    : APKG_PLAN_ROLLBACK,
                               max, &desired_tree);
            if (!code)
                ak_buf_add(
                    &changes,
                    "%s{\"before\":%s,\"dev\":\"%llu\",\"ino\":\"%llu\","
                    "\"name\":\"%s\",\"next\":%s}",
                    index ? "," : "", live, (unsigned long long)binding.st_dev,
                    (unsigned long long)binding.st_ino, name, desired_tree);
            free(live);
            free(was);
            free(next);
            free(desired_tree);
            ak_json_free(&wasj);
            ak_json_free(&nextj);
            if (code)
                goto done;
        }
        ak_buf_add(&changes, "]");
        if (changes.bad) {
            code = changes.bad;
            goto done;
        }
        /* Replace only the changes token, retaining all other canonical bytes.
         */
        ak_json generated = {0};
        if (ak_json_parse(p->raw, p->view.canonical.size, &generated)) {
            code = APKG_NOMEM;
            goto done;
        }
        int ct =
            ak_member(&generated, ak_member(&generated, 0, "body"), "changes");
        ak_buf_add(&raw, "%.*s%s%s", (int)generated.tokens[ct].start, p->raw,
                   changes.data, p->raw + generated.tokens[ct].end);
        ak_json_free(&generated);
        if (raw.bad) {
            code = raw.bad;
            goto done;
        }
        free(p->raw);
        p->raw = raw.data;
        raw.data = NULL;
        free(p->changes);
        p->changes = changes.data;
        changes.data = NULL;
        p->view.canonical.data = (unsigned char *)p->raw;
        p->view.canonical.size = strlen(p->raw);
        ak_hash(p->raw, p->view.canonical.size, &p->view.plan_digest);
        p->effects[0].record.data = (unsigned char *)p->changes;
        p->effects[0].record.size = strlen(p->changes);
    } else if (!ak_equal(&plan, ak_member(&plan, b, "action"), "install")) {
        code = APKG_UNSUPPORTED;
        goto done;
    }
    *out = p;
    p = NULL;
    code = APKG_OK;
done:
    free(intent);
    free(digest);
    free(old);
    free(after);
    free(entries);
    free(changes.data);
    free(raw.data);
    ak_json_free(&plan);
    ak_json_free(&root);
    apkg_plan_free(p);
    return code;
}
