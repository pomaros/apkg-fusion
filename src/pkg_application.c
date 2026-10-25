/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak <https://tomaszstaniak.com>
 * Copyright (c) 2026 John Knipper
 *
 * Application-folder path and launch rules adapted from arospkg app.c,
 * commit 8e6ab1858b32bbf7a80f2f1c885a11af69aeb9f7 (MIT).
 * The bounded JSON reader and common API enforce these rules on untrusted
 * application.json files without depending on either installation database.
 */
#include "pkg_application.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Offset tokens keep allocation bounded by file length and avoid recursive
 * heap objects. A separate depth limit bounds the parser's C stack. */
enum json_type { J_OBJECT, J_ARRAY, J_STRING, J_NUMBER, J_LITERAL };
struct token { enum json_type type; size_t start, end; int next, children; };
struct reader {
    const char *text;
    size_t length, pos;
    struct token *tokens;
    int count, capacity;
};
#define JSON_TOKENS 4096
#define JSON_DEPTH 32

static int fail(char *error, size_t size, const char *reason)
{
    if (error && size) snprintf(error, size, "%s", reason);
    return -1;
}

int pkg_application_path_valid(const char *rel, char *why, size_t n)
{
    const char *p, *seg;
    if (!rel || !*rel) return fail(why, n, "the path is empty");
    if (strlen(rel) >= 256) return fail(why, n, "the path is longer than 255 characters");
    if (rel[0] == '/') return fail(why, n, "the path must be relative to the drawer");
    for (p = rel; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == ':' || ch == '\\') return fail(why, n, "the path names a volume or uses a backslash");
        if (ch < 0x20 || ch >= 0x7f || ch == '"')
            return fail(why, n, "the path contains a control, quote or non-ASCII character");
    }
    for (seg = rel; ; ) {
        size_t k = strcspn(seg, "/");
        if (k == 0) return fail(why, n, "the path has an empty component");
        if (k == 2 && seg[0] == '.' && seg[1] == '.')
            return fail(why, n, "the path leads out of the drawer");
        if (k == 1 && seg[0] == '.' && strcmp(rel, ".") != 0)
            return fail(why, n, "the path has a dot component");
        if (!seg[k]) break;
        seg += k + 1;
    }
    return 0;
}

static void whitespace(struct reader *r)
{
    while (r->pos < r->length && (r->text[r->pos] == ' ' || r->text[r->pos] == '\t'
           || r->text[r->pos] == '\r' || r->text[r->pos] == '\n')) r->pos++;
}
static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int unicode_word(const char *s, size_t end, size_t *pos, unsigned *value)
{
    int j;
    unsigned v = 0;
    if (end - *pos < 4) return -1;
    for (j = 0; j < 4; j++) {
        int h = hex(s[(*pos)++]);
        if (h < 0) return -1;
        v = v * 16u + (unsigned)h;
    }
    *value = v;
    return 0;
}
/* Read one Unicode scalar from JSON string contents. Controls are valid only
 * when escaped. NUL is refused because all application fields are C strings. */
static int scalar(const char *s, size_t end, size_t *pos, unsigned *value)
{
    unsigned c;
    if (*pos >= end) return -1;
    c = (unsigned char)s[(*pos)++];
    if (c == '\\') {
        unsigned low;
        if (*pos >= end) return -1;
        c = (unsigned char)s[(*pos)++];
        switch (c) {
        case '"': case '\\': case '/': break;
        case 'b': c = 8; break;
        case 'f': c = 12; break;
        case 'n': c = 10; break;
        case 'r': c = 13; break;
        case 't': c = 9; break;
        case 'u':
            if (unicode_word(s, end, pos, &c)) return -1;
            if (c >= 0xd800 && c <= 0xdbff) {
                if (end - *pos < 6 || s[*pos] != '\\' || s[*pos + 1] != 'u') return -1;
                *pos += 2;
                if (unicode_word(s, end, pos, &low) || low < 0xdc00 || low > 0xdfff) return -1;
                c = 0x10000u + (c - 0xd800u) * 1024u + low - 0xdc00u;
            } else if (c >= 0xdc00 && c <= 0xdfff) return -1;
            break;
        default: return -1;
        }
    } else if (c < 0x20 || c == '"') return -1;
    else if (c >= 0x80) {
        int remaining;
        unsigned minimum;
        if (c >= 0xc2 && c <= 0xdf) { remaining = 1; minimum = 0x80; c &= 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { remaining = 2; minimum = 0x800; c &= 0xf; }
        else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; minimum = 0x10000; c &= 7; }
        else return -1;
        while (remaining--) {
            unsigned next;
            if (*pos >= end) return -1;
            next = (unsigned char)s[(*pos)++];
            if ((next & 0xc0) != 0x80) return -1;
            c = c * 64u + (next & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return -1;
    }
    if (c == 0) return -1;
    *value = c;
    return 0;
}
static int string_token(struct reader *r, struct token *t)
{
    unsigned c;
    t->type = J_STRING;
    t->start = ++r->pos;
    while (r->pos < r->length && r->text[r->pos] != '"')
        if (scalar(r->text, r->length, &r->pos, &c)) return -1;
    if (r->pos >= r->length) return -1;
    t->end = r->pos++;
    return 0;
}
static int digit(char c) { return c >= '0' && c <= '9'; }
static int value(struct reader *r, int depth)
{
    int id;
    struct token *t;
    char c;
    whitespace(r);
    if (r->pos >= r->length || depth > JSON_DEPTH || r->count == r->capacity) return -1;
    id = r->count++;
    t = &r->tokens[id];
    t->start = r->pos;
    c = r->text[r->pos];
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        t->type = c == '{' ? J_OBJECT : J_ARRAY;
        r->pos++;
        whitespace(r);
        if (r->pos < r->length && r->text[r->pos] != close) {
            for (;;) {
                if (c == '{') {
                    if (r->pos >= r->length || r->text[r->pos] != '"' || value(r, depth + 1)) return -1;
                    whitespace(r);
                    if (r->pos >= r->length || r->text[r->pos++] != ':') return -1;
                    t->children++;
                }
                if (value(r, depth + 1)) return -1;
                t->children++;
                whitespace(r);
                if (r->pos >= r->length) return -1;
                if (r->text[r->pos] == close) break;
                if (r->text[r->pos++] != ',') return -1;
                whitespace(r);
            }
        }
        if (r->pos >= r->length || r->text[r->pos++] != close) return -1;
        t->end = r->pos;
    } else if (c == '"') {
        if (string_token(r, t)) return -1;
    } else if (c == '-' || digit(c)) {
        t->type = J_NUMBER;
        if (c == '-') r->pos++;
        if (r->pos >= r->length || !digit(r->text[r->pos])) return -1;
        if (r->text[r->pos] == '0') r->pos++;
        else while (r->pos < r->length && digit(r->text[r->pos])) r->pos++;
        if (r->pos < r->length && r->text[r->pos] == '.') {
            r->pos++;
            if (r->pos >= r->length || !digit(r->text[r->pos])) return -1;
            while (r->pos < r->length && digit(r->text[r->pos])) r->pos++;
        }
        if (r->pos < r->length && (r->text[r->pos] == 'e' || r->text[r->pos] == 'E')) {
            r->pos++;
            if (r->pos < r->length && (r->text[r->pos] == '+' || r->text[r->pos] == '-')) r->pos++;
            if (r->pos >= r->length || !digit(r->text[r->pos])) return -1;
            while (r->pos < r->length && digit(r->text[r->pos])) r->pos++;
        }
        t->end = r->pos;
    } else {
        const char *literal = c == 't' ? "true" : c == 'f' ? "false" : "null";
        size_t len = strlen(literal);
        if (r->length - r->pos < len || memcmp(r->text + r->pos, literal, len)) return -1;
        r->pos += len;
        t->type = J_LITERAL;
        t->end = r->pos;
    }
    t->next = r->count;
    return 0;
}
static int string(const struct reader *r, int id, char *out, size_t size)
{
    size_t pos, used = 0;
    const struct token *t;
    if (id < 0 || r->tokens[id].type != J_STRING || !size) return -1;
    t = &r->tokens[id];
    pos = t->start;
    while (pos < t->end) {
        unsigned c;
        unsigned char bytes[4];
        size_t count;
        if (scalar(r->text, t->end, &pos, &c)) return -1;
        if (c < 0x80) { bytes[0] = (unsigned char)c; count = 1; }
        else if (c < 0x800) { bytes[0] = (unsigned char)(0xc0 | c >> 6); bytes[1] = (unsigned char)(0x80 | (c & 63)); count = 2; }
        else if (c < 0x10000) {
            bytes[0] = (unsigned char)(0xe0 | c >> 12); bytes[1] = (unsigned char)(0x80 | ((c >> 6) & 63));
            bytes[2] = (unsigned char)(0x80 | (c & 63)); count = 3;
        } else {
            bytes[0] = (unsigned char)(0xf0 | c >> 18); bytes[1] = (unsigned char)(0x80 | ((c >> 12) & 63));
            bytes[2] = (unsigned char)(0x80 | ((c >> 6) & 63)); bytes[3] = (unsigned char)(0x80 | (c & 63)); count = 4;
        }
        if (count >= size - used) return -1;
        memcpy(out + used, bytes, count);
        used += count;
    }
    out[used] = 0;
    return 0;
}
/* All object keys are decoded before comparison: "mode" and "\u006dode"
 * name the same member. Refuse ambiguity throughout the document. */
static int unique_keys(const struct reader *r)
{
    int object;
    for (object = 0; object < r->count; object++) {
        int a;
        if (r->tokens[object].type != J_OBJECT) continue;
        for (a = object + 1; a < r->tokens[object].next; a = r->tokens[a + 1].next) {
            int b;
            char ka[128], kb[128];
            if (string(r, a, ka, sizeof ka)) return -1;
            for (b = r->tokens[a + 1].next; b < r->tokens[object].next; b = r->tokens[b + 1].next)
                if (string(r, b, kb, sizeof kb) || !strcmp(ka, kb)) return -1;
        }
    }
    return 0;
}
static int member(const struct reader *r, int object, const char *name)
{
    int i;
    if (object < 0 || r->tokens[object].type != J_OBJECT) return -1;
    for (i = object + 1; i < r->tokens[object].next; i = r->tokens[i + 1].next) {
        char key[128];
        if (!string(r, i, key, sizeof key) && !strcmp(key, name)) return i + 1;
    }
    return -1;
}
static int known_keys(const struct reader *r, int object, const char *const *keys)
{
    int i;
    if (object < 0 || r->tokens[object].type != J_OBJECT) return -1;
    for (i = object + 1; i < r->tokens[object].next; i = r->tokens[i + 1].next) {
        char key[128];
        size_t j;
        if (string(r, i, key, sizeof key)) return -1;
        for (j = 0; keys[j] && strcmp(keys[j], key); j++) { }
        if (!keys[j]) return -1;
    }
    return 0;
}
static int field(const struct reader *r, int object, const char *name, char *out, size_t size, int required)
{
    int id = member(r, object, name);
    size_t i;
    if (id < 0) { out[0] = 0; return required ? -1 : 0; }
    if (string(r, id, out, size) || !out[0]) return -1;
    for (i = 0; out[i]; i++) if ((unsigned char)out[i] < 0x20 || out[i] == 0x7f) return -1;
    return 0;
}
static int number(const struct reader *r, int id, unsigned long *out, int required)
{
    size_t i;
    unsigned long n = 0;
    if (id < 0) { *out = 0; return required ? -1 : 0; }
    if (r->tokens[id].type != J_NUMBER) return -1;
    for (i = r->tokens[id].start; i < r->tokens[id].end; i++) {
        unsigned d;
        if (!digit(r->text[i])) return -1;
        d = (unsigned)(r->text[i] - '0');
        if (n > (ULONG_MAX - d) / 10) return -1;
        n = n * 10 + d;
    }
    *out = n;
    return 0;
}

static int application(const struct reader *r, struct pkg_application *a, char *why, size_t n)
{
    static const char *const launch_keys[] = { "executable", "mode", "working_directory", NULL };
    static const char *const target_keys[] = { "os", "arch", "abi", NULL };
    static const char *const req_keys[] = { "type", "id", "min_version", NULL };
    unsigned long schema;
    int targets, target, launch, reqs;
    char mode[16];
    const char *slash;
    if (number(r, member(r, 0, "schema"), &schema, 1) || schema != 1)
        return fail(why, n, "application.json requires integer schema 1");
    if (field(r, 0, "id", a->id, sizeof a->id, 1)
        || field(r, 0, "version", a->version, sizeof a->version, 1)
        || field(r, 0, "name", a->name, sizeof a->name, 0)
        || number(r, member(r, 0, "revision"), &a->revision, 0))
        return fail(why, n, "application identity is missing, oversized or invalid");
    if (!a->name[0]) snprintf(a->name, sizeof a->name, "%s", a->id);
    targets = member(r, 0, "targets");
    if (targets < 0 || r->tokens[targets].type != J_ARRAY || r->tokens[targets].children != 1)
        return fail(why, n, "application.json requires exactly one target");
    target = targets + 1;
    if (known_keys(r, target, target_keys)
        || field(r, target, "os", a->os, sizeof a->os, 1) || strcmp(a->os, "aros")
        || field(r, target, "arch", a->arch, sizeof a->arch, 1)
        || field(r, target, "abi", a->abi, sizeof a->abi, 1))
        return fail(why, n, "target requires os aros, CPU and ABI, with no unknown fields");
    reqs = member(r, 0, "requires_system");
    if (reqs >= 0) {
        int i;
        if (r->tokens[reqs].type != J_ARRAY || r->tokens[reqs].children > PKG_APPLICATION_MAX_REQUIREMENTS)
            return fail(why, n, "requires_system must be an array of at most 16 requirements");
        for (i = reqs + 1; i < r->tokens[reqs].next; i = r->tokens[i].next) {
            struct pkg_application_requirement *req = &a->requirements[a->nrequirements];
            if (known_keys(r, i, req_keys) || field(r, i, "type", req->type, sizeof req->type, 1)
                || field(r, i, "id", req->id, sizeof req->id, 1)
                || number(r, member(r, i, "min_version"), &req->min_version, 0))
                return fail(why, n, "a system requirement is missing, oversized or invalid");
            a->nrequirements++;
        }
    }
    launch = member(r, 0, "launch");
    if (known_keys(r, launch, launch_keys)
        || field(r, launch, "executable", a->executable, sizeof a->executable, 1)
        || field(r, launch, "mode", mode, sizeof mode, 1)
        || field(r, launch, "working_directory", a->working_directory, sizeof a->working_directory, 0))
        return fail(why, n, "launch requires executable and mode, with no unknown fields");
    if (!strcmp(mode, "workbench")) a->mode = PKG_APPLICATION_WORKBENCH;
    else if (!strcmp(mode, "shell")) a->mode = PKG_APPLICATION_SHELL;
    else return fail(why, n, "launch mode must be workbench or shell");
    if (pkg_application_path_valid(a->executable, why, n)) return -1;
    if (!strcmp(a->executable, ".")) return fail(why, n, "launch executable names the drawer");
    if (strlen(a->executable) >= 5 && !strcmp(a->executable + strlen(a->executable) - 5, ".info"))
        return fail(why, n, "launch executable names an icon");
    slash = strrchr(a->executable, '/');
    if (!a->working_directory[0]) {
        if (slash) snprintf(a->working_directory, sizeof a->working_directory, "%.*s", (int)(slash - a->executable), a->executable);
        else snprintf(a->working_directory, sizeof a->working_directory, ".");
    }
    if (pkg_application_path_valid(a->working_directory, why, n)) return -1;
    if (a->mode == PKG_APPLICATION_WORKBENCH) {
        size_t length = slash ? (size_t)(slash - a->executable) : 1;
        const char *dir = slash ? a->executable : ".";
        if (strlen(a->working_directory) != length || memcmp(a->working_directory, dir, length))
            return fail(why, n, "Workbench starts in the executable's own drawer");
    }
    return 0;
}

int pkg_application_parse(const char *text, size_t length, struct pkg_application *out, char *why, size_t n)
{
    struct reader r;
    int rc;
    if (why && n) why[0] = 0;
    if (!out) return fail(why, n, "application output is required");
    memset(out, 0, sizeof *out);
    if (!text || !length || length > PKG_APPLICATION_MAX_SIZE)
        return fail(why, n, "application.json must contain 1 to 65536 bytes");
    memset(&r, 0, sizeof r);
    r.text = text;
    r.length = length;
    r.capacity = (int)(length < JSON_TOKENS ? length : JSON_TOKENS);
    r.tokens = (struct token *)calloc((size_t)r.capacity, sizeof *r.tokens);
    if (!r.tokens) return fail(why, n, "cannot allocate application JSON tokens");
    rc = value(&r, 0);
    whitespace(&r);
    if (rc || r.pos != length || !r.count || r.tokens[0].type != J_OBJECT)
        rc = fail(why, n, "application.json is malformed or exceeds JSON complexity limits");
    else if (unique_keys(&r)) rc = fail(why, n, "application.json has duplicate or oversized object keys");
    else rc = application(&r, out, why, n);
    free(r.tokens);
    if (rc) memset(out, 0, sizeof *out);
    return rc;
}

const char *pkg_application_binary_arch(const unsigned char *h, size_t n)
{
    if (!h) return "";
    if (n >= 4 && h[0] == 0 && h[1] == 0 && h[2] == 0x03 && h[3] == 0xf3) return "m68k";
    if (n >= 20 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F'
        && (h[4] == 1 || h[4] == 2) && (h[5] == 1 || h[5] == 2) && h[6] == 1) {
        unsigned m = h[5] == 2 ? (unsigned)(h[18] << 8 | h[19]) : (unsigned)(h[19] << 8 | h[18]);
        switch (m) {
        case 3: return "i386";
        case 62: return "x86_64";
        case 183: return "aarch64";
        case 40: return "arm";
        case 20: return "ppc";
        }
    }
    return "";
}
