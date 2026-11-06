/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak <https://tomaszstaniak.com>
 * Copyright (c) 2026 John Knipper
 * Adapted from arospkg 0.7.4 for the common clients. */
/* Web addresses in PkgManager; see links.h. */
#include "links.h"
#include "linktext.h"
#include "zk_linktext.h"

/* iffparse and openurl are opened where they are used, by local bases: a
   global base would let the SDK's auto-open make them a start-up
   requirement. */
#define __IFFPARSE_NOLIBBASE__
#define __IFFPARSE_LIBBASE iff_base
#define OPENURL_BASE_NAME ou_base
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <proto/graphics.h>
#include <proto/iffparse.h>
#include <proto/openurl.h>
#include <libraries/mui.h>
#include <libraries/iffparse.h>
#include <dos/dostags.h>
#include <clib/alib_protos.h>
#include <aros/asmcall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) ((ULONG)(a)<<24 | (ULONG)(b)<<16 | (ULONG)(c)<<8 | (ULONG)(d))
#endif

static struct {
    Object *view, *owner;
    int linked;                    /* a Linktext; 0: a Floattext shows the words */
    int max;
    lk_doc shown, coming;
} areas[LK_AREAS];

static lk_setup S;
static struct MsgPort *reply_port;
static int pending;                /* opens handed to a process, not yet replied */
static int class_state;            /* 0 not tried, 1 made, -1 none */
static struct Hook action_hook;

/* The request a link process carries, and its answer. */
typedef struct {
    struct Message m;
    int  result;                   /* LK_R_... */
    char label[64];
    int  area;
    char url[1];                   /* the whole address; the message is allocated to hold it */
} lk_msg;
enum { LK_R_HANDED, LK_R_NOLIB, LK_R_REFUSED };

#define MAX_PENDING 4

static void say(const char *t) { if (S.status) S.status(t); }
#define LOG(...) do { if (S.log) S.log(__VA_ARGS__); } while (0)

/* -------------------------------------------------------------- clipboard */

static int copy_text(const char *s)
{
    struct Library *iff_base = OpenLibrary((CONST_STRPTR)"iffparse.library", 39);
    struct IFFHandle *iff;
    int ok = 0;
    LONG len = (LONG)strlen(s);
    if (!iff_base) return 0;
    if ((iff = AllocIFF())) {
        if ((iff->iff_Stream = (IPTR)OpenClipboard(0))) {
            InitIFFasClip(iff);
            if (!OpenIFF(iff, IFFF_WRITE)) {
                ok = !PushChunk(iff, MAKE_ID('F','T','X','T'), MAKE_ID('F','O','R','M'), IFFSIZE_UNKNOWN)
                     && !PushChunk(iff, 0, MAKE_ID('C','H','R','S'), len)
                     && WriteChunkBytes(iff, (APTR)s, len) == len
                     && !PopChunk(iff)
                     && !PopChunk(iff);
                CloseIFF(iff);
            }
            CloseClipboard((struct ClipboardHandle *)iff->iff_Stream);
        }
        FreeIFF(iff);
    }
    CloseLibrary(iff_base);
    return ok;
}

/* ----------------------------------------------------------------- opening */

/* The link process: hands one address to OpenURL and answers. OpenURL may
   wait seconds for a browser it started to show its port; this process
   waits, not the window. */
static void open_proc(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    lk_msg *m;
    struct Library *ou_base;
    WaitPort(&me->pr_MsgPort);
    m = (lk_msg *)GetMsg(&me->pr_MsgPort);
    ou_base = OpenLibrary((CONST_STRPTR)"openurl.library", 1);
    if (!ou_base) m->result = LK_R_NOLIB;
    else {
        /* No tags: show, bring to front, new window and launch as the user
           set them in Prefs/OpenURL. */
        m->result = URL_OpenA((STRPTR)m->url, NULL) ? LK_R_HANDED : LK_R_REFUSED;
        CloseLibrary(ou_base);
    }
    /* Under Forbid until this process is gone, so the window, which waits
       for this answer before it quits, never unloads code still running. */
    Forbid();
    ReplyMsg(&m->m);
}

static void open_url(int area, const char *url)
{
    lk_msg *m;
    struct Process *p;
    BPTR in, out;
    char line[200], label[48];
    int openable = lk_openable(url);
    lk_label(url, label, sizeof label, 40);
    if (openable != LK_OPEN_OK) {
        snprintf(line, sizeof line, openable == LK_OPEN_LONG
                 ? "not opened: %s is too long for AROS to hand to a browser -- copy it with the right mouse button"
                 : "not opened: %s holds characters a browser start would act on -- copy it with the right mouse button", label);
        say(line);
        LOG("link open refused (%s): %s", openable == LK_OPEN_LONG ? "too long" : "unsafe characters", url);
        return;
    }
    if (!reply_port || pending >= MAX_PENDING) {
        say(reply_port ? "still opening earlier links -- try again in a moment" : "links cannot be opened here -- copy it with the right mouse button");
        LOG("link open not started (%s): %s", reply_port ? "busy" : "no port", url);
        return;
    }
    m = (lk_msg *)AllocVec(sizeof *m + strlen(url), MEMF_PUBLIC | MEMF_CLEAR);
    if (!m) { say("not enough memory to open the link -- copy it with the right mouse button"); return; }
    m->m.mn_ReplyPort = reply_port;
    m->m.mn_Length = sizeof *m;
    m->area = area;
    strcpy(m->url, url);
    snprintf(m->label, sizeof m->label, "%s", label);
    in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    /* No current or home drawer: a browser OpenURL starts from here
       inherits them, and would keep PkgManager's drawer in use. */
    p = CreateNewProcTags(NP_Entry,       (IPTR)open_proc,
                          NP_Name,        (IPTR)"PkgManager link",
                          NP_StackSize,   32768,
                          NP_CurrentDir,  (IPTR)BNULL,
                          NP_HomeDir,     (IPTR)BNULL,
                          NP_Input,       (IPTR)in,
                          NP_Output,      (IPTR)out,
                          NP_CloseInput,  TRUE,
                          NP_CloseOutput, TRUE,
                          TAG_DONE);
    if (!p) {
        if (in) Close(in);
        if (out) Close(out);
        FreeVec(m);
        say("could not start opening the link -- copy it with the right mouse button");
        LOG("link open: no process for %s", url);
        return;
    }
    pending++;
    PutMsg(&p->pr_MsgPort, &m->m);
    snprintf(line, sizeof line, "opening %s ...", label);
    say(line);
    LOG("link open %s", url);
}

static void answered(lk_msg *m, int report)
{
    char line[300];
    pending--;
    LOG("link open %s: %s", m->url, m->result == LK_R_HANDED ? "handed to OpenURL"
        : m->result == LK_R_NOLIB ? "no openurl.library" : "OpenURL refused");
    if (!report) return;
    if (m->result == LK_R_HANDED) {
        /* All OpenURL confirms: a browser took the address or was started
           with it. Whether the page loads is the browser's business. */
        snprintf(line, sizeof line, "passed %s to your browser", m->label);
        say(line);
        return;
    }
    snprintf(line, sizeof line, "could not open %s -- copy it with the right mouse button", m->label);
    say(line);
    {
        Object *w = areas[m->area].owner;
        IPTR open = 0;
        if (w) GetAttr(MUIA_Window_Open, w, &open);
        MUI_Request(_app(areas[LK_STATUS].view), open ? w : NULL, 0, (STRPTR)"PkgManager", (STRPTR)"OK",
                    m->result == LK_R_NOLIB
                    ? (STRPTR)"This address could not be opened:\nopenurl.library is not installed.\n\n%s\n\n"
                              "The link's menu (right mouse button) copies it,\nto paste into your browser."
                    : (STRPTR)"This address could not be opened:\nOpenURL found no browser to pass it to\n"
                              "(see Prefs/OpenURL).\n\n%s\n\n"
                              "The link's menu (right mouse button) copies it,\nto paste into your browser.",
                    (IPTR)m->label);
    }
}

/* ------------------------------------------------------------------ views */

/* The class's hook: on the GUI task, inside the view's event handling. The
   target is valid until this returns, and everything here copies it. */
AROS_UFH3(static IPTR, action_entry,
          AROS_UFHA(struct Hook *, h, A0),
          AROS_UFHA(Object *, view, A2),
          AROS_UFHA(struct MUI_LinktextAction *, a, A1))
{
    AROS_USERFUNC_INIT
    int area, i = LK_STATUS;
    char line[200], label[48];
    (void)h;
    for (area = 0; area < LK_AREAS; area++)
        if (areas[area].view == view) i = area;
    if (a->Action == MUIV_Linktext_Action_Open) {
        open_url(i, (const char *)a->Target);
        return TRUE;
    }
    lk_label((const char *)a->Target, label, sizeof label, 40);
    if (copy_text((const char *)a->Target)) {
        snprintf(line, sizeof line, "copied %s to the clipboard", label);
        LOG("link copy %s: ok", (const char *)a->Target);
    } else {
        snprintf(line, sizeof line, "could not put %s on the clipboard", label);
        LOG("link copy %s: failed", (const char *)a->Target);
    }
    say(line);
    return TRUE;
    AROS_USERFUNC_EXIT
}

Object *lk_view(int area, int lines, int flags, int max)
{
    Object *o = NULL, *view = NULL;
    struct TagItem t[10];
    int n = 0;
    areas[area].max = max;
    if (class_state == 0) {
        class_state = !S.no_view && zk_linktext_init() ? 1 : -1;
        LOG("links: %s", class_state > 0 ? "text views" : "no text view class, plain text");
        action_hook.h_Entry = (HOOKFUNC)action_entry;
    }
    if (class_state > 0) {
        t[n].ti_Tag = MUIA_Linktext_ActionHook; t[n++].ti_Data = (IPTR)&action_hook;
        t[n].ti_Tag = MUIA_Linktext_Lines;      t[n++].ti_Data = lines;
        if (!(flags & LK_V_SCROLL)) { t[n].ti_Tag = MUIA_Linktext_FixHeight; t[n++].ti_Data = TRUE; }
        if (flags & LK_V_FRAME) {
            t[n].ti_Tag = MUIA_Frame;      t[n++].ti_Data = MUIV_Frame_Text;
            t[n].ti_Tag = MUIA_Background; t[n++].ti_Data = (IPTR)MUII_TextBack;
        } else if (!(flags & LK_V_SCROLL)) {
            t[n].ti_Tag = MUIA_Frame;      t[n++].ti_Data = MUIV_Frame_None;
            t[n].ti_Tag = MUIA_Background; t[n++].ti_Data = (IPTR)MUII_WindowBack;
            t[n].ti_Tag = MUIA_CycleChain; t[n++].ti_Data = 0;
        }
        if (flags & LK_V_TINY) { t[n].ti_Tag = MUIA_Font; t[n++].ti_Data = MUIV_Font_Tiny; }
        t[n].ti_Tag = TAG_DONE; t[n].ti_Data = 0;
        o = zk_linktext_new(&view, (flags & LK_V_SCROLL) != 0, t);
        if (o) {
            areas[area].view = view;
            areas[area].linked = 1;
            return o;
        }
        LOG("links: no text view for place %d, plain text", area);
    }
    /* Floattext keeps the pointer it is given: lk_end gives it `shown`. */
    {
        static const char *const heights[] = { "", "\n", "\n\n", "\n\n\n", "\n\n\n\n", "\n\n\n\n\n" };
        o = FloattextObject,
                MUIA_Frame, MUIV_Frame_ReadList,
                MUIA_Floattext_Text, (IPTR)"",
                MUIA_FixHeightTxt, (IPTR)heights[lines > 5 ? 5 : lines > 0 ? lines - 1 : 0],
            End;
        areas[area].view = o;
        areas[area].linked = 0;
    }
    return o;
}

void lk_owner(int area, Object *window) { areas[area].owner = window; }

void lk_begin(int area)
{
    if (area >= 0 && area < LK_AREAS) lk_doc_free(&areas[area].coming);
}

void lk_add(int area, const char *text, const lk_field *fields, int nf)
{
    if (area >= 0 && area < LK_AREAS) lk_doc_markup(&areas[area].coming, text, areas[area].max, fields, nf);
}

void lk_add_plain(int area, const char *text)
{
    if (area >= 0 && area < LK_AREAS) lk_doc_text(&areas[area].coming, text);
}

/* In a plain text view the address itself has to be readable. */
void lk_add_field(int area, const char *label, const char *url)
{
    lk_field f;
    if (area < 0 || area >= LK_AREAS || !url) return;
    f.label = label;
    f.url = url;
    if (areas[area].linked) lk_doc_field(&areas[area].coming, &f, areas[area].max, 0);
    else lk_doc_text(&areas[area].coming, url);
}

static int show_doc(int area)
{
    lk_doc *d = &areas[area].coming;
    struct MUI_LinktextSpan *sp;
    ULONG r;
    int i;
    if (!areas[area].linked) {
        SetAttrs(areas[area].view, MUIA_Floattext_Text, (IPTR)(d->text ? d->text : ""), TAG_DONE);
        return 1;
    }
    if (!(sp = malloc(sizeof *sp * (size_t)(d->np + 1)))) return 0;
    for (i = 0; i < d->np; i++) {
        sp[i].Start = (ULONG)d->p[i].start;
        sp[i].Length = (ULONG)d->p[i].len;
        sp[i].Flags = d->p[i].bold ? MUIV_Linktext_Span_Bold : 0;
        sp[i].Target = (CONST_STRPTR)d->p[i].target;
        sp[i].ID = 0;
    }
    r = DoMethod(areas[area].view, MUIM_Linktext_SetContent, (IPTR)(d->text ? d->text : ""), (ULONG)d->n, (IPTR)sp, (ULONG)d->np);
    free(sp);
    if (r != MUIV_Linktext_Status_OK) LOG("links: place %d refused its text: status %lu", area, (unsigned long)r);
    return r == MUIV_Linktext_Status_OK;
}

void lk_end(int area)
{
    lk_doc none = {0};
    if (area < 0 || area >= LK_AREAS || !areas[area].view) return;
    if (areas[area].coming.failed || (!lk_doc_same(&areas[area].shown, &areas[area].coming) && !show_doc(area))) {
        lk_doc_free(&areas[area].coming);
        if (area != LK_STATUS) say("not enough memory to show that text");
        return;
    }
    if (lk_doc_same(&areas[area].shown, &areas[area].coming)) {
        lk_doc_free(&areas[area].coming);
        return;
    }
    lk_doc_free(&areas[area].shown);
    areas[area].shown = areas[area].coming;
    areas[area].coming = none;
}

void lk_add_report(int area, const char *text)
{
    if (area < 0 || area >= LK_AREAS || !text) return;
    if (areas[area].linked) lk_doc_report(&areas[area].coming, text, areas[area].max);
    else lk_doc_text(&areas[area].coming, text);
}

void lk_show1(int area, const char *text)
{
    lk_begin(area);
    lk_add(area, text, NULL, 0);
    lk_end(area);
}

int lk_init(const lk_setup *s)
{
    S = *s;
    reply_port = CreateMsgPort();
    return reply_port != NULL;
}

ULONG lk_signal(void) { return reply_port ? 1UL << reply_port->mp_SigBit : 0; }

void lk_poll(void)
{
    lk_msg *m;
    if (!reply_port) return;
    while ((m = (lk_msg *)GetMsg(reply_port))) { answered(m, 1); FreeVec(m); }
}

void lk_finish(void)
{
    lk_msg *m;
    if (!reply_port) return;
    while (pending > 0) {
        WaitPort(reply_port);
        while ((m = (lk_msg *)GetMsg(reply_port))) { answered(m, 0); FreeVec(m); }
    }
    DeleteMsgPort(reply_port);
    reply_port = NULL;
}

void lk_free(void)
{
    int a;
    /* The application and every view have already been disposed. */
    if (class_state > 0) zk_linktext_exit();
    class_state = 0;
    for (a = 0; a < LK_AREAS; a++) {
        lk_doc_free(&areas[a].shown);
        lk_doc_free(&areas[a].coming);
    }
}
