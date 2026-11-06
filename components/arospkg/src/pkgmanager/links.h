/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak <https://tomaszstaniak.com>
 * Copyright (c) 2026 John Knipper
 * Adapted from arospkg 0.7.4 for the common clients. */
/* Web addresses in PkgManager: one way to show, open and copy them.
 *
 * Texts are shown by a Linktext view (zunekit, compiled into this program):
 * a link stands where it occurs in its sentence, wraps with the text and
 * scrolls with it. A click hands the full address to openurl.library, which
 * passes it to the browser the user chose in Prefs/OpenURL; that happens in
 * a small process of its own, so the window never waits for a browser to
 * start. The context menu and Amiga+C copy the full address to the clipboard
 * (unit 0), at any length. Other schemes stay text.
 *
 * Without openurl.library a click says so and copying still works. If the
 * view cannot be made, a Floattext shows the same words with the addresses
 * written out, and the program runs. */
#ifndef PM_LINKS_H
#define PM_LINKS_H

#include <exec/types.h>
#include <intuition/classusr.h>
#include <stddef.h>
#include "linktext.h"

/* The places with text. */
/* LK_CHANNEL: the signed-catalogue window's row detail (common clients). */
enum { LK_ABOUT, LK_RELEASE, LK_DETAIL, LK_HEAD, LK_CREDIT, LK_CONFIRM, LK_STATUS, LK_CHANNEL, LK_AREAS };

typedef struct {
    /* The main window's status line, set as it is: what links report there
       is not searched for addresses again. */
    void (*status)(const char *);
    void (*log)(const char *fmt, ...);      /* --log, or nothing             */
    int no_view;                            /* tests: as if the class were missing */
} lk_setup;

/* Once, before the windows are made; 0 when the reply port could not be
   made (links then show and copy, and say they cannot open). */
int lk_init(const lk_setup *s);

/* The view of one place, for the window tree. `lines` is its height;
   LK_V_* say what kind of place it is; `max` is how many characters of an
   address are shown before it is shortened. */
#define LK_V_SCROLL 1     /* a scrollbar beside it, and it stretches     */
#define LK_V_FRAME  2     /* a text frame; without: bare on the window   */
#define LK_V_TINY   4     /* the small font                              */
Object *lk_view(int area, int lines, int flags, int max);
void lk_owner(int area, Object *window);

/* New content, shown at once. lk_begin, then lk_add for text (see
   lk_doc_markup) and lk_add_field for an address from a field, then lk_end.
   The same content again changes nothing, so a text stays scrolled where it
   was. Failing to get memory for it keeps the old text, and says so. */
void lk_begin(int area);
void lk_add(int area, const char *text, const lk_field *fields, int nf);
void lk_add_field(int area, const char *label, const char *url);
/* Text as it is: no addresses looked for. */
void lk_add_plain(int area, const char *text);
/* A common inspection or row report (lk_doc_report): repository fields
   exact, raw descriptor JSON shown but not searched. */
void lk_add_report(int area, const char *text);
void lk_end(int area);
void lk_show1(int area, const char *text);

/* The signal of the reply port (0 without one), and what to do when it
   arrives: report finished opens. */
ULONG lk_signal(void);
void lk_poll(void);
/* At quit, after the windows are gone: waits for every open still in
   progress, since its process runs this program's code. */
void lk_finish(void);
/* After the application is disposed: the class and the texts' memory. */
void lk_free(void);

#endif
