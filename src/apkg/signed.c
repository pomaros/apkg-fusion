/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include "pkg_ed25519.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static char *field(const ak_json *j, int body, const char *name, size_t max) {
    char *p = malloc(max);
    if (!p)
        return NULL;
    if (ak_string(j, ak_member(j, body, name), p, max)) {
        free(p);
        return NULL;
    }
    return p;
}
apkg_status apkg_package_read(apkg_ctx *c, apkg_bytes descriptor, apkg_bytes signature,
                              apkg_package **out, apkg_error **e) {
    apkg_package *p = NULL;
    apkg_snapshot *s = NULL;
    ak_json sig = {0}, policy = {0};
    apkg_status r = APKG_INVALID;
    char key[65], hex[65], signature_hex[129];
    unsigned char message[83], bytes[64];
    uint64_t len;
    int body;
    size_t i;
    static const char *const fields[] = {"bytes",      "domain", "format",
                                         "public_key", "sha256", "signature"};
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c)
        return APKG_INVALID;
    if (descriptor.size > c->object_max || signature.size > c->object_max ||
        descriptor.size > c->memory_max / 32 || signature.size > c->memory_max / 32)
        return ak_fail(e, APKG_LIMIT, "signed object exceeds budget", "");
    p = calloc(1, sizeof *p);
    if (!p)
        return APKG_NOMEM;
    {
        int parsed = ak_json_parse(descriptor.data, descriptor.size, &p->json);
        if (!parsed)
            parsed = ak_json_parse(signature.data, signature.size, &sig);
        if (parsed) {
            r = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
            goto fail;
        }
    }
    if (!ak_fields(&sig, 0, fields, 6) ||
        !ak_equal(&sig, ak_member(&sig, 0, "format"), "apkg-signature-1") ||
        !ak_equal(&sig, ak_member(&sig, 0, "domain"), "descriptor")) {
        r = APKG_SIGNATURE;
        goto fail;
    }
    if (ak_u64(&sig, ak_member(&sig, 0, "bytes"), &len) || len != descriptor.size) {
        r = APKG_INTEGRITY;
        goto fail;
    }
    ak_hash(descriptor.data, descriptor.size, &p->digest);
    ak_tohex(p->digest.bytes, 32, hex);
    if (!ak_equal(&sig, ak_member(&sig, 0, "sha256"), hex)) {
        r = APKG_INTEGRITY;
        goto fail;
    }
    if (ak_string(&sig, ak_member(&sig, 0, "public_key"), key, sizeof key) ||
        ak_hex(key, p->key, 32) ||
        ak_string(&sig, ak_member(&sig, 0, "signature"), signature_hex, sizeof signature_hex) ||
        ak_hex(signature_hex, bytes, 64)) {
        r = APKG_SIGNATURE;
        goto fail;
    }
    memcpy(message, "APKG-SIG\0\1", 10);
    message[10] = 1;
    for (i = 0; i < 8; i++)
        message[11 + i] = (unsigned char)(len >> (56 - 8 * i));
    memcpy(message + 19, p->digest.bytes, 32);
    memcpy(message + 51, p->key, 32);
    if (pkg_ed25519_verify(bytes, message, sizeof message, p->key)) {
        r = APKG_SIGNATURE;
        goto fail;
    }
    errno = 0;
    if (!ak_descriptor(&p->json)) {
        r = errno == ENOMEM    ? APKG_NOMEM
            : errno == ENOEXEC ? APKG_TARGET
            : errno == EIO     ? APKG_INTEGRITY
                               : APKG_UNSUPPORTED;
        goto fail;
    }
    body = ak_member(&p->json, 0, "body");
    p->name = field(&p->json, body, "name", 256);
    p->version = field(&p->json, body, "version", 256);
    p->authority = field(&p->json, body, "authority", 33);
    p->target = field(&p->json, body, "target", 32);
    p->raw = ak_copy(descriptor.data, descriptor.size);
    p->signature = ak_copy(signature.data, signature.size);
    p->raw_size = descriptor.size;
    if (!p->name || !p->version || !p->authority || !p->target || !p->raw || !p->signature) {
        r = APKG_NOMEM;
        goto fail;
    }
    if (ak_u64(&p->json, ak_member(&p->json, body, "revision"), &p->revision))
        goto fail;
    r = apkg_snapshot_read(c, &s, e);
    if (r != APKG_OK)
        goto done;
    if (s->view.state != APKG_ROOT_READY) {
        r = s->view.state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED : APKG_POLICY;
        goto fail;
    }
    if (strcmp(p->target, s->target)) {
        r = APKG_TARGET;
        goto fail;
    }
    {
        int parsed = ak_json_parse(s->policy, strlen(s->policy), &policy);
        if (parsed) {
            r = parsed == -2 ? APKG_NOMEM : APKG_TRUST;
            goto fail;
        }
    }
    if (!ak_policy(&policy, 0, p->authority, key)) {
        r = APKG_TRUST;
        goto fail;
    }
    *out = p;
    p = NULL;
    r = APKG_OK;
    goto done;
fail:
    if (e && !*e)
        ak_fail(e, r, "signed descriptor refused", "");
done:
    ak_json_free(&sig);
    ak_json_free(&policy);
    apkg_snapshot_free(s);
    apkg_package_free(p);
    return r;
}
void apkg_package_free(apkg_package *p) {
    if (p) {
        free(p->raw);
        free(p->signature);
        free(p->name);
        free(p->version);
        free(p->authority);
        free(p->target);
        ak_json_free(&p->json);
        free(p);
    }
}
apkg_bytes apkg_package_descriptor(const apkg_package *p) {
    apkg_bytes b = {(const unsigned char *)p->raw, p->raw_size};
    return b;
}
