/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include "pkg_target.h"
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#define CLAIM_MAX (AK_MAX / 32)
struct apkg_attestation {
    apkg_attestation_view view;
    ak_json json;
    char authority[33], name[256], version[256], target[32], summary[1001],
        evidence[4097];
    char *signature;
};
static int hex_field(const ak_json *j, int b, const char *field,
                     unsigned char *out, size_t n) {
    char text[65];
    return ak_string(j, ak_member(j, b, field), text, 2 * n + 1) ||
           ak_hex(text, out, n);
}
static int text_field(const ak_json *j, int b, const char *field, size_t max) {
    char text[4097];
    if (max > sizeof text || ak_string(j, ak_member(j, b, field), text, max) ||
        !*text)
        return 0;
    for (size_t i = 0; text[i]; i++)
        if ((unsigned char)text[i] < 32 || text[i] == 127)
            return 0;
    return 1;
}
static int is_null(const ak_json *j, int t) {
    return t >= 0 && j->tokens[t].end - j->tokens[t].start == 4 &&
           !memcmp(j->text + j->tokens[t].start, "null", 4);
}
apkg_status ak_attestation_validate(const ak_json *j) {
    if (!ak_object(j, "attestation"))
        return APKG_UNSUPPORTED;
    int b = ak_member(j, 0, "body"), env = ak_member(j, b, "environment");
    const char *fields[] = {"authority",   "descriptor_sha256",
                            "environment", "evidence",
                            "issued_at",   "issuer",
                            "kind",        "name",
                            "result",      "revision",
                            "sequence",    "summary",
                            "supersedes",  "target",
                            "version"};
    char name[256], target[32];
    unsigned char bytes[32];
    uint64_t n, sequence;
    if (!ak_fields(j, b, fields, 15) ||
        hex_field(j, b, "authority", bytes, 16) ||
        hex_field(j, b, "issuer", bytes, 16) ||
        hex_field(j, b, "descriptor_sha256", bytes, 32) ||
        ak_string(j, ak_member(j, b, "name"), name, sizeof name) ||
        !ak_valid_name(name) || !text_field(j, b, "version", 256) ||
        !text_field(j, b, "summary", 1001) ||
        !text_field(j, b, "evidence", 4097) ||
        ak_string(j, ak_member(j, b, "target"), target, sizeof target) ||
        pkg_target_parse(target, NULL, NULL, 0) ||
        ak_u64(j, ak_member(j, b, "revision"), &n) ||
        ak_u64(j, ak_member(j, b, "sequence"), &sequence) || !sequence ||
        ak_u64(j, ak_member(j, b, "issued_at"), &n) || !n)
        return APKG_UNSUPPORTED;
    int previous = ak_member(j, b, "supersedes");
    if (sequence == 1 ? !is_null(j, previous)
                      : hex_field(j, b, "supersedes", bytes, 32))
        return APKG_UNSUPPORTED;
    int result = ak_member(j, b, "result");
    if (!ak_equal(j, result, "passed") && !ak_equal(j, result, "failed") &&
        !ak_equal(j, result, "revoked"))
        return APKG_UNSUPPORTED;
    if (ak_equal(j, result, "revoked") && sequence == 1)
        return APKG_UNSUPPORTED;
    if (ak_equal(j, ak_member(j, b, "kind"), "review"))
        return is_null(j, env) ? APKG_OK : APKG_UNSUPPORTED;
    if (!ak_equal(j, ak_member(j, b, "kind"), "runtime") ||
        !ak_fields(j, env,
                   (const char *const[]){"hardware", "log_sha256", "os_build",
                                         "test_suite"},
                   4) ||
        !text_field(j, env, "hardware", 256) ||
        !text_field(j, env, "os_build", 256) ||
        !text_field(j, env, "test_suite", 256) ||
        hex_field(j, env, "log_sha256", bytes, 32))
        return APKG_UNSUPPORTED;
    return APKG_OK;
}
void apkg_attestation_free(apkg_attestation *a) {
    if (a) {
        ak_json_free(&a->json);
        free(a->signature);
        free(a);
    }
}
const apkg_attestation_view *apkg_attestation_get(const apkg_attestation *a) {
    return a ? &a->view : NULL;
}
apkg_status apkg_attestation_read(const apkg_snapshot *snapshot,
                                  apkg_bytes descriptor,
                                  apkg_bytes descriptor_signature,
                                  apkg_bytes record, apkg_bytes signature,
                                  const apkg_attestation *previous,
                                  apkg_attestation **out, apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!snapshot)
        return APKG_INVALID;
    if (snapshot->view.state != APKG_ROOT_READY)
        return snapshot->view.state == APKG_ROOT_PENDING
                   ? APKG_RECOVERY_REQUIRED
                   : APKG_POLICY;
    if (descriptor.size > CLAIM_MAX || record.size > CLAIM_MAX ||
        descriptor_signature.size > 16384 || signature.size > 16384)
        return APKG_LIMIT;
    apkg_attestation *a = calloc(1, sizeof *a);
    if (!a)
        return APKG_NOMEM;
    ak_json d = {0}, policy = {0};
    unsigned char author_key[32], issuer_key[32];
    apkg_status code = APKG_INVALID;
    int parsed = ak_json_parse(record.data, record.size, &a->json);
    if (parsed) {
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    code = ak_signature_check(record, signature, "attestation", issuer_key,
                              &a->view.record_digest);
    if (code)
        goto done;
    code = ak_attestation_validate(&a->json);
    if (code)
        goto done;
    parsed = ak_json_parse(descriptor.data, descriptor.size, &d);
    if (parsed) {
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    code = ak_signature_check(descriptor, descriptor_signature, "descriptor",
                              author_key, &a->view.descriptor_digest);
    if (code)
        goto done;
    errno = 0;
    if (!ak_descriptor(&d)) {
        code = errno == ENOMEM    ? APKG_NOMEM
               : errno == EIO     ? APKG_INTEGRITY
               : errno == ENOEXEC ? APKG_TARGET
                                  : APKG_UNSUPPORTED;
        goto done;
    }
    int b = ak_member(&a->json, 0, "body"), db = ak_member(&d, 0, "body");
    apkg_digest digest;
    hex_field(&a->json, b, "descriptor_sha256", digest.bytes, 32);
    if (memcmp(digest.bytes, a->view.descriptor_digest.bytes, 32)) {
        code = APKG_INTEGRITY;
        goto done;
    }
    const char *fields[] = {"authority", "name", "version", "target"};
    char *dest[] = {a->authority, a->name, a->version, a->target};
    size_t sizes[] = {sizeof a->authority, sizeof a->name, sizeof a->version,
                      sizeof a->target};
    apkg_text *views[] = {&a->view.authority, &a->view.name, &a->view.version,
                          &a->view.target};
    for (size_t i = 0; i < 4; i++) {
        if (ak_string(&a->json, ak_member(&a->json, b, fields[i]), dest[i],
                      sizes[i]) ||
            !ak_equal(&d, ak_member(&d, db, fields[i]), dest[i])) {
            code = APKG_CONFLICT;
            goto done;
        }
        *views[i] = (apkg_text){dest[i], strlen(dest[i])};
    }
    uint64_t revision;
    ak_u64(&a->json, ak_member(&a->json, b, "revision"), &a->view.revision);
    ak_u64(&d, ak_member(&d, db, "revision"), &revision);
    if (revision != a->view.revision) {
        code = APKG_CONFLICT;
        goto done;
    }
    hex_field(&a->json, b, "issuer", a->view.issuer.bytes, 16);
    ak_u64(&a->json, ak_member(&a->json, b, "sequence"), &a->view.sequence);
    ak_u64(&a->json, ak_member(&a->json, b, "issued_at"), &a->view.issued_at);
    a->view.has_previous = a->view.sequence > 1;
    if (a->view.has_previous)
        hex_field(&a->json, b, "supersedes", a->view.supersedes.bytes, 32);
    a->view.kind = ak_equal(&a->json, ak_member(&a->json, b, "kind"), "review")
                       ? APKG_ATTEST_REVIEW
                       : APKG_ATTEST_RUNTIME;
    a->view.result =
        ak_equal(&a->json, ak_member(&a->json, b, "result"), "passed")
            ? APKG_ATTEST_PASSED
        : ak_equal(&a->json, ak_member(&a->json, b, "result"), "failed")
            ? APKG_ATTEST_FAILED
            : APKG_ATTEST_REVOKED;
    a->view.prefix_complete = !a->view.has_previous;
    if (previous) {
        const apkg_attestation_view *v = &previous->view;
        if (!a->view.has_previous || v->sequence == UINT64_MAX ||
            a->view.sequence != v->sequence + 1 || v->kind != a->view.kind ||
            memcmp(v->issuer.bytes, a->view.issuer.bytes, 16) ||
            memcmp(v->descriptor_digest.bytes, a->view.descriptor_digest.bytes,
                   32) ||
            memcmp(v->record_digest.bytes, a->view.supersedes.bytes, 32) ||
            a->view.issued_at < v->issued_at) {
            code = APKG_CONFLICT;
            goto done;
        }
        if (a->view.kind == APKG_ATTEST_RUNTIME) {
            int current_env = ak_member(&a->json, b, "environment");
            int previous_env =
                ak_member(&previous->json,
                          ak_member(&previous->json, 0, "body"), "environment");
            const char *scope[] = {"hardware", "os_build", "test_suite"};
            for (size_t i = 0; i < 3; i++) {
                char text[256];
                if (ak_string(&a->json,
                              ak_member(&a->json, current_env, scope[i]), text,
                              sizeof text) ||
                    !ak_equal(
                        &previous->json,
                        ak_member(&previous->json, previous_env, scope[i]),
                        text)) {
                    code = APKG_CONFLICT;
                    goto done;
                }
            }
        }
        a->view.prefix_complete = v->prefix_complete;
    }
    parsed = ak_json_parse(snapshot->policy, strlen(snapshot->policy), &policy);
    if (parsed) {
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    char author[65], issuer[65], identity[33];
    ak_tohex(author_key, 32, author);
    ak_tohex(issuer_key, 32, issuer);
    ak_tohex(a->view.issuer.bytes, 16, identity);
    a->view.author_trust = ak_policy(&policy, 0, a->authority, author)
                               ? APKG_TRUST_ACCEPTED
                               : APKG_TRUST_REJECTED;
    a->view.issuer_trust =
        ak_attestation_policy(&policy,
                              a->view.kind == APKG_ATTEST_REVIEW ? "reviewers"
                                                                 : "testers",
                              identity, issuer)
            ? APKG_TRUST_ACCEPTED
            : APKG_TRUST_REJECTED;
    a->view.independent = strcmp(identity, a->authority) &&
                          memcmp(author_key, issuer_key, 32) &&
                          !ak_policy(&policy, 0, a->authority, issuer);
    a->view.target_matches = !strcmp(a->target, snapshot->target);
    a->view.signature = a->view.descriptor_signature = APKG_SIGNATURE_VALID;
    a->view.snapshot_digest = snapshot->view.snapshot_digest;
    a->view.generation = snapshot->view.generation.value;
    ak_string(&a->json, ak_member(&a->json, b, "summary"), a->summary,
              sizeof a->summary);
    ak_string(&a->json, ak_member(&a->json, b, "evidence"), a->evidence,
              sizeof a->evidence);
    a->view.summary = (apkg_text){a->summary, strlen(a->summary)};
    a->view.evidence = (apkg_text){a->evidence, strlen(a->evidence)};
    int env = ak_member(&a->json, b, "environment");
    a->view.environment = (apkg_bytes){
        (const unsigned char *)a->json.text + a->json.tokens[env].start,
        a->json.tokens[env].end - a->json.tokens[env].start};
    a->view.record =
        (apkg_bytes){(const unsigned char *)a->json.text, a->json.size};
    a->signature = ak_copy(signature.data, signature.size);
    if (!a->signature) {
        code = APKG_NOMEM;
        goto done;
    }
    a->view.signature_envelope =
        (apkg_bytes){(const unsigned char *)a->signature, signature.size};
    *out = a;
    a = NULL;
    code = APKG_OK;
done:
    ak_json_free(&d);
    ak_json_free(&policy);
    apkg_attestation_free(a);
    return code ? ak_fail(error, code, "signed attestation refused", "")
                : APKG_OK;
}
apkg_status apkg_attestation_format(const apkg_attestation *a, char **out,
                                    apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!a)
        return APKG_INVALID;
    const apkg_attestation_view *v = &a->view;
    char record[65], descriptor[65], snapshot[65], issuer[33], previous[65];
    ak_tohex(v->record_digest.bytes, 32, record);
    ak_tohex(v->descriptor_digest.bytes, 32, descriptor);
    ak_tohex(v->snapshot_digest.bytes, 32, snapshot);
    ak_tohex(v->issuer.bytes, 16, issuer);
    ak_tohex(v->supersedes.bytes, 32, previous);
    ak_buf b = {0};
    b.max = 32768;
    ak_buf_add(
        &b,
        "kind %s\nresult %s\nissuer %s\nauthority %s\nname %s\nversion "
        "%s\nrevision %" PRIu64 "\ntarget %s\nsequence %" PRIu64
        "\nissuedat %" PRIu64
        "\nrecord %s\ndescriptor %s\nsnapshot %s\ngeneration %" PRIu64
        "\nsupersedes %s\nsignature verified\ndescriptorsignature "
        "verified\nissuertrust %s\nauthortrust %s\nindependent "
        "%s\ntargetmatches %s\nhistory %s\nfreshness unknown\nverification "
        "signed-claim-only\nsummary %s\nevidence %s\nenvironment %.*s\n",
        v->kind == APKG_ATTEST_REVIEW ? "review" : "runtime",
        v->result == APKG_ATTEST_PASSED   ? "passed"
        : v->result == APKG_ATTEST_FAILED ? "failed"
                                          : "revoked",
        issuer, a->authority, a->name, a->version, v->revision, a->target,
        v->sequence, v->issued_at, record, descriptor, snapshot, v->generation,
        v->has_previous ? previous : "none",
        v->issuer_trust == APKG_TRUST_ACCEPTED ? "accepted" : "rejected",
        v->author_trust == APKG_TRUST_ACCEPTED ? "accepted" : "rejected",
        v->independent ? "yes" : "no", v->target_matches ? "yes" : "no",
        v->prefix_complete ? "linked-prefix" : "not-checked", a->summary,
        a->evidence, (int)v->environment.size,
        (const char *)v->environment.data);
    if (b.bad) {
        apkg_status code = (apkg_status)b.bad;
        free(b.data);
        return ak_fail(error, code, "attestation format refused", "");
    }
    *out = b.data;
    return APKG_OK;
}
apkg_status apkg_client_attestation(const char *root, const char *folder,
                                    char **out, apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!root || !folder || !*folder)
        return APKG_INVALID;
    const char *files[] = {"descriptor.json", "signature.json",
                           "attestation.json", "attestation.signature.json"};
    unsigned char *bytes[4] = {0};
    size_t sizes[4] = {0};
    apkg_ctx *ctx = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_attestation *a = NULL;
    apkg_status code = APKG_OK;
    for (size_t i = 0; i < 4; i++) {
        char path[AK_PATH];
        if (ak_path(path, folder, files[i])) {
            code = APKG_LIMIT;
            goto done;
        }
        if (ak_read(path, &bytes[i], &sizes[i], i % 2 ? 16384 : CLAIM_MAX)) {
            code = errno == ENOMEM  ? APKG_NOMEM
                   : errno == EFBIG ? APKG_LIMIT
                                    : APKG_IO;
            goto done;
        }
    }
    apkg_open_options options = {0};
    options.contract_version = APKG_CONTRACT_VERSION;
    options.struct_size = sizeof options;
    options.root = (apkg_text){root, strlen(root)};
    code = apkg_open(&options, &ctx, error);
    if (!code)
        code = apkg_snapshot_read(ctx, &snapshot, error);
    if (!code)
        code = apkg_attestation_read(
            snapshot, (apkg_bytes){bytes[0], sizes[0]},
            (apkg_bytes){bytes[1], sizes[1]}, (apkg_bytes){bytes[2], sizes[2]},
            (apkg_bytes){bytes[3], sizes[3]}, NULL, &a, error);
    if (!code)
        code = apkg_attestation_format(a, out, error);
done:
    for (size_t i = 0; i < 4; i++)
        free(bytes[i]);
    apkg_attestation_free(a);
    apkg_snapshot_free(snapshot);
    apkg_close(ctx);
    return code;
}
