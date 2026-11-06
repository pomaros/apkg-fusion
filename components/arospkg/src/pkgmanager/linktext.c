/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak <https://tomaszstaniak.com>
 * Copyright (c) 2026 John Knipper
 * Adapted from arospkg 0.7.4 for the common clients. */
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

/* ---------------------------------------------------------------- lk_doc */

static int doc_room(lk_doc *d, size_t more)
{
    if (d->failed) return 0;
    if (d->n + more + 1 > d->cap) {
        size_t cap = d->cap ? d->cap : 256;
        char *t;
        while (cap < d->n + more + 1) cap *= 2;
        if (!(t = realloc(d->text, cap))) { d->failed = 1; return 0; }
        d->text = t;
        d->cap = cap;
    }
    return 1;
}

static void doc_piece(lk_doc *d, size_t start, size_t len, int bold, char *target)
{
    lk_piece *p;
    if (d->failed) { free(target); return; }
    if (d->np == d->pcap) {
        int cap = d->pcap ? d->pcap * 2 : 16;
        lk_piece *v = realloc(d->p, sizeof *v * (size_t)cap);
        if (!v) { d->failed = 1; free(target); return; }
        d->p = v;
        d->pcap = cap;
    }
    p = &d->p[d->np++];
    p->start = start;
    p->len = len;
    p->bold = bold;
    p->target = target;
}

static void doc_add(lk_doc *d, const char *s, size_t n, int bold)
{
    if (!n || !doc_room(d, n)) return;
    memcpy(d->text + d->n, s, n);
    if (bold) doc_piece(d, d->n, n, 1, NULL);
    d->n += n;
    d->text[d->n] = 0;
}

void lk_doc_text(lk_doc *d, const char *s)
{
    if (s) doc_add(d, s, strlen(s), 0);
}

void lk_doc_link(lk_doc *d, const char *label, const char *target, size_t target_len, int bold)
{
    size_t n = strlen(label);
    char *t;
    if (!n || !target_len || !doc_room(d, n)) return;
    if (!(t = malloc(target_len + 1))) { d->failed = 1; return; }
    memcpy(t, target, target_len);
    t[target_len] = 0;
    memcpy(d->text + d->n, label, n);
    doc_piece(d, d->n, n, bold, t);
    if (d->failed) return;
    d->n += n;
    d->text[d->n] = 0;
}

void lk_doc_field(lk_doc *d, const lk_field *f, int max, int bold)
{
    char label[128];
    if (!lk_exact(f->url)) { lk_doc_text(d, f->url); return; }
    if (f->label) snprintf(label, sizeof label, "%s", f->label);
    else lk_label(f->url, label, sizeof label, max);
    lk_doc_link(d, label, f->url, strlen(f->url), bold);
}

void lk_doc_markup(lk_doc *d, const char *text, int max, const lk_field *fields, int nf)
{
    const char *p = text;
    int bold = 0;
    if (!text) return;
    while (*p) {
        const char *at = text, *q;
        size_t len = lk_next_url(p, &at);
        q = len ? at : p + strlen(p);
        while (p < q) {
            if (*p == '\33') {
                if (p[1] == 'b') bold = 1;
                else if (p[1] == 'n') bold = 0;
                p += p[1] ? 2 : 1;
            } else if ((unsigned char)*p >= 1 && (unsigned char)*p <= 8) {
                int k = *p - 1;
                if (k < nf && fields[k].url) lk_doc_field(d, &fields[k], max, bold);
                p++;
            } else {
                const char *r = p;
                while (r < q && *r != '\33' && (unsigned char)*r > 8) r++;
                doc_add(d, p, (size_t)(r - p), bold);
                p = r;
            }
        }
        if (len) {
            char label[128];
            /* The label is cut from a copy of the address: the text may run
               on after it. */
            if (max >= 12 && (int)len > max) {
                size_t tail = (size_t)max / 3;
                snprintf(label, sizeof label, "%.*s...%.*s", (int)((size_t)max - 3 - tail), at, (int)tail, at + len - tail);
            } else if (len < sizeof label) {
                memcpy(label, at, len);
                label[len] = 0;
            } else {
                snprintf(label, sizeof label, "%.*s...", (int)(sizeof label - 4), at);
            }
            lk_doc_link(d, label, at, len, bold);
            p = at + len;
        }
    }
}

int lk_doc_same(const lk_doc *a, const lk_doc *b)
{
    int i;
    if (a->n != b->n || a->np != b->np) return 0;
    if (a->n && memcmp(a->text, b->text, a->n)) return 0;
    for (i = 0; i < a->np; i++) {
        const lk_piece *x = &a->p[i], *y = &b->p[i];
        if (x->start != y->start || x->len != y->len || x->bold != y->bold) return 0;
        if (!x->target != !y->target) return 0;
        if (x->target && strcmp(x->target, y->target)) return 0;
    }
    return 1;
}

void lk_doc_report(lk_doc *d, const char *text, int max)
{
    const char *p;
    if (!text) return;
    for (p = text; *p && !d->failed;) {
        size_t n = strcspn(p, "\n");
        size_t prefix = n >= 19 && !strncmp(p, "Source repository: ", 19) ? 19
                      : n >= 11 && !strncmp(p, "repository ", 11) ? 11 : 0;
        char *line = malloc(n + 1);
        if (!line) { d->failed = 1; return; }
        memcpy(line, p, n);
        line[n] = 0;
        if (prefix) {
            lk_field f;
            char keep = line[prefix];
            line[prefix] = 0;
            lk_doc_text(d, line);
            line[prefix] = keep;
            f.label = NULL;
            f.url = line + prefix;
            lk_doc_field(d, &f, max, 0);
        } else if (*line == '{') {
            lk_doc_text(d, line);
        } else {
            lk_doc_markup(d, line, max, NULL, 0);
        }
        free(line);
        p += n;
        if (*p) { lk_doc_text(d, "\n"); p++; }
    }
}

void lk_doc_free(lk_doc *d)
{
    int i;
    for (i = 0; i < d->np; i++) free(d->p[i].target);
    free(d->p);
    free(d->text);
    memset(d, 0, sizeof *d);
}
