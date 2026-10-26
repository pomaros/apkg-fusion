/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include "pkg_fs.h"
#include "pkg_target.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int same_snapshot(apkg_ctx *c, const apkg_snapshot *s) {
    return !strcmp(c->anchor, s->anchor) &&
           c->binding.st_dev == s->binding.st_dev &&
           c->binding.st_ino == s->binding.st_ino &&
           ak_physical(c->anchor, &c->binding);
}
apkg_status ak_make_plan(apkg_ctx *c, const apkg_snapshot *s,
                         const char *root_state, const char *package,
                         const char *name, apkg_plan **out, apkg_error **e) {
    apkg_plan *p = calloc(1, sizeof *p);
    char before[65], *root_hex;
    size_t cap, n;
    int r;
    if (!p)
        return APKG_NOMEM;
    cap = strlen(root_state) + strlen(c->root) * 2 + 2048;
    if (cap > c->object_max || cap > c->memory_max / 32) {
        free(p);
        return APKG_LIMIT;
    }
    p->raw = malloc(cap);
    root_hex = malloc(strlen(c->root) * 2 + 1);
    p->root_record = ak_copy(root_state, strlen(root_state));
    p->name = ak_copy(name, strlen(name));
    if (!p->raw || !root_hex || !p->root_record || !p->name) {
        free(root_hex);
        apkg_plan_free(p);
        return APKG_NOMEM;
    }
    ak_tohex((const unsigned char *)c->root, strlen(c->root), root_hex);
    ak_tohex(s->view.snapshot_digest.bytes, 32, before);
    p->absent = s->view.state == APKG_ROOT_ABSENT;
    p->view.kind = p->absent ? APKG_PLAN_INITIALIZE : APKG_PLAN_INSTALL;
    p->view.generation = s->view.generation.value;
    p->before = s->view.snapshot_digest;
    r = snprintf(
        p->raw, cap,
        "{\"body\":{\"action\":\"%s\",\"anchor_dev\":\"%"
        "llu"
        "\",\"anchor_ino\":\"%"
        "llu"
        "\",\"before\":\"%s\",\"generation\":\"%" PRIu64
        "\",\"new_state\":%s,\"root_path\":\"%s\",\"target_path\":\"%s\"},"
        "\"format\":"
        "\"apkg-json-1\",\"requires\":[],\"schema\":\"1\",\"type\":\"plan\"}",
        p->absent ? "initialize" : "install",
        (unsigned long long)c->binding.st_dev,
        (unsigned long long)c->binding.st_ino, p->absent ? "absent" : before,
        p->view.generation, root_state, root_hex, name);
    free(root_hex);
    if (r < 0 || (size_t)r >= cap) {
        apkg_plan_free(p);
        return APKG_LIMIT;
    }
    n = (size_t)r;
    p->view.canonical.data = (unsigned char *)p->raw;
    p->view.canonical.size = n;
    ak_hash(p->raw, n, &p->view.plan_digest);
    p->view.executable = 1;
    strcpy(p->root, c->root);
    strcpy(p->anchor, c->anchor);
    p->binding = c->binding;
    memcpy(p->session, c->session, 16);
    if (package) {
        p->package = ak_copy(package, strlen(package));
        if (!p->package) {
            apkg_plan_free(p);
            return APKG_NOMEM;
        }
        p->package_size = strlen(package);
    }
    {
        ak_json j = {0};
        char id[33];
        int body;
        {
            int parsed = ak_json_parse(root_state, strlen(root_state), &j);
            if (parsed) {
                apkg_plan_free(p);
                return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
            }
        }
        body = ak_member(&j, 0, "body");
        if (ak_string(&j, ak_member(&j, body, "root_id"), id, sizeof id) ||
            ak_hex(id, p->view.root_id.bytes, 16)) {
            ak_json_free(&j);
            apkg_plan_free(p);
            return APKG_INVALID;
        }
        ak_json_free(&j);
    }
    /* Effects retain complete descriptor/policy records. Destinations are
     * relative to the physically bound root, not host command strings. */
    p->view.effect_count = 2;
    if (package) {
        ak_json j = {0};
        int arr;
        size_t i;
        {
            int parsed = ak_json_parse(package, strlen(package), &j);
            if (parsed) {
                apkg_plan_free(p);
                return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
            }
        }
        arr = ak_member(&j, ak_member(&j, 0, "body"), "files");
        for (i = (size_t)arr + 1; i < j.tokens[arr].next; i = j.tokens[i].next)
            p->view.effect_count++;
        ak_json_free(&j);
    }
    p->effects = calloc((size_t)p->view.effect_count, sizeof *p->effects);
    if (!p->effects) {
        apkg_plan_free(p);
        return APKG_NOMEM;
    }
    p->effects[0].kind = p->absent ? APKG_EFFECT_TRUST : APKG_EFFECT_CREATE;
    p->effects[0].destination.data = p->absent ? ".apkg/state#policy" : p->name;
    p->effects[0].destination.size = strlen(p->effects[0].destination.data);
    p->effects[0].record.data = (unsigned char *)p->root_record;
    p->effects[0].record.size = strlen(p->root_record);
    p->effects[1].kind = APKG_EFFECT_DATABASE;
    p->effects[1].destination.data = ".apkg/state";
    p->effects[1].destination.size = 11;
    p->effects[1].record = p->effects[0].record;
    if (package) {
        ak_json j = {0};
        int arr;
        size_t i, k = 2;
        {
            int parsed = ak_json_parse(package, strlen(package), &j);
            if (parsed) {
                apkg_plan_free(p);
                return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
            }
        }
        arr = ak_member(&j, ak_member(&j, 0, "body"), "files");
        for (i = (size_t)arr + 1; i < j.tokens[arr].next;
             i = j.tokens[i].next) {
            char path[AK_PATH], *dest;
            ak_string(&j, ak_member(&j, (int)i, "path"), path, sizeof path);
            dest = malloc(strlen(name) + strlen(path) + 2);
            if (!dest) {
                ak_json_free(&j);
                apkg_plan_free(p);
                return APKG_NOMEM;
            }
            sprintf(dest, "%s/%s", name, path);
            p->effects[k].kind = APKG_EFFECT_CREATE;
            p->effects[k].destination.data = dest;
            p->effects[k].destination.size = strlen(dest);
            p->effects[k].record.data =
                (unsigned char *)p->package + j.tokens[i].start;
            p->effects[k].record.size = j.tokens[i].end - j.tokens[i].start;
            k++;
        }
        ak_json_free(&j);
    }
    *out = p;
    (void)e;
    return APKG_OK;
}
apkg_status apkg_plan_initialize(apkg_ctx *c, const apkg_snapshot *s,
                                 apkg_text target, apkg_bytes policy,
                                 apkg_plan **out, apkg_error **e) {
    ak_json j = {0};
    char t[32], id[33];
    unsigned char random[16];
    char *record;
    size_t cap;
    apkg_status r;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c || !s)
        return APKG_INVALID;
    if (!c->binding.st_ino)
        return ak_fail(e, APKG_UNSUPPORTED,
                       "filesystem does not expose stable root identity",
                       c->root);
    if (!same_snapshot(c, s))
        return APKG_STALE;
    if (s->view.state != APKG_ROOT_ABSENT)
        return s->view.state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED
                                                  : APKG_CONFLICT;
    if (!target.data || target.size >= sizeof t ||
        memchr(target.data, 0, target.size))
        return APKG_TARGET;
    memcpy(t, target.data, target.size);
    t[target.size] = 0;
    if (pkg_target_parse(t, NULL, NULL, 0))
        return APKG_TARGET;
    if (policy.size > c->object_max)
        return APKG_LIMIT;
    {
        int parsed = ak_json_parse(policy.data, policy.size, &j);
        if (parsed)
            return parsed == -2 ? APKG_NOMEM : APKG_POLICY;
    }
    if (!ak_policy_valid(&j)) {
        ak_json_free(&j);
        return ak_fail(e, APKG_POLICY, "invalid seed trust policy", "");
    }
    if (pkg_fs_random(random, 16)) {
        ak_json_free(&j);
        return APKG_IO;
    }
    ak_tohex(random, 16, id);
    cap = policy.size + 512;
    record = malloc(cap);
    if (!record) {
        ak_json_free(&j);
        return APKG_NOMEM;
    }
    snprintf(record, cap,
             "{\"body\":{\"generation\":\"1\",\"installed\":[],\"policy\":%s,"
             "\"root_id\":\"%s\","
             "\"target\":\"%s\"},\"format\":\"apkg-json-1\",\"requires\":[],"
             "\"schema\":\"1\","
             "\"type\":\"root\"}",
             j.text, id, t);
    r = ak_make_plan(c, s, record, NULL, "", out, e);
    free(record);
    ak_json_free(&j);
    return r;
}
apkg_status apkg_plan_install(apkg_ctx *c, const apkg_snapshot *s,
                              const apkg_package *p,
                              const apkg_observer *observer, apkg_plan **out,
                              apkg_error **e) {
    apkg_snapshot *current = NULL;
    ak_json j = {0}, policy = {0};
    int b, arr;
    char dest[AK_PATH], key[65], id[33];
    char *record, *entries;
    size_t entries_size, cap;
    apkg_status r;
    uint64_t gen;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c || !s || !p)
        return APKG_INVALID;
    if (!pkg_fs_atomic_replace_supported())
        return ak_fail(
            e, APKG_UNSUPPORTED,
            "atomic state replacement is unavailable on this SDK/runtime",
            c->root);
    if (ak_cancelled(observer))
        return APKG_CANCELLED;
    r = ak_package_payload_check(c, p);
    if (r)
        return r;
    if (!c->binding.st_ino)
        return ak_fail(e, APKG_UNSUPPORTED,
                       "filesystem does not expose stable root identity",
                       c->root);
    if (!same_snapshot(c, s))
        return APKG_STALE;
    if (s->view.state != APKG_ROOT_READY)
        return s->view.state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED
                                                  : APKG_POLICY;
    r = apkg_snapshot_read(c, &current, e);
    if (r)
        return r;
    if (current->view.state != APKG_ROOT_READY ||
        memcmp(current->view.snapshot_digest.bytes,
               s->view.snapshot_digest.bytes, 32)) {
        apkg_snapshot_free(current);
        return APKG_STALE;
    }
    apkg_snapshot_free(current);
    if (strcmp(s->target, p->target))
        return APKG_TARGET;
    ak_tohex(p->key, 32, key);
    {
        int parsed = ak_json_parse(s->policy, strlen(s->policy), &policy);
        if (parsed) {
            return parsed == -2 ? APKG_NOMEM : APKG_TRUST;
        }
    }
    if (!ak_policy(&policy, 0, p->authority, key)) {
        ak_json_free(&policy);
        return APKG_TRUST;
    }
    ak_json_free(&policy);
    int related =
        ak_member(&p->json, ak_member(&p->json, 0, "body"), "relations") >= 0;
    for (size_t row = 0; row < s->nrows && !related; row++) {
        ak_json entry = {0};
        if (ak_json_parse(s->records[row], strlen(s->records[row]), &entry))
            return errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
        related =
            ak_member(
                &entry,
                ak_member(&entry, ak_member(&entry, 0, "descriptor"), "body"),
                "relations") >= 0;
        ak_json_free(&entry);
    }
    if (related) {
        /* Install cannot implicitly turn into an update of the requested name.
         */
        for (size_t row = 0; row < s->nrows; row++)
            if (!strcmp(p->name, s->rows[row].name.value.data))
                return APKG_CONFLICT;
        apkg_package *request = (apkg_package *)p;
        return apkg_plan_resolve(c, s, &request, 1, NULL, 0, 0, out, e);
    }
    if (ak_path(dest, c->root, p->name) || ak_exists(dest) != 0)
        return APKG_CONFLICT;
    {
        size_t i;
        for (i = 0; i < s->nrows; i++)
            if (s->rows[i].name.value.size == strlen(p->name) &&
                !memcmp(s->rows[i].name.value.data, p->name, strlen(p->name)))
                return APKG_CONFLICT;
    }
    {
        int parsed = ak_json_parse(s->raw, s->view.source_record.size, &j);
        if (parsed)
            return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    }
    b = ak_member(&j, 0, "body");
    arr = ak_member(&j, b, "installed");
    entries_size = j.tokens[arr].end - j.tokens[arr].start - 2;
    entries = ak_copy(j.text + j.tokens[arr].start + 1, entries_size);
    if (!entries) {
        ak_json_free(&j);
        return APKG_NOMEM;
    }
    gen = s->view.generation.value;
    if (gen == UINT64_MAX) {
        free(entries);
        ak_json_free(&j);
        return APKG_LIMIT;
    }
    ak_tohex(s->view.root_id.bytes, 16, id);
    cap = entries_size + strlen(p->raw) + strlen(p->signature) +
          strlen(s->policy) + 1024;
    if (cap > c->object_max || cap > c->memory_max / 32) {
        free(entries);
        ak_json_free(&j);
        return APKG_LIMIT;
    }
    record = malloc(cap);
    if (!record) {
        free(entries);
        ak_json_free(&j);
        return APKG_NOMEM;
    }
    snprintf(record, cap,
             "{\"body\":{\"generation\":\"%" PRIu64
             "\",\"installed\":[%s%s{\"descriptor\":%s,\"signature\":%s}],"
             "\"policy\":%s,\"root_"
             "id\":\"%s\",\"target\":\"%s\"},\"format\":\"apkg-json-1\","
             "\"requires\":[],\"schema\":"
             "\"1\",\"type\":\"root\"}",
             gen + 1, entries, entries_size ? "," : "", p->raw, p->signature,
             s->policy, id, s->target);
    r = ak_make_plan(c, s, record, p->raw, p->name, out, e);
    free(record);
    free(entries);
    ak_json_free(&j);
    return r;
}
const apkg_plan_view *apkg_plan_get(const apkg_plan *p) { return &p->view; }
apkg_status apkg_plan_effect_at(const apkg_plan *p, uint64_t i,
                                const apkg_effect_view **out, apkg_error **e) {
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!p)
        return APKG_INVALID;
    if (i >= p->view.effect_count)
        return APKG_NOT_FOUND;
    *out = &p->effects[(size_t)i];
    return APKG_OK;
}
void apkg_plan_free(apkg_plan *p) {
    size_t i;
    if (!p)
        return;
    if (p->effects)
        for (i = 2; i < p->view.effect_count; i++)
            free((void *)p->effects[i].destination.data);
    free(p->effects);
    free(p->raw);
    free(p->root_record);
    free(p->package);
    free(p->name);
    free(p->changes);
    free(p->before_state);
    free(p);
}
apkg_status apkg_plan_approve(apkg_ctx *c, const apkg_plan *p,
                              const apkg_digest *digest, apkg_approval **out,
                              apkg_error **e) {
    apkg_approval *a;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c || !p || !digest)
        return APKG_INVALID;
    if (c->mode != APKG_WRITE)
        return APKG_POLICY;
    if (!p->view.executable ||
        memcmp(digest->bytes, p->view.plan_digest.bytes, 32) ||
        memcmp(c->session, p->session, 16) ||
        !ak_physical(p->anchor, &p->binding))
        return APKG_STALE;
    a = calloc(1, sizeof *a);
    if (!a)
        return APKG_NOMEM;
    memcpy(a->session, c->session, 16);
    a->digest = *digest;
    *out = a;
    return APKG_OK;
}
void apkg_approval_free(apkg_approval *a) { free(a); }
