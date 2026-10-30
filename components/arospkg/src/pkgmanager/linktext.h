/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak <https://tomaszstaniak.com>
 * Copyright (c) 2026 John Knipper
 * Adapted from arospkg 0.7.2 for the common clients. */
/* Web addresses in running text: where they are, how they are shown
 * shortened, and which may be handed to a browser. No AROS calls, so the
 * host tests cover it (tests/test_linktext.c). */
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

/* `in` with every address longer than `max` characters written as its
   label: for one-line texts that would otherwise widen their window. */
void lk_shorten(const char *in, char *out, size_t n, int max);

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

/* The addresses of one place, in order, without repeats, however many and
   however long; each its own copy. Start from {0}. */
typedef struct { char **v; int n, cap; } lk_list;
/* Those in the first n characters of running text ((size_t)-1: all), as
   lk_next_url finds them. */
void lk_list_text(lk_list *l, const char *text, size_t n);
/* A field that is an address, exactly as it is (lk_exact); 0 when it is
   not one, and nothing is added. */
int lk_list_url(lk_list *l, const char *field);
/* Common inspection/row reports: repository fields are exact, and the raw
   descriptor JSON is evidence rather than prose to scan for addresses. */
void lk_list_report(lk_list *l, const char *text);
int lk_list_same(const lk_list *a, const lk_list *b);
void lk_list_free(lk_list *l);

#endif
