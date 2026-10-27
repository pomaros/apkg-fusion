/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Amiga metadata is distinct from host ownership. Hosts retain its canonical
 * value in one private xattr; AROS applies representable native facts. */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "pkg_fs.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#ifdef __AROS__
#include <dos/dos.h>
/* DOS stores owner permissions as prohibitions, group/other as grants.
 * These facts are already represented by mode, not additional Amiga metadata. */
static uint64_t native_mode_protection(uint64_t mode) {
    static const uint32_t bits[] = {
        FIBF_OTR_EXECUTE, FIBF_OTR_WRITE, FIBF_OTR_READ,
        FIBF_GRP_EXECUTE, FIBF_GRP_WRITE, FIBF_GRP_READ,
        FIBF_EXECUTE, FIBF_WRITE, FIBF_READ
    };
    uint64_t word = 0;
    for (unsigned bit = 0; bit < 9; bit++)
        if (((mode >> bit) & 1) == (bit < 6))
            word |= bits[bit];
    return word;
}
#endif
#if defined(__APPLE__) || defined(__linux__)
#include <sys/xattr.h>
#endif
#define AK_ATTR_NAME "user.apkg.amiga"
#define AK_ATTR_MAX 16384u
static int hex_bytes(const ak_json *j, int token, char **out, size_t *n) {
    char *hex = ak_slice(j, token);
    unsigned char *bytes;
    if (!hex)
        return -2;
    /* The raw token must be a JSON string containing unescaped lower hex. */
    size_t size = strlen(hex);
    if (size < 2 || hex[0] != '"' || size % 2) {
        free(hex);
        return -1;
    }
    size = (size - 2) / 2;
    bytes = malloc(size + 1);
    if (!bytes) {
        free(hex);
        return -2;
    }
    memmove(hex, hex + 1, strlen(hex));
    hex[size * 2] = 0;
    if (ak_hex(hex, bytes, size)) {
        free(bytes);
        free(hex);
        return -1;
    }
    bytes[size] = 0;
    free(hex);
    *out = (char *)bytes;
    *n = size;
    return 0;
}
/* Comments use UTF-8 in signed records. The native DOS limit is 79 Latin-1
 * characters; reject unrepresentable text on every authoring host. */
static int comment_latin1(const char *text, char out[80]) {
    const unsigned char *p = (const unsigned char *)text;
    size_t n = 0;
    while (*p) {
        unsigned c;
        if (*p < 0x80)
            c = *p++;
        else if ((p[0] == 0xc2 || p[0] == 0xc3) && (p[1] & 0xc0) == 0x80) {
            c = ((p[0] & 31) << 6) | (p[1] & 63);
            p += 2;
        } else
            return -1;
        if (c < 32 || (c >= 127 && c < 160) || n == 79)
            return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return 0;
}
static const char *default_value(uint64_t mode) {
    return mode & 0100 ? "{\"comment\":\"\",\"extra\":[],\"gid\":null,"
                         "\"protection\":\"0\",\"uid\":null}"
                       : "{\"comment\":\"\",\"extra\":[],\"gid\":null,"
                         "\"protection\":\"2\",\"uid\":null}";
}
int ak_attributes_valid(const ak_json *j, int at, uint64_t mode) {
    static const char *const fields[] = {"comment", "extra", "gid",
                                         "protection", "uid"};
    uint64_t word, value;
    size_t i, n;
    char *bytes = NULL;
    int extra;
    errno = 0;
    if (at < 0)
        return 1;
    if (j->tokens[at].end - j->tokens[at].start > AK_ATTR_MAX ||
        !ak_fields(j, at, fields, 5) ||
        ak_u64(j, ak_member(j, at, "protection"), &word))
        return 0;
    if (mode != UINT64_MAX && (word & 2) != (mode & 0100 ? 0 : 2))
        return 0;
    int code = hex_bytes(j, ak_member(j, at, "comment"), &bytes, &n);
    char latin[80];
    if (code || memchr(bytes, 0, n) || comment_latin1(bytes, latin)) {
        free(bytes);
        if (code == -2)
            errno = ENOMEM;
        return 0;
    }
    free(bytes);
    const char *ids[] = {"uid", "gid"};
    for (i = 0; i < 2; i++) {
        int t = ak_member(j, at, ids[i]);
        if (t < 0 || (j->tokens[t].type != 'n' &&
                      (ak_u64(j, t, &value) || value > UINT32_MAX)))
            return 0;
    }
    extra = ak_member(j, at, "extra");
    if (extra < 0 || j->tokens[extra].type != '[')
        return 0;
    for (i = (size_t)extra + 1; i < j->tokens[extra].next;
         i = j->tokens[i].next) {
        const char *const kv[] = {"key", "value"};
        if (!ak_fields(j, (int)i, kv, 2))
            return 0;
        bytes = NULL;
        code = hex_bytes(j, ak_member(j, (int)i, "key"), &bytes, &n);
        if (code || !n || n > 255 || memchr(bytes, 0, n) ||
            memchr(bytes, ' ', n) || !ak_text_path_valid(bytes, n) ||
            !strcmp(bytes, "file") || !strcmp(bytes, "prot") ||
            !strcmp(bytes, "comment") || !strcmp(bytes, "uid") ||
            !strcmp(bytes, "gid")) {
            free(bytes);
            if (code == -2)
                errno = ENOMEM;
            return 0;
        }
        free(bytes);
        bytes = NULL;
        code = hex_bytes(j, ak_member(j, (int)i, "value"), &bytes, &n);
        if (code || n > 4096 || memchr(bytes, 0, n) ||
            (n && !ak_text_path_valid(bytes, n))) {
            free(bytes);
            if (code == -2)
                errno = ENOMEM;
            return 0;
        }
        free(bytes);
    }
    return 1;
}
apkg_status ak_attributes_normalize(const char *raw, uint64_t mode,
                                    char **out) {
    *out = NULL;
    ak_json j = {0};
    ak_buf b = {0};
    uint64_t word;
    int parsed = ak_json_parse(raw, strlen(raw), &j);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    if (!ak_attributes_valid(&j, 0, UINT64_MAX)) {
        ak_json_free(&j);
        return errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
    }
    int p = ak_member(&j, 0, "protection");
    ak_u64(&j, p, &word);
    word = (word & ~UINT64_C(2)) | (mode & 0100 ? 0 : 2);
    b.max = AK_ATTR_MAX;
    ak_buf_add(&b, "%.*s\"%" PRIu64 "\"%s", (int)j.tokens[p].start, j.text,
               word, j.text + j.tokens[p].end);
    ak_json_free(&j);
    if (b.bad) {
        free(b.data);
        return (apkg_status)b.bad;
    }
    if (!strcmp(b.data, default_value(mode)))
        free(b.data);
    else
        *out = b.data;
    return APKG_OK;
}
apkg_status ak_attributes_read(const char *path, uint64_t mode, char **out) {
    *out = NULL;
#if defined(__APPLE__) || defined(__linux__)
    char value[AK_ATTR_MAX + 1];
    ssize_t n;
#ifdef __APPLE__
    n = getxattr(path, AK_ATTR_NAME, value, AK_ATTR_MAX, 0, XATTR_NOFOLLOW);
    if (n < 0 && errno == ENOATTR)
        return APKG_OK;
#else
    n = lgetxattr(path, AK_ATTR_NAME, value, AK_ATTR_MAX);
    if (n < 0 && errno == ENODATA)
        return APKG_OK;
#endif
    if (n < 0)
        return errno == ENOTSUP  ? APKG_OK
               : errno == ERANGE ? APKG_LIMIT
                                 : APKG_IO;
    if (!n || memchr(value, 0, (size_t)n))
        return APKG_INVALID;
    value[n] = 0;
    return ak_attributes_normalize(value, mode, out);
#else
    unsigned long long word = 0;
    char comment[256];
    int native = pkg_fs_amiga_get(path, &word, comment, sizeof comment);
    if (native < 0)
        return APKG_IO;
    if (!native)
        return APKG_OK;
    uint64_t ordinary = mode & 0100 ? 0 : 2;
#ifdef __AROS__
    ordinary = native_mode_protection(mode);
#endif
    if (word == ordinary && !*comment)
        return APKG_OK;
    unsigned char utf8[sizeof comment * 2];
    size_t count = 0;
    for (size_t x = 0; comment[x]; x++) {
        unsigned c = (unsigned char)comment[x];
        if (c >= 128) {
            utf8[count++] = (unsigned char)(0xc0 | (c >> 6));
            utf8[count++] = (unsigned char)(0x80 | (c & 63));
        } else
            utf8[count++] = (unsigned char)c;
    }
    char hex[sizeof utf8 * 2 + 1];
    ak_tohex(utf8, count, hex);
    ak_buf b = {0};
    b.max = AK_ATTR_MAX;
    ak_buf_add(&b,
               "{\"comment\":\"%s\",\"extra\":[],\"gid\":null,\"protection\":"
               "\"%llu\",\"uid\":null}",
               hex, word);
    if (b.bad) {
        free(b.data);
        return (apkg_status)b.bad;
    }
    *out = b.data;
    return APKG_OK;
#endif
}
apkg_status ak_attributes_write(const char *path, const char *raw,
                                uint64_t mode) {
    if (!raw)
        return APKG_OK;
    ak_json j = {0};
    int parsed = ak_json_parse(raw, strlen(raw), &j);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    if (!ak_attributes_valid(&j, 0, mode)) {
        ak_json_free(&j);
        return errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
    }
    ak_json_free(&j);
#if defined(__APPLE__) || defined(__linux__)
#ifdef __APPLE__
    int result =
        setxattr(path, AK_ATTR_NAME, raw, strlen(raw), 0, XATTR_NOFOLLOW);
#else
    int result = lsetxattr(path, AK_ATTR_NAME, raw, strlen(raw), 0);
#endif
    return result ? (errno == ENOTSUP ? APKG_UNSUPPORTED : APKG_IO) : APKG_OK;
#else
    parsed = ak_json_parse(raw, strlen(raw), &j);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    uint64_t word;
    ak_u64(&j, ak_member(&j, 0, "protection"), &word);
    if (word > UINT32_MAX || !ak_empty(&j, ak_member(&j, 0, "extra")) ||
        j.tokens[ak_member(&j, 0, "uid")].type != 'n' ||
        j.tokens[ak_member(&j, 0, "gid")].type != 'n') {
        ak_json_free(&j);
        return APKG_UNSUPPORTED;
    }
    char *comment = NULL;
    size_t n;
    int code = hex_bytes(&j, ak_member(&j, 0, "comment"), &comment, &n);
    ak_json_free(&j);
    if (code)
        return code == -2 ? APKG_NOMEM : APKG_INVALID;
    char latin[80];
    if (comment_latin1(comment, latin)) {
        free(comment);
        return APKG_INVALID;
    }
    int result = pkg_fs_amiga_set(path, word, latin);
    free(comment);
    return result > 0 ? APKG_OK : result == 0 ? APKG_UNSUPPORTED : APKG_IO;
#endif
}
static char *default_attributes(uint64_t mode) {
    const char *value = default_value(mode);
    return ak_copy(value, strlen(value));
}
static char *attributes_at(const ak_json *j, int entry) {
    int at = ak_member(j, entry, "amiga");
    if (at >= 0)
        return ak_slice(j, at);
    uint64_t mode = 0700;
    if (ak_member(j, entry, "mode") >= 0 &&
        ak_u64(j, ak_member(j, entry, "mode"), &mode))
        return NULL;
    return default_attributes(mode);
}
int ak_attributes_equal(const ak_json *a, int x, const ak_json *b, int y) {
    if (ak_member(a, x, "amiga") < 0 && ak_member(b, y, "amiga") < 0)
        return 1;
    char *left = attributes_at(a, x), *right = attributes_at(b, y);
    int equal = left && right ? !strcmp(left, right) : -1;
    free(left);
    free(right);
    return equal;
}
/* Merge metadata independently from content. Changed protection bits, comments,
 * owner facts and opaque extension lists survive upgrades and repairs. */
apkg_status ak_attributes_merge(const ak_json *old, int o, const ak_json *nw,
                                int n, const ak_json *live, int l,
                                uint64_t mode, char **out) {
    *out = NULL;
    if (ak_member(old, o, "amiga") < 0 && ak_member(nw, n, "amiga") < 0 &&
        ak_member(live, l, "amiga") < 0)
        return APKG_OK;
    char *a = o < 0 ? NULL : attributes_at(old, o),
         *b = n < 0 ? NULL : attributes_at(nw, n),
         *c = l < 0 ? NULL : attributes_at(live, l);
    ak_json oj = {0}, nj = {0}, lj = {0};
    ak_buf result = {0};
    apkg_status code = APKG_OK;
    *out = NULL;
    result.max = AK_ATTR_MAX;
    if ((o >= 0 && !a) || (n >= 0 && !b) || (l >= 0 && !c)) {
        code = APKG_NOMEM;
        goto done;
    }
    if (!b) {
        b = c ? ak_copy(c, strlen(c)) : default_attributes(mode);
        if (!b) {
            code = APKG_NOMEM;
            goto done;
        }
    }
    if (!a) {
        a = default_attributes(mode);
        if (!a) {
            code = APKG_NOMEM;
            goto done;
        }
    }
    if (!c) {
        c = ak_copy(a, strlen(a));
        if (!c) {
            code = APKG_NOMEM;
            goto done;
        }
    }
    int parsed = ak_json_parse(a, strlen(a), &oj);
    if (!parsed)
        parsed = ak_json_parse(b, strlen(b), &nj);
    if (!parsed)
        parsed = ak_json_parse(c, strlen(c), &lj);
    if (parsed) {
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    const char *fields[] = {"comment", "extra", "gid", "protection", "uid"};
    ak_buf_add(&result, "{");
    for (size_t i = 0; i < 5; i++) {
        if (i == 3) {
            uint64_t before, next, actual;
            ak_u64(&oj, ak_member(&oj, 0, fields[i]), &before);
            ak_u64(&nj, ak_member(&nj, 0, fields[i]), &next);
            ak_u64(&lj, ak_member(&lj, 0, fields[i]), &actual);
            uint64_t changed = before ^ actual;
            next = (next & ~changed) | (actual & changed);
            next = (next & ~UINT64_C(2)) | (mode & 0100 ? 0 : 2);
            ak_buf_add(&result, ",\"protection\":\"%" PRIu64 "\"", next);
            continue;
        }
        char *before = ak_slice(&oj, ak_member(&oj, 0, fields[i])),
             *actual = ak_slice(&lj, ak_member(&lj, 0, fields[i])),
             *next = ak_slice(&nj, ak_member(&nj, 0, fields[i]));
        if (!before || !actual || !next) {
            free(before);
            free(actual);
            free(next);
            code = APKG_NOMEM;
            goto done;
        }
        ak_buf_add(&result, "%s\"%s\":%s", i ? "," : "", fields[i],
                   strcmp(before, actual) ? actual : next);
        free(before);
        free(actual);
        free(next);
    }
    ak_buf_add(&result, "}");
    if (result.bad) {
        code = (apkg_status)result.bad;
        goto done;
    }
    char *def = default_attributes(mode);
    if (!def) {
        code = APKG_NOMEM;
        goto done;
    }
    if (strcmp(def, result.data)) {
        *out = result.data;
        result.data = NULL;
    }
    free(def);
done:
    free(a);
    free(b);
    free(c);
    free(result.data);
    ak_json_free(&oj);
    ak_json_free(&nj);
    ak_json_free(&lj);
    return code;
}

apkg_status ak_attributes_format(const ak_json *j, int entry, ak_buf *out) {
    int at = ak_member(j, entry, "amiga");
    if (at < 0)
        return APKG_OK;
    char *comment = NULL, *uid = ak_slice(j, ak_member(j, at, "uid")),
         *gid = ak_slice(j, ak_member(j, at, "gid")),
         *word = ak_slice(j, ak_member(j, at, "protection")),
         *extra = ak_slice(j, ak_member(j, at, "extra"));
    size_t n;
    int code = hex_bytes(j, ak_member(j, at, "comment"), &comment, &n);
    if (code || !uid || !gid || !word || !extra) {
        free(comment);
        free(uid);
        free(gid);
        free(word);
        free(extra);
        return APKG_NOMEM;
    }
    ak_buf_add(out,
               "; Amiga protection %s, uid %s, gid %s, comment [%s], "
               "extensions (hex) %s",
               word, uid, gid, comment, extra);
    free(comment);
    free(uid);
    free(gid);
    free(word);
    free(extra);
    return (apkg_status)out->bad;
}
