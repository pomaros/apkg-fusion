/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include "pkg_ed25519.h"
#include "pkg_fs.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Keep the existing pkg key-file identity: no key conversion or new principal
 * is needed to author packages for the common engine. */
apkg_status ak_author_command(int argc, char **argv, char **out) {
    unsigned char seed[32] = {0}, pk[32], sk[64] = {0}, signature[64],
                  message[83], *bytes = NULL;
    char seedhex[65], publichex[65], sighex[129], hashhex[65], keytext[256];
    size_t size = 0;
    ak_json j = {0};
    ak_buf b = {0};
    apkg_status code = APKG_INVALID;
    *out = NULL;
    b.max = 16384;
    int keygen = !strcmp(argv[1], "keygen"),
        pub = !strcmp(argv[1], "key-public");
    if ((keygen || pub) ? argc != 3 : argc != 5)
        return APKG_INVALID;
    if (keygen) {
        if (pkg_fs_random(seed, sizeof seed)) {
            code = APKG_UNSUPPORTED;
            goto done;
        }
    } else {
        if (ak_read(argv[2], &bytes, &size, 512)) {
            code = errno == ENOMEM ? APKG_NOMEM : APKG_IO;
            goto done;
        }
        if (memchr(bytes, 0, size) ||
            sscanf((char *)bytes, "Pkg-Secret-Key: 1\nSeed: %64s\nPublic: %64s",
                   seedhex, publichex) != 2 ||
            ak_hex(seedhex, seed, 32)) {
            code = APKG_SIGNATURE;
            goto done;
        }
    }
    pkg_ed25519_keypair(pk, sk, seed);
    ak_tohex(seed, 32, seedhex);
    ak_tohex(pk, 32, publichex);
    int n = snprintf(keytext, sizeof keytext,
                     "Pkg-Secret-Key: 1\nSeed: %s\nPublic: %s\n", seedhex,
                     publichex);
    if (keygen) {
        if (ak_write(argv[2], keytext, (size_t)n, 1)) {
            code = APKG_IO;
            goto done;
        }
    } else if (size != (size_t)n || memcmp(bytes, keytext, size)) {
        code = APKG_SIGNATURE;
        goto done;
    }
    if (bytes) {
        memset(bytes, 0, size);
        free(bytes);
        bytes = NULL;
    }
    if (keygen || pub) {
        ak_buf_add(&b, "{\"public_key\":\"%s\"}", publichex);
        goto output;
    }
    int channel = !strcmp(argv[3], "channel");
    if (strcmp(argv[3], "descriptor") && !channel) {
        code = APKG_UNSUPPORTED;
        goto done;
    }
    if (ak_read(argv[4], &bytes, &size, AK_MAX / 8)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_IO;
        goto done;
    }
    int parsed = ak_json_parse(bytes, size, &j);
    if (parsed) {
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    if (channel) {
        code = ak_channel_validate(&j);
        if (code)
            goto done;
    } else {
        if (!ak_descriptor(&j)) {
            code = errno == ENOMEM ? APKG_NOMEM : APKG_UNSUPPORTED;
            goto done;
        }
        char version[256];
        ak_string(&j, ak_member(&j, ak_member(&j, 0, "body"), "version"),
                  version, sizeof version);
        if (ak_version_compare(version, version) != 0) {
            code = APKG_UNSUPPORTED;
            goto done;
        }
    }
    apkg_digest digest;
    ak_hash(bytes, size, &digest);
    ak_tohex(digest.bytes, 32, hashhex);
    memcpy(message, "APKG-SIG\0\1", 10);
    message[10] = channel ? 2 : 1;
    for (size_t i = 0; i < 8; i++)
        message[11 + i] = (unsigned char)((uint64_t)size >> (56 - 8 * i));
    memcpy(message + 19, digest.bytes, 32);
    memcpy(message + 51, pk, 32);
    pkg_ed25519_sign(signature, message, sizeof message, sk);
    ak_tohex(signature, 64, sighex);
    ak_buf_add(&b,
               "{\"bytes\":\"%zu\",\"domain\":\"%s\",\"format\":\"apkg-"
               "signature-1\",\"public_key\":\"%s\",\"sha256\":\"%s\","
               "\"signature\":\"%s\"}",
               size, channel ? "channel" : "descriptor", publichex, hashhex,
               sighex);
output:
    code = (apkg_status)b.bad;
    if (!code) {
        *out = b.data;
        b.data = NULL;
    }
done:
    memset(seed, 0, sizeof seed);
    memset(sk, 0, sizeof sk);
    memset(seedhex, 0, sizeof seedhex);
    memset(keytext, 0, sizeof keytext);
    if (bytes) {
        memset(bytes, 0, size);
        free(bytes);
    }
    free(b.data);
    ak_json_free(&j);
    return code;
}
/* Cryptographic metadata verification for publishers. This does not grant any
 * role to the signer or assert archive contents/review/runtime qualifications.
 */
apkg_status ak_verify_command(int argc, char **argv, char **out) {
    unsigned char *document = NULL, *envelope = NULL, key[32];
    size_t dn = 0, sn = 0;
    ak_json object = {0};
    ak_buf result = {0};
    apkg_digest digest;
    apkg_status code = APKG_INVALID;
    char hex[65], hash[65];
    *out = NULL;
    result.max = 16384;
    if (argc != 5)
        return APKG_INVALID;
    if (strcmp(argv[2], "descriptor") && strcmp(argv[2], "channel"))
        return APKG_UNSUPPORTED;
    if (ak_read(argv[3], &document, &dn, AK_MAX / 8) ||
        ak_read(argv[4], &envelope, &sn, 16384)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_IO;
        goto done;
    }
    int parsed = ak_json_parse(document, dn, &object);
    if (parsed) {
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    apkg_bytes d = {document, dn}, s = {envelope, sn};
    code = ak_signature_check(d, s, argv[2], key, &digest);
    if (code)
        goto done;
    if (!strcmp(argv[2], "channel"))
        code = ak_channel_validate(&object);
    else {
        errno = 0;
        if (!ak_descriptor(&object))
            code = errno == ENOMEM    ? APKG_NOMEM
                   : errno == EIO     ? APKG_INTEGRITY
                   : errno == ENOEXEC ? APKG_TARGET
                                      : APKG_UNSUPPORTED;
    }
    if (code)
        goto done;
    ak_tohex(key, 32, hex);
    ak_tohex(digest.bytes, 32, hash);
    ak_buf_add(&result,
               "{\"domain\":\"%s\",\"public_key\":\"%s\",\"sha256\":\"%s\"}",
               argv[2], hex, hash);
    code = (apkg_status)result.bad;
    if (!code) {
        *out = result.data;
        result.data = NULL;
    }
done:
    free(document);
    free(envelope);
    free(result.data);
    ak_json_free(&object);
    return code;
}
