/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak <https://tomaszstaniak.com>
 * Copyright (c) 2026 John Knipper
 * Adapted from arospkg 0.7.2 for the common clients. */
/* Web addresses in PkgManager: one way to show, open and copy them.
 *
 * Every http:// or https:// address found in a text the window shows gets a
 * row below that text: the address (shortened on screen when it is long,
 * the full one as bubble help) and a Copy button. Clicking the address hands
 * the full address to openurl.library, which passes it to the browser the
 * user chose in Prefs/OpenURL; that happens in a small process of its own,
 * so the window never waits for a browser to start. Copy puts the full
 * address on the clipboard (unit 0). Other schemes stay text.
 *
 * Urltext.mcc draws the address when the class is installed; otherwise a
 * plain Zune Text does. Neither is required to start: without
 * openurl.library a click says so and Copy still works. */
#ifndef PM_LINKS_H
#define PM_LINKS_H

#include <exec/types.h>
#include <intuition/classusr.h>
#include <stddef.h>
#include "linktext.h"

/* The places with links. Each is a group made by lk_area. */
enum { LK_HOME, LK_RELEASE, LK_DETAIL, LK_CONFIRM, LK_STATUS, LK_CHANNEL, LK_AREAS };
#define LK_ROWS 4                  /* rows shown at a time; Previous/Next turn to the others */

/* Return IDs: LK_ID(area, row) opens, +1 copies; row LK_ROWS is the pager
   (Previous, +1 Next). Kept clear of the window's own IDs. */
#define LK_ID_BASE 2000
#define LK_ID(a, i) (LK_ID_BASE + ((a) * (LK_ROWS + 1) + (i)) * 2)
#define LK_ID_END  LK_ID(LK_AREAS, 0)

typedef struct {
    /* The main window's status line, set as it is: what links report there
       is not searched for addresses again. */
    void (*status)(const char *);
    void (*log)(const char *fmt, ...);      /* --log, or nothing             */
} lk_setup;

/* Once, before the windows are made; 0 when the reply port could not be
   made (links then show and copy, and say they cannot open). */
int lk_init(const lk_setup *s);
/* The group for one place, for the window tree; `label_max` is how many
   characters of an address are shown; `owner` is set later (lk_owner). */
Object *lk_area(int area, int label_max);
void lk_owner(int area, Object *window);
/* After the application object exists: connects the rows' notifications. */
void lk_connect(Object *app);

/* The addresses now shown in `area`, in order and without repeats, however
   many and however long: lk_begin, then lk_text for running text (the
   first n characters, or all with (size_t)-1; see lk_next_url) and lk_url
   for a field that is an address and is used exactly as it is (lk_exact;
   anything else gives no link), then lk_end. Rows that keep their address
   are left alone; none hides the area. */
void lk_begin(int area);
void lk_text(int area, const char *text, size_t n);
void lk_url(int area, const char *field);
void lk_end(int area);
/* The same for running texts only: texts[0..n-1], or one. */
void lk_show(int area, const char *const *texts, int n);
void lk_show1(int area, const char *text);
/* Fusion inspection/row reports retain exact repository fields. */
void lk_report(int area, const char *text);

/* A return ID from the main loop: 1 when it was a link's. */
int lk_event(unsigned long id);
/* The signal of the reply port (0 without one), and what to do when it
   arrives: report finished opens. */
ULONG lk_signal(void);
void lk_poll(void);
/* At quit, after the windows are gone: waits for every open still in
   progress, since its process runs this program's code. */
void lk_finish(void);
/* After lk_finish: the addresses' memory. */
void lk_free(void);

#endif
