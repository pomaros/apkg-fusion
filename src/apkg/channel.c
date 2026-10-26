/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#define AK_CHANNEL_ROWS 4096u
struct apkg_channel {
    apkg_channel_view view;
    ak_json json;
    apkg_channel_entry_view *rows;
    char *policy, *target;
    char root[AK_PATH], anchor[AK_PATH];
    struct stat binding;
    apkg_id root_id;
};
static apkg_bytes node_bytes(const ak_json *j, int token) {
    apkg_bytes b = {(const unsigned char *)j->text + j->tokens[token].start,
                    j->tokens[token].end - j->tokens[token].start};
    return b;
}
static apkg_text node_text(const ak_json *j, int token) {
    apkg_text t = {j->text + j->tokens[token].start + 1,
                   j->tokens[token].end - j->tokens[token].start - 2};
    return t;
}
static int names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}
apkg_status ak_channel_validate(const ak_json *j) {
    int body, entries;
    uint64_t number;
    char id[33], last[65] = "";
    unsigned char digest[32];
    char **identities = NULL;
    size_t count = 0, k = 0;
    apkg_status code = APKG_UNSUPPORTED;
    if (!ak_object(j, "channel"))
        return APKG_UNSUPPORTED;
    body = ak_member(j, 0, "body");
    entries = ak_member(j, body, "entries");
    if (!ak_fields(j, body, (const char *const[]){"entries", "id", "sequence"},
                   3) ||
        ak_string(j, ak_member(j, body, "id"), id, sizeof id) ||
        ak_hex(id, digest, 16) ||
        ak_u64(j, ak_member(j, body, "sequence"), &number) || !number ||
        entries < 0 || j->tokens[entries].type != '[')
        return APKG_UNSUPPORTED;
    for (size_t i = (size_t)entries + 1; i < j->tokens[entries].next;
         i = j->tokens[i].next)
        if (++count > AK_CHANNEL_ROWS)
            return APKG_LIMIT;
    identities = calloc(count ? count : 1, sizeof *identities);
    if (!identities)
        return APKG_NOMEM;
    for (size_t i = (size_t)entries + 1; i < j->tokens[entries].next;
         i = j->tokens[i].next) {
        int archive = ak_member(j, (int)i, "archive"),
            descriptor = ak_member(j, (int)i, "descriptor"),
            signature = ak_member(j, (int)i, "signature");
        if (!ak_fields(
                j, (int)i,
                (const char *const[]){"archive", "descriptor", "signature"},
                3) ||
            !ak_fields(j, archive, (const char *const[]){"sha256", "size"},
                       2)) {
            code = APKG_UNSUPPORTED;
            goto done;
        }
        char sha[65];
        uint64_t size;
        if (ak_string(j, ak_member(j, archive, "sha256"), sha, sizeof sha) ||
            ak_hex(sha, digest, 32) ||
            ak_u64(j, ak_member(j, archive, "size"), &size) || size < 8 ||
            size > 2147483647u) {
            code = APKG_UNSUPPORTED;
            goto done;
        }
        apkg_bytes d = node_bytes(j, descriptor), s = node_bytes(j, signature);
        unsigned char key[32];
        apkg_digest hash;
        ak_json package = {0};
        int parsed = ak_json_parse(d.data, d.size, &package);
        if (parsed) {
            code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
            goto done;
        }
        errno = 0;
        if (!ak_descriptor(&package)) {
            code = errno == ENOMEM    ? APKG_NOMEM
                   : errno == ENOEXEC ? APKG_TARGET
                   : errno == EIO     ? APKG_INTEGRITY
                                      : APKG_UNSUPPORTED;
            ak_json_free(&package);
            goto done;
        }
        code = ak_signature_check(d, s, "descriptor", key, &hash);
        if (code) {
            ak_json_free(&package);
            goto done;
        }
        ak_tohex(hash.bytes, 32, sha);
        if (*last && strcmp(last, sha) >= 0) {
            code = APKG_CONFLICT;
            ak_json_free(&package);
            goto done;
        }
        strcpy(last, sha);
        int b = ak_member(&package, 0, "body");
        char authority[33], name[256], target[32], version[256];
        uint64_t revision;
        ak_string(&package, ak_member(&package, b, "authority"), authority,
                  sizeof authority);
        ak_string(&package, ak_member(&package, b, "name"), name, sizeof name);
        ak_string(&package, ak_member(&package, b, "target"), target,
                  sizeof target);
        ak_string(&package, ak_member(&package, b, "version"), version,
                  sizeof version);
        ak_u64(&package, ak_member(&package, b, "revision"), &revision);
        char identity[700];
        int n = snprintf(identity, sizeof identity, "%s/%s/%s/%s/%" PRIu64,
                         authority, name, target, version, revision);
        ak_json_free(&package);
        if (n < 0 || (size_t)n >= sizeof identity) {
            code = APKG_LIMIT;
            goto done;
        }
        identities[k] = ak_copy(identity, (size_t)n);
        if (!identities[k++]) {
            code = APKG_NOMEM;
            goto done;
        }
    }
    if (count > 1)
        qsort(identities, count, sizeof *identities, names);
    for (size_t i = 1; i < count; i++)
        if (!strcmp(identities[i - 1], identities[i])) {
            code = APKG_CONFLICT;
            goto done;
        }
    code = APKG_OK;
done:
    for (size_t i = 0; i < count; i++)
        free(identities[i]);
    free(identities);
    return code;
}
apkg_status apkg_channel_read(apkg_ctx *c, const apkg_id *expected,
                              uint64_t floor, apkg_bytes object,
                              apkg_bytes signature, apkg_channel **out,
                              apkg_error **error) {
    apkg_channel *channel = NULL;
    apkg_snapshot *snapshot = NULL;
    ak_json policy = {0};
    apkg_status code = APKG_INVALID;
    unsigned char key[32];
    char hex[65], id[33];
    int body, entries;
    size_t k = 0;
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c || !expected)
        return APKG_INVALID;
    size_t max =
        c->object_max < c->memory_max / 32 ? c->object_max : c->memory_max / 32;
    if (object.size > max || signature.size > max)
        return APKG_LIMIT;
    channel = calloc(1, sizeof *channel);
    if (!channel)
        return APKG_NOMEM;
    int parsed = ak_json_parse(object.data, object.size, &channel->json);
    if (parsed) {
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    code = ak_signature_check(object, signature, "channel", key,
                              &channel->view.snapshot_digest);
    if (code)
        goto done;
    body = ak_member(&channel->json, 0, "body");
    if (ak_string(&channel->json, ak_member(&channel->json, body, "id"), id,
                  sizeof id) ||
        ak_hex(id, channel->view.id.bytes, 16)) {
        code = APKG_UNSUPPORTED;
        goto done;
    }
    if (memcmp(expected->bytes, channel->view.id.bytes, 16)) {
        code = APKG_TRUST;
        goto done;
    }
    code = apkg_snapshot_read(c, &snapshot, error);
    if (code)
        goto done;
    if (snapshot->view.state != APKG_ROOT_READY) {
        code = snapshot->view.state == APKG_ROOT_PENDING
                   ? APKG_RECOVERY_REQUIRED
                   : APKG_POLICY;
        goto done;
    }
    parsed = ak_json_parse(snapshot->policy, strlen(snapshot->policy), &policy);
    if (parsed) {
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    ak_tohex(key, 32, hex);
    if (!ak_channel_policy(&policy, id, hex)) {
        code = APKG_TRUST;
        goto done;
    }
    code = ak_channel_validate(&channel->json);
    if (code)
        goto done;
    ak_u64(&channel->json, ak_member(&channel->json, body, "sequence"),
           &channel->view.sequence);
    if (channel->view.sequence < floor) {
        code = APKG_STALE;
        goto done;
    }
    entries = ak_member(&channel->json, body, "entries");
    for (size_t i = (size_t)entries + 1; i < channel->json.tokens[entries].next;
         i = channel->json.tokens[i].next)
        channel->view.entry_count++;
    channel->rows =
        calloc(channel->view.entry_count ? channel->view.entry_count : 1,
               sizeof *channel->rows);
    if (!channel->rows) {
        code = APKG_NOMEM;
        goto done;
    }
    for (size_t i = (size_t)entries + 1; i < channel->json.tokens[entries].next;
         i = channel->json.tokens[i].next) {
        const ak_json *j = &channel->json;
        apkg_channel_entry_view *row = &channel->rows[k++];
        int descriptor = ak_member(j, (int)i, "descriptor"),
            b = ak_member(j, descriptor, "body"),
            sig = ak_member(j, (int)i, "signature"),
            archive = ak_member(j, (int)i, "archive");
        row->authority = node_text(j, ak_member(j, b, "authority"));
        row->name = node_text(j, ak_member(j, b, "name"));
        row->version = node_text(j, ak_member(j, b, "version"));
        row->target = node_text(j, ak_member(j, b, "target"));
        ak_u64(j, ak_member(j, b, "revision"), &row->revision);
        ak_u64(j, ak_member(j, archive, "size"), &row->archive_bytes);
        row->descriptor = node_bytes(j, descriptor);
        row->signature_envelope = node_bytes(j, sig);
        ak_hash(row->descriptor.data, row->descriptor.size,
                &row->descriptor_digest);
        ak_string(j, ak_member(j, archive, "sha256"), hex, sizeof hex);
        ak_hex(hex, row->archive_digest.bytes, 32);
        char authority[33];
        ak_string(j, ak_member(j, b, "authority"), authority, sizeof authority);
        ak_string(j, ak_member(j, sig, "public_key"), hex, sizeof hex);
        row->author_trust = ak_policy(&policy, 0, authority, hex)
                                ? APKG_TRUST_ACCEPTED
                                : APKG_TRUST_REJECTED;
        row->signature = APKG_SIGNATURE_VALID;
        row->target_matches =
            ak_equal(j, ak_member(j, b, "target"), snapshot->target);
    }
    channel->policy = ak_copy(snapshot->policy, strlen(snapshot->policy));
    channel->target = ak_copy(snapshot->target, strlen(snapshot->target));
    if (!channel->policy || !channel->target) {
        code = APKG_NOMEM;
        goto done;
    }
    strcpy(channel->root, c->root);
    strcpy(channel->anchor, c->anchor);
    channel->binding = c->binding;
    channel->root_id = snapshot->view.root_id;
    channel->view.signature = APKG_SIGNATURE_VALID;
    channel->view.trust = APKG_TRUST_ACCEPTED;
    channel->view.freshness = APKG_UNKNOWN;
    *out = channel;
    channel = NULL;
    code = APKG_OK;
done:
    apkg_snapshot_free(snapshot);
    ak_json_free(&policy);
    apkg_channel_free(channel);
    if (code && error && !*error)
        ak_fail(error, code, "signed channel refused", "");
    return code;
}
const apkg_channel_view *apkg_channel_get(const apkg_channel *c) {
    return c ? &c->view : NULL;
}
apkg_status apkg_channel_entry_at(const apkg_channel *c, uint64_t index,
                                  const apkg_channel_entry_view **out,
                                  apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c)
        return APKG_INVALID;
    if (index >= c->view.entry_count)
        return APKG_NOT_FOUND;
    *out = &c->rows[index];
    return APKG_OK;
}
apkg_status apkg_channel_package(apkg_ctx *ctx, const apkg_channel *channel,
                                 uint64_t index, apkg_package **out,
                                 apkg_error **error) {
    apkg_snapshot *snapshot = NULL;
    apkg_status code;
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!ctx || !channel)
        return APKG_INVALID;
    if (index >= channel->view.entry_count)
        return APKG_NOT_FOUND;
    if (strcmp(ctx->root, channel->root) ||
        strcmp(ctx->anchor, channel->anchor) ||
        ctx->binding.st_ino != channel->binding.st_ino ||
        ctx->binding.st_dev != channel->binding.st_dev ||
        !ak_physical(ctx->anchor, &ctx->binding))
        return APKG_STALE;
    code = apkg_snapshot_read(ctx, &snapshot, error);
    if (code)
        return code;
    if (snapshot->view.state != APKG_ROOT_READY ||
        memcmp(snapshot->view.root_id.bytes, channel->root_id.bytes, 16) ||
        strcmp(snapshot->policy, channel->policy) ||
        strcmp(snapshot->target, channel->target))
        code = APKG_STALE;
    apkg_snapshot_free(snapshot);
    if (code)
        return code;
    return apkg_package_read(ctx, channel->rows[index].descriptor,
                             channel->rows[index].signature_envelope, out,
                             error);
}
void apkg_channel_free(apkg_channel *c) {
    if (!c)
        return;
    free(c->rows);
    free(c->policy);
    free(c->target);
    ak_json_free(&c->json);
    free(c);
}
apkg_status ak_channel_inspect(const char *root, const char *dir,
                               const char *identity, uint64_t floor, char **out,
                               apkg_error **error) {
    apkg_ctx *ctx = NULL;
    apkg_channel *channel = NULL;
    unsigned char *object = NULL, *signature = NULL;
    size_t dn = 0, sn = 0;
    apkg_id id;
    ak_buf b = {0};
    char path[AK_PATH], digest[65];
    apkg_status code = APKG_INVALID;
    apkg_open_options options = {0};
    *out = NULL;
    b.max = AK_MAX;
    if (ak_hex(identity, id.bytes, 16))
        return APKG_INVALID;
    options.contract_version = 1;
    options.struct_size = sizeof options;
    options.root.data = root;
    options.root.size = strlen(root);
    options.mode = APKG_READ;
    code = apkg_open(&options, &ctx, error);
    if (code)
        goto done;
    if (ak_path(path, dir, "channel.json") ||
        ak_read(path, &object, &dn, AK_MAX / 8) ||
        ak_path(path, dir, "channel.signature.json") ||
        ak_read(path, &signature, &sn, 16384)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_IO;
        goto done;
    }
    apkg_bytes d = {object, dn}, s = {signature, sn};
    code = apkg_channel_read(ctx, &id, floor, d, s, &channel, error);
    if (code)
        goto done;
    ak_tohex(channel->view.snapshot_digest.bytes, 32, digest);
    ak_buf_add(&b, "{\"channel_id\":\"%s\",\"entries\":[", identity);
    for (uint64_t i = 0; i < channel->view.entry_count; i++) {
        const apkg_channel_entry_view *r = &channel->rows[i];
        char sha[65];
        ak_tohex(r->descriptor_digest.bytes, 32, sha);
        ak_buf_add(
            &b,
            "%s{\"author_trust\":\"%s\",\"authority\":\"%.*s\",\"descriptor_"
            "sha256\":\"%s\",\"name\":\"%.*s\",\"review\":\"not-asserted\","
            "\"revision\":\"%" PRIu64
            "\",\"runtime_tests\":\"not-asserted\",\"target\":\"%.*s\","
            "\"target_matches\":%s,\"version\":\"%.*s\"}",
            i ? "," : "",
            r->author_trust == APKG_TRUST_ACCEPTED ? "accepted" : "rejected",
            (int)r->authority.size, r->authority.data, sha, (int)r->name.size,
            r->name.data, r->revision, (int)r->target.size, r->target.data,
            r->target_matches ? "true" : "false", (int)r->version.size,
            r->version.data);
    }
    ak_buf_add(&b,
               "],\"freshness\":\"unknown\",\"payload_availability\":\"not-"
               "checked\",\"sequence\":\"%" PRIu64
               "\",\"snapshot_sha256\":\"%s\"}",
               channel->view.sequence, digest);
    code = (apkg_status)b.bad;
    if (!code) {
        *out = b.data;
        b.data = NULL;
    }
done:
    free(object);
    free(signature);
    free(b.data);
    apkg_channel_free(channel);
    apkg_close(ctx);
    return code;
}
