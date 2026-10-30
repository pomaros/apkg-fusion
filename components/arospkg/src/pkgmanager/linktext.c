/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak <https://tomaszstaniak.com>
 * Copyright (c) 2026 John Knipper
 * Adapted from arospkg 0.7.2 for the common clients. */
/* Finding and shortening web addresses in PkgManager's texts; see
 * linktext.h. Plain C, so the host tests run it. */
#include "linktext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Where an address ends: what can never be part of one in running text. */
static int ends(unsigned char c)
{
    return c <= ' ' || c >= 0x7f || c == '"' || c == '<' || c == '>' || c == '\\'
        || c == '^' || c == '{' || c == '}' || c == '|' || c == '`';
}

int lk_unsafe(const char *u)
{
    return strpbrk(u, "'*;$") != NULL;
}

int lk_openable(const char *u)
{
    if (lk_unsafe(u)) return LK_OPEN_CHARS;
    if (strlen(u) > LK_OPEN_MAX) return LK_OPEN_LONG;
    return LK_OPEN_OK;
}

static int starts(const char *p)
{
    return !strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8);
}

/* An address in running text without what a sentence put after it: final
   punctuation, and a closing bracket the address does not open. */
static size_t sentence_end(const char *p, size_t n)
{
    while (n > 0 && strchr(".,:;!?'", p[n - 1])) n--;
    if (n > 0 && p[n - 1] == ')') {
        int open = 0; size_t i;
        for (i = 0; i < n; i++) open += p[i] == '(' ? 1 : p[i] == ')' ? -1 : 0;
        if (open < 0) n--;
    }
    if (n > 0 && p[n - 1] == ']' && !memchr(p, '[', n)) n--;
    return n;
}

size_t lk_next_url(const char *p, const char **at)
{
    for (; *p; p++) {
        size_t n;
        const char *s;
        if (!starts(p) || (p > *at && !ends((unsigned char)p[-1]) && p[-1] != '(' && p[-1] != '['
                           && p[-1] != '\'' && p[-1] != ':' && p[-1] != '='))
            continue;
        for (n = 0; p[n] && !ends((unsigned char)p[n]); n++) ;
        /* <address>: as written; otherwise where a sentence would end it. */
        if (!(p > *at && p[-1] == '<' && p[n] == '>')) n = sentence_end(p, n);
        s = strstr(p, "://") + 3;
        if ((size_t)(s - p) >= n) { p += n ? n - 1 : 0; continue; }   /* nothing after the scheme */
        *at = p;
        return n;
    }
    return 0;
}

int lk_exact(const char *f)
{
    const char *s;
    if (!f || !starts(f)) return 0;
    s = strstr(f, "://") + 3;
    if (!*s) return 0;
    for (; *f; f++)
        if ((unsigned char)*f <= ' ' || (unsigned char)*f >= 0x7f) return 0;
    return 1;
}

void lk_label(const char *url, char *out, size_t n, int max)
{
    size_t len = strlen(url), tail;
    if ((int)len <= max || max < 12) { snprintf(out, n, "%s", url); return; }
    tail = (size_t)max / 3;
    snprintf(out, n, "%.*s...%s", (int)((size_t)max - 3 - tail), url, url + len - tail);
}

void lk_shorten(const char *in, char *out, size_t n, int max)
{
    size_t k = 0;
    const char *p = in, *at;
    size_t len;
    if (!n) return;
    for (;;) {
        at = p;
        len = lk_next_url(p, &at);
        if (!len) break;
        while (p < at && k + 1 < n) out[k++] = *p++;
        if ((int)len > max && max < 90) {
            /* The label is the start and the end; no copy of the address
               needed, however long it is. */
            size_t tail = (size_t)max / 3, head = (size_t)max - 3 - tail;
            k += (size_t)snprintf(out + k, n - k, "%.*s...%.*s", (int)head, at, (int)tail, at + len - tail);
            if (k >= n) k = n - 1;
        } else {
            size_t i;
            for (i = 0; i < len && k + 1 < n; i++) out[k++] = at[i];
        }
        p = at + len;
    }
    while (*p && k + 1 < n) out[k++] = *p++;
    out[k] = 0;
}

static void list_add(lk_list *l, const char *u, size_t len)
{
    int i;
    char *c;
    for (i = 0; i < l->n; i++)
        if (strlen(l->v[i]) == len && !memcmp(l->v[i], u, len)) return;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        char **v = realloc(l->v, sizeof *v * (size_t)cap);
        if (!v) return;
        l->v = v; l->cap = cap;
    }
    if (!(c = malloc(len + 1))) return;
    memcpy(c, u, len); c[len] = 0;
    l->v[l->n++] = c;
}

void lk_list_text(lk_list *l, const char *text, size_t n)
{
    const char *p, *at;
    char *copy = NULL;
    size_t len;
    if (!text) return;
    if (n != (size_t)-1) {
        /* Only that part, so an address must end inside it. */
        if (!(copy = malloc(n + 1))) return;
        memcpy(copy, text, n); copy[n] = 0;
        text = copy;
    }
    for (p = text; (at = p, len = lk_next_url(p, &at)) > 0; p = at + len)
        list_add(l, at, len);
    free(copy);
}

int lk_list_url(lk_list *l, const char *field)
{
    if (!lk_exact(field)) return 0;
    list_add(l, field, strlen(field));
    return 1;
}

int lk_list_same(const lk_list *a, const lk_list *b)
{
    int i;
    if (a->n != b->n) return 0;
    for (i = 0; i < a->n; i++)
        if (strcmp(a->v[i], b->v[i])) return 0;
    return 1;
}

void lk_list_free(lk_list *l)
{
    int i;
    for (i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL; l->n = l->cap = 0;
}

/* Fusion reports use complete newline-delimited fields. Preserve punctuation
 * in repository values; do not invent a second, trimmed link from their JSON
 * evidence. Other displayed lines retain the upstream running-text rules. */
void lk_list_report(lk_list *l, const char *text)
{
    const char *p;
    if (!text) return;
    for (p = text; *p;) {
        size_t n = strcspn(p, "\n");
        size_t prefix = n >= 19 && !strncmp(p, "Source repository: ", 19) ? 19
                      : n >= 11 && !strncmp(p, "repository ", 11) ? 11 : 0;
        if (prefix) {
            char *field = malloc(n - prefix + 1);
            if (field) {
                memcpy(field, p + prefix, n - prefix);
                field[n - prefix] = 0;
                lk_list_url(l, field);
                free(field);
            }
        } else if (*p != '{') {
            lk_list_text(l, p, n);
        }
        p += n;
        if (*p) p++;
    }
}
