/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include <errno.h>
#include <string.h>

static int file_at(const ak_json *j, const char *path) {
    int b = ak_member(j, 0, "body"), a = ak_member(j, b < 0 ? 0 : b, "files");
    if (a < 0)
        return -1;
    for (size_t i = a + 1; i < j->tokens[a].next; i = j->tokens[i].next)
        if (ak_equal(j, ak_member(j, i, "path"), path))
            return (int)i;
    return -1;
}
static int paths_valid(const ak_json *j, int a) {
    char last[AK_PATH] = "", path[AK_PATH];
    if (a < 0 || j->tokens[a].type != '[')
        return 0;
    for (size_t i = a + 1; i < j->tokens[a].next; i = j->tokens[i].next) {
        if (ak_string(j, i, path, sizeof path) || !ak_valid_path(path) ||
            (*last && strcmp(last, path) >= 0))
            return 0;
        strcpy(last, path);
    }
    return 1;
}
static int spec_valid(const ak_json *j, const ak_json *desc) {
    int b = ak_member(j, 0, "body"), a = ak_member(j, b, "baseline");
    char last[AK_PATH] = "", path[AK_PATH], hash[65], other[65];
    unsigned char digest[32];
    uint64_t size, n;
    if (!ak_object(j, "adoption") ||
        !ak_fields(
            j, b,
            (const char *const[]){"baseline", "reason", "record", "source"},
            4) ||
        (!ak_equal(j, ak_member(j, b, "source"), "manual") &&
         !ak_equal(j, ak_member(j, b, "source"), "arospkg-v1") &&
         !ak_equal(j, ak_member(j, b, "source"), "pkg-v1")) ||
        (!ak_equal(j, ak_member(j, b, "reason"), "explicit") &&
         !ak_equal(j, ak_member(j, b, "reason"), "dependency") &&
         !ak_equal(j, ak_member(j, b, "reason"), "unrecorded")) ||
        a < 0 || j->tokens[a].type != '[')
        return 0;
    /* Raw historical bytes are inert lowercase hex, never parsed as commands.
     */
    int r = ak_member(j, b, "record");
    if (r < 0 || j->tokens[r].type != '"')
        return 0;
    size_t len = j->tokens[r].end - j->tokens[r].start - 2;
    if (len % 2)
        return 0;
    for (size_t i = j->tokens[r].start + 1; i < j->tokens[r].end - 1; i++)
        if (!strchr("0123456789abcdef", j->text[i]))
            return 0;
    for (size_t i = a + 1; i < j->tokens[a].next; i = j->tokens[i].next) {
        if (!ak_fields(j, i, (const char *const[]){"path", "sha256", "size"},
                       3) ||
            ak_string(j, ak_member(j, i, "path"), path, sizeof path) ||
            !ak_valid_path(path) || (*last && strcmp(last, path) >= 0) ||
            ak_string(j, ak_member(j, i, "sha256"), hash, sizeof hash) ||
            ak_hex(hash, digest, 32) ||
            ak_u64(j, ak_member(j, i, "size"), &size))
            return 0;
        int f = desc ? file_at(desc, path) : -1;
        if (desc &&
            (f < 0 || !ak_equal(desc, ak_member(desc, f, "type"), "file") ||
             ak_string(desc, ak_member(desc, f, "sha256"), other,
                       sizeof other) ||
             strcmp(hash, other) ||
             ak_u64(desc, ak_member(desc, f, "size"), &n) || n != size))
            return 0;
        strcpy(last, path);
    }
    return 1;
}
int ak_adoption_valid(const ak_json *j, int t, const ak_json *desc) {
    errno = 0;
    if (!ak_fields(j, t,
                   (const char *const[]){"evidence", "preserve", "unowned"},
                   3) ||
        !paths_valid(j, ak_member(j, t, "preserve")) ||
        !paths_valid(j, ak_member(j, t, "unowned")))
        return 0;
    int e = ak_member(j, t, "evidence");
    ak_json spec = {0};
    if (e < 0 || ak_json_parse(j->text + j->tokens[e].start,
                               j->tokens[e].end - j->tokens[e].start, &spec))
        return 0;
    int valid = spec_valid(&spec, desc);
    ak_json_free(&spec);
    return valid;
}
int ak_adoption_protected(const ak_json *j, const char *path) {
    for (size_t k = 0; k < 2; k++) {
        int a = ak_member(j, 0, k ? "preserve" : "unowned");
        if (a < 0)
            continue;
        for (size_t i = a + 1; i < j->tokens[a].next; i = j->tokens[i].next)
            if (ak_equal(j, i, path))
                return 1;
    }
    return 0;
}
void ak_entry_write(ak_buf *b, const apkg_package *p, const char *reason,
                    const apkg_package *old) {
    const char *adoption = old ? old->adoption : p->adoption;
    ak_buf_add(b, "%s{", b->size > 1 ? "," : "");
    if (adoption)
        ak_buf_add(b, "\"adoption\":%s,", adoption);
    ak_buf_add(b, "\"descriptor\":%s,\"reason\":\"%s\",\"signature\":%s}",
               p->raw, reason, p->signature);
}
static apkg_status prepare_adoption(apkg_ctx *c, const apkg_snapshot *s,
                                    const apkg_package *package,
                                    apkg_bytes evidence, char **entry,
                                    char **observed) {
    ak_json spec = {0}, live = {0};
    ak_buf guards = {0}, entries = {0};
    char *tree = NULL, *unowned = NULL, *preserve = NULL, path[AK_PATH],
         root[AK_PATH];
    char reason[32];
    apkg_package adopted = {0};
    struct stat binding;
    apkg_status code = APKG_INVALID;
    *entry = NULL;
    *observed = NULL;
    if (!c || !s || !package || !evidence.data || !evidence.size)
        return APKG_INVALID;
    if (evidence.size > c->object_max || evidence.size > c->memory_max / 32)
        return APKG_LIMIT;
    if (ak_json_parse(evidence.data, evidence.size, &spec))
        return errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
    if (!spec_valid(&spec, &package->json))
        goto done;
    for (size_t i = 0; i < s->nrows; i++)
        if (!strcmp(package->name, s->rows[i].name.value.data)) {
            code = APKG_CONFLICT;
            goto done;
        }
    if (ak_path(root, c->root, package->name)) {
        code = APKG_LIMIT;
        goto done;
    }
    guards.max = entries.max =
        c->object_max < c->memory_max / 32 ? c->object_max : c->memory_max / 32;
    code = ak_tree_read(root, guards.max, &tree, &binding);
    if (code)
        goto done;
    if (!strcmp(tree, "null")) {
        code = APKG_NOT_FOUND;
        goto done;
    }
    if (ak_json_parse(tree, strlen(tree), &live)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    int b = ak_member(&spec, 0, "body"),
        baseline = ak_member(&spec, b, "baseline"),
        a = ak_member(&live, 0, "files");
    for (int pass = 0; pass < 2; pass++) {
        ak_buf list = {0};
        list.max = guards.max;
        ak_buf_add(&list, "[");
        for (size_t i = a + 1; i < live.tokens[a].next;
             i = live.tokens[i].next) {
            ak_string(&live, ak_member(&live, i, "path"), path, sizeof path);
            int owned = 0, edited = 0;
            for (size_t k = baseline + 1; k < spec.tokens[baseline].next;
                 k = spec.tokens[k].next)
                if (ak_equal(&spec, ak_member(&spec, k, "path"), path))
                    owned = 1;
            if (owned) {
                int f = file_at(&package->json, path);
                char hash[65], mode[24];
                ak_string(&package->json,
                          ak_member(&package->json, f, "sha256"), hash,
                          sizeof hash);
                ak_string(&package->json, ak_member(&package->json, f, "mode"),
                          mode, sizeof mode);
                int equal = ak_attributes_equal(&live, i, &package->json, f);
                if (equal < 0) {
                    free(list.data);
                    code = APKG_NOMEM;
                    goto done;
                }
                edited =
                    !ak_equal(&live, ak_member(&live, i, "type"), "file") ||
                    !ak_equal(&live, ak_member(&live, i, "sha256"), hash) ||
                    !ak_equal(&live, ak_member(&live, i, "mode"), mode) ||
                    !equal;
            }
            if ((!pass && !owned) || (pass && owned && edited))
                ak_buf_add(&list, "%s\"%s\"", list.size > 1 ? "," : "", path);
        }
        ak_buf_add(&list, "]");
        if (list.bad) {
            code = list.bad;
            free(list.data);
            goto done;
        }
        if (pass)
            preserve = list.data;
        else
            unowned = list.data;
    }
    ak_buf_add(&guards, "{\"evidence\":%.*s,\"preserve\":%s,\"unowned\":%s}",
               (int)evidence.size, (const char *)evidence.data, preserve,
               unowned);
    if (guards.bad) {
        code = guards.bad;
        goto done;
    }
    adopted = *package;
    adopted.adoption = guards.data;
    ak_string(&spec, ak_member(&spec, b, "reason"), reason, sizeof reason);
    ak_entry_write(&entries, &adopted, reason, NULL);
    code = entries.bad;
    if (!code) {
        *entry = entries.data;
        entries.data = NULL;
        *observed = tree;
        tree = NULL;
    }
done:
    free(tree);
    free(unowned);
    free(preserve);
    free(guards.data);
    free(entries.data);
    ak_json_free(&live);
    ak_json_free(&spec);
    return code;
}

apkg_status apkg_plan_adopt_many(apkg_ctx *c, const apkg_snapshot *s,
                                 const apkg_adoption_input *inputs,
                                 size_t count, apkg_plan **out,
                                 apkg_error **error) {
    char *trees[128] = {0};
    const apkg_package *packages[128] = {0};
    ak_buf entries = {0};
    ak_json changes = {0};
    apkg_status code = APKG_INVALID;
    size_t retained = 0;
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c || !s || !inputs || !count)
        return APKG_INVALID;
    if (count > 128)
        return APKG_LIMIT;
    entries.max =
        c->object_max < c->memory_max / 32 ? c->object_max : c->memory_max / 32;
    ak_buf_add(&entries, "[");
    for (size_t i = 0; i < s->nrows; i++)
        ak_buf_add(&entries, "%s%s", i ? "," : "", s->records[i]);
    if (entries.bad) {
        code = entries.bad;
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        char *entry = NULL;
        code = APKG_INVALID;
        packages[i] = inputs[i].package;
        if (!packages[i])
            goto done;
        for (size_t k = 0; k < i; k++)
            if (!strcmp(packages[k]->name, packages[i]->name)) {
                code = APKG_CONFLICT;
                goto done;
            }
        code = prepare_adoption(c, s, packages[i], inputs[i].evidence, &entry,
                                &trees[i]);
        if (code) {
            free(entry);
            goto done;
        }
        size_t n = strlen(trees[i]);
        if (n > entries.max - retained) {
            free(entry);
            code = APKG_LIMIT;
            goto done;
        }
        retained += n;
        ak_buf_add(&entries, "%s%s", entries.size > 1 ? "," : "", entry);
        free(entry);
        if (entries.bad) {
            code = entries.bad;
            goto done;
        }
    }
    ak_buf_add(&entries, "]");
    code = entries.bad;
    if (!code)
        code = ak_plan_records(c, s, entries.data, APKG_PLAN_ADOPT, NULL, out,
                               error);
    if (code)
        goto done;
    if (ak_json_parse((*out)->changes, strlen((*out)->changes), &changes)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    /* Classification and the approved before-trees must be one observation. */
    for (size_t k = 0; k < count; k++) {
        int found = 0;
        for (size_t i = 1; i < changes.tokens[0].next;
             i = changes.tokens[i].next)
            if (ak_equal(&changes, ak_member(&changes, i, "name"),
                         packages[k]->name)) {
                int t = ak_member(&changes, i, "before");
                found = 1;
                if (t < 0 ||
                    strlen(trees[k]) !=
                        changes.tokens[t].end - changes.tokens[t].start ||
                    memcmp(trees[k], changes.text + changes.tokens[t].start,
                           strlen(trees[k]))) {
                    code = APKG_STALE;
                    goto done;
                }
            }
        if (!found) {
            code = APKG_STALE;
            goto done;
        }
    }
    (*out)->effects[0].kind = APKG_EFFECT_OWNERSHIP;
    code = ak_plan_bind(c, *out, packages, count, error);
done:
    if (code && *out) {
        apkg_plan_free(*out);
        *out = NULL;
    }
    for (size_t i = 0; i < count && i < 128; i++)
        free(trees[i]);
    free(entries.data);
    ak_json_free(&changes);
    return code;
}
apkg_status apkg_plan_adopt(apkg_ctx *c, const apkg_snapshot *s,
                            const apkg_package *package, apkg_bytes evidence,
                            apkg_plan **out, apkg_error **error) {
    apkg_adoption_input input = {package, evidence};
    return apkg_plan_adopt_many(c, s, &input, 1, out, error);
}
