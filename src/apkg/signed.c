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
/* Pure domain-separated verification; role/target authorization belongs to
 * the caller. No trust is created by a valid cryptographic envelope. */
apkg_status ak_signature_check(apkg_bytes object, apkg_bytes signature,
                               const char *domain, unsigned char key[32],
                               apkg_digest *digest) {
    ak_json sig = {0};
    apkg_status code = APKG_SIGNATURE;
    char hex[65], keyhex[65], sighex[129];
    unsigned char bytes[64], message[83];
    uint64_t length;
    unsigned char tag = !strcmp(domain, "descriptor")    ? 1
                        : !strcmp(domain, "channel")     ? 2
                        : !strcmp(domain, "attestation") ? 3
                                                         : 0;
    if (!tag)
        return APKG_UNSUPPORTED;
    int parsed = ak_json_parse(signature.data, signature.size, &sig);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    const char *const fields[] = {"bytes",      "domain", "format",
                                  "public_key", "sha256", "signature"};
    if (!ak_fields(&sig, 0, fields, 6) ||
        !ak_equal(&sig, ak_member(&sig, 0, "format"), "apkg-signature-1") ||
        !ak_equal(&sig, ak_member(&sig, 0, "domain"), domain))
        goto done;
    if (ak_u64(&sig, ak_member(&sig, 0, "bytes"), &length) ||
        length != object.size) {
        code = APKG_INTEGRITY;
        goto done;
    }
    ak_hash(object.data, object.size, digest);
    ak_tohex(digest->bytes, 32, hex);
    if (!ak_equal(&sig, ak_member(&sig, 0, "sha256"), hex)) {
        code = APKG_INTEGRITY;
        goto done;
    }
    if (ak_string(&sig, ak_member(&sig, 0, "public_key"), keyhex,
                  sizeof keyhex) ||
        ak_hex(keyhex, key, 32) ||
        ak_string(&sig, ak_member(&sig, 0, "signature"), sighex,
                  sizeof sighex) ||
        ak_hex(sighex, bytes, 64))
        goto done;
    memcpy(message, "APKG-SIG\0\1", 10);
    message[10] = tag;
    for (size_t i = 0; i < 8; i++)
        message[11 + i] = (unsigned char)(length >> (56 - 8 * i));
    memcpy(message + 19, digest->bytes, 32);
    memcpy(message + 51, key, 32);
    code = pkg_ed25519_verify(bytes, message, sizeof message, key)
               ? APKG_SIGNATURE
               : APKG_OK;
done:
    ak_json_free(&sig);
    return code;
}
apkg_status apkg_package_read(apkg_ctx *c, apkg_bytes descriptor,
                              apkg_bytes signature, apkg_package **out,
                              apkg_error **e) {
    apkg_package *p = NULL;
    apkg_snapshot *s = NULL;
    ak_json policy = {0};
    apkg_status r = APKG_INVALID;
    char key[65];
    int body;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c)
        return APKG_INVALID;
    if (descriptor.size > c->object_max || signature.size > c->object_max ||
        descriptor.size > c->memory_max / 32 ||
        signature.size > c->memory_max / 32)
        return ak_fail(e, APKG_LIMIT, "signed object exceeds budget", "");
    p = calloc(1, sizeof *p);
    if (!p)
        return APKG_NOMEM;
    {
        int parsed = ak_json_parse(descriptor.data, descriptor.size, &p->json);
        if (parsed) {
            r = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
            goto fail;
        }
    }
    r = ak_signature_check(descriptor, signature, "descriptor", p->key,
                           &p->digest);
    if (r)
        goto fail;
    ak_tohex(p->key, 32, key);
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
    if (!p->name || !p->version || !p->authority || !p->target || !p->raw ||
        !p->signature) {
        r = APKG_NOMEM;
        goto fail;
    }
    if (ak_u64(&p->json, ak_member(&p->json, body, "revision"), &p->revision))
        goto fail;
    r = apkg_snapshot_read(c, &s, e);
    if (r != APKG_OK)
        goto done;
    if (s->view.state != APKG_ROOT_READY) {
        r = s->view.state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED
                                               : APKG_POLICY;
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
    ak_json_free(&policy);
    apkg_snapshot_free(s);
    apkg_package_free(p);
    return r;
}
void apkg_package_free(apkg_package *p) {
    if (p) {
        free(p->raw);
        free(p->signature);
        free(p->origin);
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
