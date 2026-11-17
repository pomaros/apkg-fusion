/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak */
/* An installed entry that carries no signature.
 *
 * A package installed by the classic client came from an unsigned catalogue
 * archive. Migration registers it with "signature": null, the reserved
 * all-zero authority and adoption evidence whose source is arospkg-v1. The
 * form is recognised from the structure of the entry alone: nothing here
 * calls signature code, and no policy can grant the reserved authority. */
#include "internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int token_null(const ak_json *j, int t) {
    return t >= 0 && j->tokens[t].end - j->tokens[t].start == 4 &&
           !memcmp(j->text + j->tokens[t].start, "null", 4);
}
int ak_entry_unsigned(const ak_json *j, int entry) {
    return token_null(j, ak_member(j, entry, "signature"));
}
int ak_authority_reserved(const char *authority) {
    return !strcmp(authority, AK_UNSIGNED_AUTHORITY);
}
/* The descriptor of an unsigned entry: reserved authority, and every file
 * named by hash only. It holds no bytes, so nothing can be restored from it. */
int ak_unsigned_descriptor(const ak_json *d) {
    int b = ak_member(d, 0, "body"), files = ak_member(d, b, "files");
    if (b < 0 || files < 0 ||
        !ak_equal(d, ak_member(d, b, "authority"), AK_UNSIGNED_AUTHORITY))
        return 0;
    for (size_t i = (size_t)files + 1; i < d->tokens[files].next;
         i = d->tokens[i].next)
        if (!ak_equal(d, ak_member(d, (int)i, "type"), "directory") &&
            (ak_member(d, (int)i, "blob") < 0 ||
             ak_member(d, (int)i, "config") >= 0 ||
             ak_member(d, (int)i, "amiga") >= 0))
            return 0;
    return 1;
}
/* Whole-entry form. The caller has already checked the descriptor schema and
 * the adoption object. Sets errno to ENOMEM when memory ran out. */
int ak_unsigned_entry_valid(const ak_json *j, int entry) {
    int desc = ak_member(j, entry, "descriptor"),
        adoption = ak_member(j, entry, "adoption"), evidence, valid;
    ak_json d = {0}, spec = {0};
    errno = 0;
    if (!ak_entry_unsigned(j, entry) || desc < 0 || adoption < 0)
        return 0;
    evidence = ak_member(j, adoption, "evidence");
    if (evidence < 0)
        return 0;
    if (ak_json_parse(j->text + j->tokens[desc].start,
                      j->tokens[desc].end - j->tokens[desc].start, &d))
        return 0;
    valid = ak_unsigned_descriptor(&d);
    ak_json_free(&d);
    if (!valid)
        return 0;
    if (ak_json_parse(j->text + j->tokens[evidence].start,
                      j->tokens[evidence].end - j->tokens[evidence].start,
                      &spec))
        return 0;
    valid = ak_equal(&spec,
                     ak_member(&spec, ak_member(&spec, 0, "body"), "source"),
                     "arospkg-v1");
    ak_json_free(&spec);
    errno = 0;
    return valid;
}
int ak_package_unsigned(const apkg_package *p) {
    return p && p->signature && !strcmp(p->signature, "null");
}
static char *text_field(const ak_json *j, int body, const char *name,
                        size_t max) {
    char *p = malloc(max);
    if (p && ak_string(j, ak_member(j, body, name), p, max)) {
        free(p);
        p = NULL;
    }
    return p;
}
/* Read an unsigned descriptor as a package object. The result can only be
 * registered through adoption or carried in installed state. */
apkg_status ak_unsigned_package(apkg_ctx *c, apkg_bytes descriptor,
                                apkg_package **out) {
    apkg_package *p;
    apkg_snapshot *s = NULL;
    apkg_status code = APKG_INVALID;
    int body;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c || !descriptor.data)
        return APKG_INVALID;
    if (descriptor.size > c->object_max ||
        descriptor.size > c->memory_max / 32)
        return APKG_LIMIT;
    p = calloc(1, sizeof *p);
    if (!p)
        return APKG_NOMEM;
    {
        int parsed = ak_json_parse(descriptor.data, descriptor.size, &p->json);
        if (parsed) {
            code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
            goto done;
        }
    }
    errno = 0;
    if (!ak_descriptor(&p->json)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    if (!ak_unsigned_descriptor(&p->json)) {
        code = APKG_TRUST;
        goto done;
    }
    body = ak_member(&p->json, 0, "body");
    p->name = text_field(&p->json, body, "name", 256);
    p->version = text_field(&p->json, body, "version", 256);
    p->authority = text_field(&p->json, body, "authority", 33);
    p->target = text_field(&p->json, body, "target", 32);
    p->raw = ak_copy(descriptor.data, descriptor.size);
    p->signature = ak_copy("null", 4);
    p->raw_size = descriptor.size;
    if (!p->name || !p->version || !p->authority || !p->target || !p->raw ||
        !p->signature) {
        code = APKG_NOMEM;
        goto done;
    }
    if (ak_u64(&p->json, ak_member(&p->json, body, "revision"),
               &p->revision)) {
        code = APKG_INVALID;
        goto done;
    }
    ak_hash(descriptor.data, descriptor.size, &p->digest);
    code = apkg_snapshot_read(c, &s, NULL);
    if (code)
        goto done;
    if (s->view.state != APKG_ROOT_READY)
        code = s->view.state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED
                                                  : APKG_POLICY;
    else if (strcmp(p->target, s->target))
        code = APKG_TARGET;
    if (!code) {
        *out = p;
        p = NULL;
    }
done:
    apkg_snapshot_free(s);
    apkg_package_free(p);
    return code;
}
