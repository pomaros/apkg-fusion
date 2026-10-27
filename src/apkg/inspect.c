/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include "pkg_target.h"
#include <inttypes.h>
#include <string.h>

struct apkg_inspection {
    apkg_inspection_view view;
    ak_json record;
    char *strings[5];
};
static const char empty_relations[] =
    "{\"conflicts\":[],\"depends\":[],\"recommends\":[],\"replaces\":[]}";
static apkg_bytes slice(const ak_json *j, int token) {
    apkg_bytes out = {(const unsigned char *)j->text + j->tokens[token].start,
                      j->tokens[token].end - j->tokens[token].start};
    return out;
}
void apkg_inspection_free(apkg_inspection *info) {
    if (!info)
        return;
    for (size_t i = 0; i < 5; i++)
        free(info->strings[i]);
    ak_json_free(&info->record);
    free(info);
}
const apkg_inspection_view *apkg_inspection_get(const apkg_inspection *info) {
    return info ? &info->view : NULL;
}
apkg_status apkg_snapshot_inspect(const apkg_snapshot *snapshot, uint64_t index,
                                  apkg_inspection **out, apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!snapshot)
        return APKG_INVALID;
    if ((snapshot->view.state != APKG_ROOT_READY &&
         snapshot->view.state != APKG_ROOT_PENDING) ||
        !snapshot->policy || !snapshot->target)
        return ak_fail(error, APKG_POLICY,
                       "inspection requires a common committed snapshot", "");
    if (index >= snapshot->nrows)
        return APKG_NOT_FOUND;
    apkg_inspection *info = calloc(1, sizeof *info);
    if (!info)
        return APKG_NOMEM;
    ak_json policy = {0};
    apkg_status code = APKG_INVALID;
    const apkg_installed_view *row = &snapshot->rows[index];
    int parsed =
        ak_json_parse(row->record.data, row->record.size, &info->record);
    if (parsed) {
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    int descriptor = ak_member(&info->record, 0, "descriptor");
    int signature = ak_member(&info->record, 0, "signature");
    if (descriptor < 0 || signature < 0)
        goto done;
    int body = ak_member(&info->record, descriptor, "body");
    if (body < 0)
        goto done;
    apkg_inspection_view *v = &info->view;
    const char *names[] = {"authority", "name", "version", "target", "kind"};
    apkg_text *values[] = {&v->authority, &v->name, &v->version, &v->target,
                           &v->kind};
    for (size_t i = 0; i < 5; i++) {
        int token = ak_member(&info->record, body, names[i]);
        if (token < 0)
            goto done;
        size_t size = info->record.tokens[token].end -
                      info->record.tokens[token].start + 1;
        info->strings[i] = malloc(size);
        if (!info->strings[i]) {
            code = APKG_NOMEM;
            goto done;
        }
        if (ak_string(&info->record, token, info->strings[i], size))
            goto done;
        *values[i] = (apkg_text){info->strings[i], strlen(info->strings[i])};
    }
    if (ak_u64(&info->record, ak_member(&info->record, body, "revision"),
               &v->revision))
        goto done;
    int files = ak_member(&info->record, body, "files");
    if (files < 0)
        goto done;
    for (size_t i = (size_t)files + 1; i < info->record.tokens[files].next;
         i = info->record.tokens[i].next) {
        if (ak_equal(&info->record, ak_member(&info->record, (int)i, "type"),
                     "directory")) {
            v->directory_count++;
        } else {
            uint64_t size;
            if (ak_u64(&info->record, ak_member(&info->record, (int)i, "size"),
                       &size))
                goto done;
            if (size > UINT64_MAX - v->payload_bytes) {
                code = APKG_LIMIT;
                goto done;
            }
            v->payload_bytes += size;
            v->file_count++;
        }
    }
    v->descriptor = slice(&info->record, descriptor);
    v->signature_envelope = slice(&info->record, signature);
    int relations = ak_member(&info->record, body, "relations");
    v->relations = relations < 0
                       ? (apkg_bytes){(const unsigned char *)empty_relations,
                                      sizeof empty_relations - 1}
                       : slice(&info->record, relations);
    unsigned char key[32];
    apkg_digest digest;
    code = ak_signature_check(v->descriptor, v->signature_envelope,
                              "descriptor", key, &digest);
    if (code == APKG_NOMEM)
        goto done;
    v->signature =
        code == APKG_OK ? APKG_SIGNATURE_VALID : APKG_SIGNATURE_INVALID;
    if (v->signature == APKG_SIGNATURE_VALID) {
        parsed =
            ak_json_parse(snapshot->policy, strlen(snapshot->policy), &policy);
        if (parsed) {
            code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
            goto done;
        }
        char keyhex[65];
        ak_tohex(key, 32, keyhex);
        v->author_trust = ak_policy(&policy, 0, v->authority.data, keyhex)
                              ? APKG_TRUST_ACCEPTED
                              : APKG_TRUST_REJECTED;
    }
    v->reason = row->reason;
    v->target_matches = !strcmp(v->target.data, snapshot->target);
    v->root_state = snapshot->view.state;
    v->root_id = snapshot->view.root_id;
    v->snapshot_digest = snapshot->view.snapshot_digest;
    v->generation = snapshot->view.generation.value;
    *out = info;
    info = NULL;
    code = APKG_OK;
done:
    ak_json_free(&policy);
    apkg_inspection_free(info);
    return code;
}

static const char fields[] = "id version revision state installedversion "
                             "installedrevision summary category "
                             "arch abi kind size requires canupgrade "
                             "canrollback compatibility requirements "
                             "authority target reason signature authortrust "
                             "payloadbytes files directories "
                             "relations review runtime payloadverification "
                             "rootstate rootid generation snapshot";
const char *apkg_info_fields(void) { return fields; }
static void field_value(ak_buf *b, const apkg_inspection_view *v,
                        const char *field) {
    if (!strcmp(field, "id"))
        ak_buf_add(b, "%s", v->name.data);
    else if (!strcmp(field, "version") || !strcmp(field, "installedversion"))
        ak_buf_add(b, "%s", v->version.data);
    else if (!strcmp(field, "revision") || !strcmp(field, "installedrevision"))
        ak_buf_add(b, "%" PRIu64, v->revision);
    else if (!strcmp(field, "state"))
        ak_buf_add(b, "installed");
    else if (!strcmp(field, "arch") || !strcmp(field, "abi")) {
        struct pkg_target target;
        if (pkg_target_parse(v->target.data, &target, NULL, 0))
            b->bad = APKG_INVALID;
        else
            ak_buf_add(b, "%s",
                       !strcmp(field, "arch") ? target.cpu : target.abi);
    } else if (!strcmp(field, "kind"))
        ak_buf_add(b, "%s", v->kind.data);
    else if (!strcmp(field, "compatibility"))
        ak_buf_add(b, "%s",
                   v->target_matches ? "target-match" : "target-mismatch");
    else if (!strcmp(field, "requirements") ||
             !strcmp(field, "payloadverification"))
        ak_buf_add(b, "not-checked");
    else if (!strcmp(field, "authority"))
        ak_buf_add(b, "%s", v->authority.data);
    else if (!strcmp(field, "target"))
        ak_buf_add(b, "%s", v->target.data);
    else if (!strcmp(field, "reason"))
        ak_buf_add(b, "%s",
                   v->reason == APKG_REASON_DEPENDENCY       ? "dependency"
                   : v->reason == APKG_REASON_RECOMMENDATION ? "recommendation"
                                                             : "explicit");
    else if (!strcmp(field, "signature"))
        ak_buf_add(b, "%s",
                   v->signature == APKG_SIGNATURE_VALID     ? "verified"
                   : v->signature == APKG_SIGNATURE_INVALID ? "invalid"
                                                            : "unknown");
    else if (!strcmp(field, "authortrust"))
        ak_buf_add(b, "%s",
                   v->author_trust == APKG_TRUST_ACCEPTED   ? "accepted"
                   : v->author_trust == APKG_TRUST_REJECTED ? "rejected"
                                                            : "unknown");
    else if (!strcmp(field, "payloadbytes"))
        ak_buf_add(b, "%" PRIu64, v->payload_bytes);
    else if (!strcmp(field, "files"))
        ak_buf_add(b, "%" PRIu64, v->file_count);
    else if (!strcmp(field, "directories"))
        ak_buf_add(b, "%" PRIu64, v->directory_count);
    else if (!strcmp(field, "relations"))
        ak_buf_add(b, "%.*s", (int)v->relations.size, v->relations.data);
    else if (!strcmp(field, "review") || !strcmp(field, "runtime"))
        ak_buf_add(b, "not-asserted");
    else if (!strcmp(field, "rootstate"))
        ak_buf_add(b, "%s",
                   v->root_state == APKG_ROOT_PENDING ? "pending" : "ready");
    else if (!strcmp(field, "generation"))
        ak_buf_add(b, "%" PRIu64, v->generation);
    else if (!strcmp(field, "rootid") || !strcmp(field, "snapshot")) {
        char hex[65];
        if (!strcmp(field, "rootid"))
            ak_tohex(v->root_id.bytes, 16, hex);
        else
            ak_tohex(v->snapshot_digest.bytes, 32, hex);
        ak_buf_add(b, "%s", hex);
    } else
        ak_buf_add(b, "unknown");
}
apkg_status apkg_inspection_format(const apkg_inspection *info,
                                   const char *field, char **out,
                                   apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!info)
        return APKG_INVALID;
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
    for (const char *p = fields; *p;) {
        char name[32];
        size_t n = strcspn(p, " ");
        memcpy(name, p, n);
        name[n] = 0;
        p += n;
        if (*p)
            p++;
        if (*wanted && strcmp(wanted, name))
            continue;
        if (!*wanted)
            ak_buf_add(&b, "%s%s ", found ? "\n" : "", name);
        field_value(&b, &info->view, name);
        found = 1;
    }
    if (!found && !b.bad)
        b.bad = ak_fail(error, APKG_INVALID, "unknown inspection field",
                        field ? field : "");
    if (b.bad) {
        free(b.data);
        return b.bad;
    }
    *out = b.data;
    return APKG_OK;
}
apkg_status apkg_client_info(const char *root, const char *name,
                             const char *field, char **out,
                             apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!root || !name || !ak_valid_name(name))
        return APKG_INVALID;
    apkg_open_options options = {0};
    options.contract_version = APKG_CONTRACT_VERSION;
    options.struct_size = sizeof options;
    options.root = (apkg_text){root, strlen(root)};
    apkg_ctx *ctx = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_inspection *info = NULL;
    apkg_status code = apkg_open(&options, &ctx, error);
    if (!code)
        code = apkg_snapshot_read(ctx, &snapshot, error);
    if (code)
        goto done;
    if ((snapshot->view.state != APKG_ROOT_READY &&
         snapshot->view.state != APKG_ROOT_PENDING) ||
        !snapshot->policy) {
        code = APKG_POLICY;
        goto done;
    }
    code = APKG_NOT_FOUND;
    for (size_t i = 0; i < snapshot->nrows; i++)
        if (!strcmp(snapshot->rows[i].name.value.data, name)) {
            code = apkg_snapshot_inspect(snapshot, i, &info, error);
            break;
        }
    if (!code)
        code = apkg_inspection_format(info, field, out, error);
done:
    apkg_inspection_free(info);
    apkg_snapshot_free(snapshot);
    apkg_close(ctx);
    return code;
}
