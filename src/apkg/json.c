/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Strict canonical framing; no legacy token parser is used on signed input. */
#include "internal.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static int utf8(const unsigned char *p, size_t n, size_t *used) {
    uint32_t v;
    size_t k, i;
    if (p[0] < 0x80) {
        *used = 1;
        return p[0] >= 32;
    }
    if (p[0] >= 0xc2 && p[0] <= 0xdf) {
        k = 2;
        v = p[0] & 31;
    } else if (p[0] >= 0xe0 && p[0] <= 0xef) {
        k = 3;
        v = p[0] & 15;
    } else if (p[0] >= 0xf0 && p[0] <= 0xf4) {
        k = 4;
        v = p[0] & 7;
    } else
        return 0;
    if (n < k)
        return 0;
    for (i = 1; i < k; i++) {
        if ((p[i] & 0xc0) != 0x80)
            return 0;
        v = (v << 6) | (p[i] & 63);
    }
    if ((k == 3 && v < 0x800) || (k == 4 && v < 0x10000) || v > 0x10ffff ||
        (v >= 0xd800 && v <= 0xdfff))
        return 0;
    *used = k;
    return 1;
}
int ak_text_path_valid(const char *text, size_t n) {
    size_t i = 0, used;
    while (i < n) {
        if (!utf8((const unsigned char *)text + i, n - i, &used))
            return 0;
        i += used;
    }
    return 1;
}
static int string_end(const char *s, size_t n, size_t *at) {
    size_t p = *at + 1, k;
    while (p < n) {
        unsigned char c = (unsigned char)s[p++];
        if (c == '"') {
            *at = p;
            return 1;
        }
        if (c == '\\') {
            if (p >= n)
                return 0;
            c = (unsigned char)s[p++];
            if (c && strchr("\"\\bfnrt", c))
                continue;
            if (c != 'u' || p + 4 > n || s[p] != '0' || s[p + 1] != '0')
                return 0;
            {
                char h[3] = {s[p + 2], s[p + 3], 0};
                unsigned char v;
                if (ak_hex(h, &v, 1) || v >= 32 || v == 8 || v == 9 ||
                    v == 10 || v == 12 || v == 13)
                    return 0;
            }
            p += 4;
        } else {
            p--;
            if (!utf8((const unsigned char *)s + p, n - p, &k))
                return 0;
            p += k;
        }
    }
    return 0;
}
static int decode(const char *s, size_t a, size_t b, char *out, size_t cap,
                  size_t *length) {
    size_t p = a + 1, n = 0;
    while (p + 1 < b) {
        unsigned char c = (unsigned char)s[p++];
        if (c == '\\') {
            c = (unsigned char)s[p++];
            switch (c) {
            case 'b':
                c = 8;
                break;
            case 'f':
                c = 12;
                break;
            case 'n':
                c = 10;
                break;
            case 'r':
                c = 13;
                break;
            case 't':
                c = 9;
                break;
            case 'u': {
                char h[3] = {s[p + 2], s[p + 3], 0};
                if (ak_hex(h, &c, 1))
                    return -1;
                p += 4;
                break;
            }
            default:
                break;
            }
        }
        if (out) {
            if (n + 1 >= cap)
                return -1;
            out[n] = (char)c;
        }
        n++;
    }
    if (out)
        out[n] = 0;
    if (length)
        *length = n;
    return 0;
}
static int parse(ak_json *j, size_t *p, unsigned depth) {
    size_t id = j->count++, last = 0;
    char prev[256], key[256];
    int have = 0;
    if (depth > 32 || id >= j->capacity || *p >= j->size)
        return -1;
    j->tokens[id].start = *p;
    j->tokens[id].type = j->text[*p];
    if (j->text[*p] == '"') {
        if (!string_end(j->text, j->size, p))
            return -1;
    } else if (j->text[*p] == '{' || j->text[*p] == '[') {
        char end = j->text[(*p)++] == '{' ? '}' : ']';
        if (*p < j->size && j->text[*p] == end)
            (*p)++;
        else
            for (;;) {
                if (end == '}') {
                    size_t a = *p, b = *p, keylen;
                    if (a >= j->size || j->text[a] != '"' ||
                        !string_end(j->text, j->size, &b) || b - a > 1048576)
                        return -1;
                    if (decode(j->text, a, b, key, sizeof key, &keylen) ||
                        memchr(key, 0, keylen))
                        return -1;
                    if (have && strcmp(prev, key) >= 0)
                        return -1;
                    memcpy(prev, key, strlen(key) + 1);
                    have = 1;
                    if (parse(j, p, depth + 1) < 0 || *p >= j->size ||
                        j->text[(*p)++] != ':')
                        return -1;
                }
                if (parse(j, p, depth + 1) < 0)
                    return -1;
                if (*p >= j->size)
                    return -1;
                if (j->text[*p] == end) {
                    (*p)++;
                    break;
                }
                if (j->text[(*p)++] != ',')
                    return -1;
            }
    } else {
        const char *v = j->text + *p;
        size_t n = j->size - *p;
        if (n >= 4 && !memcmp(v, "true", 4))
            *p += 4;
        else if (n >= 5 && !memcmp(v, "false", 5))
            *p += 5;
        else if (n >= 4 && !memcmp(v, "null", 4))
            *p += 4;
        else
            return -1;
    }
    last = *p;
    j->tokens[id].end = last;
    j->tokens[id].next = j->count;
    return (int)id;
}
int ak_json_parse(const void *v, size_t n, ak_json *j) {
    size_t p = 0;
    memset(j, 0, sizeof *j);
    if (!v || !n || n > AK_MAX)
        return -1;
    j->text = ak_copy(v, n);
    j->capacity = n / 2 + 1;
    if (j->capacity > AK_TOKENS)
        j->capacity = AK_TOKENS;
    j->tokens = calloc(j->capacity, sizeof *j->tokens);
    j->size = n;
    if (!j->text || !j->tokens) {
        ak_json_free(j);
        errno = ENOMEM;
        return -2;
    }
    if (parse(j, &p, 1) < 0 || p != n) {
        ak_json_free(j);
        return -1;
    }
    return 0;
}
void ak_json_free(ak_json *j) {
    free(j->text);
    free(j->tokens);
    memset(j, 0, sizeof *j);
}
int ak_member(const ak_json *j, int obj, const char *key) {
    size_t i;
    if (obj < 0 || (size_t)obj >= j->count || j->tokens[obj].type != '{')
        return -1;
    for (i = (size_t)obj + 1; i < j->tokens[obj].next;) {
        int k = (int)i;
        i++;
        if (i >= j->count)
            return -1;
        if (ak_equal(j, k, key))
            return (int)i;
        i = j->tokens[i].next;
    }
    return -1;
}
int ak_string(const ak_json *j, int i, char *o, size_t n) {
    size_t l;
    if (i < 0 || (size_t)i >= j->count || j->tokens[i].type != '"')
        return -1;
    if (decode(j->text, j->tokens[i].start, j->tokens[i].end, o, n, &l))
        return -1;
    return memchr(o, 0, l) ? -1 : 0;
}
int ak_equal(const ak_json *j, int i, const char *s) {
    char b[256];
    return !ak_string(j, i, b, sizeof b) && !strcmp(b, s);
}
int ak_u64(const ak_json *j, int i, uint64_t *out) {
    char s[21];
    uint64_t v = 0;
    size_t k;
    if (ak_string(j, i, s, sizeof s) || !*s || (s[0] == '0' && s[1]))
        return -1;
    for (k = 0; s[k]; k++) {
        unsigned d = (unsigned)(s[k] - '0');
        if (d > 9 || v > (UINT64_MAX - d) / 10)
            return -1;
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}
int ak_hex(const char *s, unsigned char *o, size_t n) {
    size_t i;
    if (strlen(s) != n * 2)
        return -1;
    for (i = 0; i < n; i++) {
        unsigned a, b;
        char x = s[2 * i], y = s[2 * i + 1];
        a = x >= '0' && x <= '9'   ? (unsigned)(x - '0')
            : x >= 'a' && x <= 'f' ? (unsigned)(x - 'a' + 10)
                                   : 99;
        b = y >= '0' && y <= '9'   ? (unsigned)(y - '0')
            : y >= 'a' && y <= 'f' ? (unsigned)(y - 'a' + 10)
                                   : 99;
        if (a > 15 || b > 15)
            return -1;
        o[i] = (unsigned char)(a * 16 + b);
    }
    return 0;
}
void ak_tohex(const unsigned char *p, size_t n, char *s) {
    static const char h[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        s[2 * i] = h[p[i] >> 4];
        s[2 * i + 1] = h[p[i] & 15];
    }
    s[n * 2] = 0;
}
int ak_empty(const ak_json *j, int i) {
    return i >= 0 && (j->tokens[i].type == '[' || j->tokens[i].type == '{') &&
           j->tokens[i].next == (size_t)i + 1;
}
int ak_fields(const ak_json *j, int obj, const char *const *fields, size_t n) {
    size_t i, k, count = 0;
    if (obj < 0 || j->tokens[obj].type != '{')
        return 0;
    for (i = (size_t)obj + 1; i < j->tokens[obj].next;) {
        int found = 0;
        for (k = 0; k < n; k++)
            if (ak_equal(j, (int)i, fields[k]))
                found = 1;
        if (!found)
            return 0;
        count++;
        i++;
        i = j->tokens[i].next;
    }
    return count == n;
}
int ak_object(const ak_json *j, const char *type) {
    static const char *const f[] = {"body", "format", "requires", "schema",
                                    "type"};
    return ak_fields(j, 0, f, 5) &&
           ak_equal(j, ak_member(j, 0, "format"), "apkg-json-1") &&
           ak_equal(j, ak_member(j, 0, "schema"), "1") &&
           ak_equal(j, ak_member(j, 0, "type"), type) &&
           ak_empty(j, ak_member(j, 0, "requires")) &&
           j->tokens[ak_member(j, 0, "body")].type == '{';
}
