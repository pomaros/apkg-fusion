/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak <https://tomaszstaniak.com>
 * Copyright (c) 2026 John Knipper
 * Adapted from arospkg 0.7.4 for the common clients. */
/* Web addresses in PkgManager's texts: where they are, how they are shown
 * shortened, which may be handed to a browser, and the text with its links
 * as the window's text views take it. No AROS calls, so the host tests cover
 * it (tests/test_linktext.c). */
#ifndef PM_LINKTEXT_H
#define PM_LINKTEXT_H

#include <stddef.h>

/* The next http:// or https:// address in `p`: its length, and *at its
   start; 0 when there is none. On entry *at is the start of the whole text
   (an address must begin there or after a character that cannot be part of
   a word). In running text an address ends where a sentence would: a
   final . , : ; ! ? or ' is left out, as is a closing bracket it does not
   open. An address written as <address> is taken whole, up to the '>',
   as RFC 3986 (appendix C) suggests for text. Other schemes are not
   addresses here. Fields that hold nothing but an address go through
   lk_exact instead: no guessing there. */
size_t lk_next_url(const char *p, const char **at);

/* 1 when `field`, taken whole and as it is, is an http:// or https://
   address: something after the scheme, nothing but printable ASCII other
   than space. A metadata field that is an address is used like this,
   never cut or trimmed. */
int lk_exact(const char *field);

/* `url` as shown in at most `max` characters: its start, "...", its end.
   Never what is opened or copied. */
void lk_label(const char *url, char *out, size_t n, int max);

/* 1 when the address holds a character that would act if it reached a
   Shell or ARexx command line: OpenURL puts the address into one unquoted
   (to start a browser, or to tell a running one). Such an address can be
   copied, not opened. */
int lk_unsafe(const char *url);

/* OpenURL starts a browser with a Shell command line that holds the
   address, and the AROS Shell takes at most 512 characters of one
   (workbench/c/shell/state.h, LINE_MAX). A longer line fails after
   OpenURL has already answered yes: a 1500-character address never
   reached the browser (AROS 2026.09). So longer addresses, with room for
   the browser's own name, are copied, not opened. */
#define LK_OPEN_MAX 480

/* Whether `url` may be handed to a browser: LK_OPEN_OK, or why not. */
enum { LK_OPEN_OK, LK_OPEN_CHARS, LK_OPEN_LONG };
int lk_openable(const char *url);

/* A text and its links, built piece by piece, for a text view. A span of
   bold text and a link never overlap, and pieces stay in text order. Start
   from {0}; lk_doc_free at the end. A failed allocation sets `failed` and
   leaves the rest undone: such a doc is not shown. */
typedef struct {
    size_t start, len;
    int bold;
    char *target;              /* own copy, exact; NULL: bold text only */
} lk_piece;

typedef struct {
    char *text;
    size_t n, cap;
    lk_piece *p;
    int np, pcap;
    int failed;
} lk_doc;

/* An address from a metadata field: used exactly (lk_exact), shown as
   `label`, or shortened to the view's width when label is NULL. A field that
   is not an address is plain text. */
typedef struct { const char *label; const char *url; } lk_field;

void lk_doc_text(lk_doc *d, const char *s);
void lk_doc_link(lk_doc *d, const char *label, const char *target, size_t target_len, int bold);
void lk_doc_field(lk_doc *d, const lk_field *f, int max, int bold);
/* Running text: \33b and \33n turn bold on and off as in Floattext, other
   \33x are dropped, addresses become links as lk_next_url finds them (shown
   shortened past `max` characters), and a character 1..8 stands for
   fields[0..7]: that is how a field keeps its exact address in a sentence. */
void lk_doc_markup(lk_doc *d, const char *text, int max, const lk_field *fields, int nf);
/* A common inspection or row report: lines of complete fields. A
   "Source repository: " or "repository " line holds one exact field (an
   address keeps its final punctuation); a line starting with '{' is raw
   descriptor JSON, shown as it is and not searched for addresses; every
   other line is running text (lk_doc_markup without fields). */
void lk_doc_report(lk_doc *d, const char *text, int max);
int lk_doc_same(const lk_doc *a, const lk_doc *b);
void lk_doc_free(lk_doc *d);

#endif
