/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak <https://tomaszstaniak.com>
 * Copyright (c) 2026 John Knipper
 * Adapted from arospkg 0.7.2 for the common clients. */
/* Web addresses in PkgManager; see links.h. */
#include "links.h"
#include "linktext.h"

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
#include <proto/iffparse.h>
#include <proto/openurl.h>
#include <libraries/mui.h>
#include <libraries/iffparse.h>
#include <dos/dostags.h>
#include <clib/alib_protos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Urltext.mcc's published interface (mui/Urltext_mcc.h, which the Pi SDK
   does not have): the few names used here. */
#define MUIC_Urltext            "Urltext.mcc"
#define MUIA_Urltext_Url        0xFEC900D0ul
#define MUIA_Urltext_Text       0xFEC900D1ul
#define MUIA_Urltext_SetMax     0xFEC900D7ul
#define MUIA_Urltext_DoOpenURL  0xFEC900D8ul
#define MUIA_Urltext_NoMenu     0xFEC900D9ul

#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) ((ULONG)(a)<<24 | (ULONG)(b)<<16 | (ULONG)(c)<<8 | (ULONG)(d))
#endif

typedef struct {
    Object *row, *box, *link, *copy;
    char *url;                     /* the whole address, its own copy; NULL: row unused */
    char label[96];
    char *help;                    /* bubble help: the address, broken into lines */
    int  openable;                 /* LK_OPEN_OK, or why it can only be copied */
} lk_row;

/* Every address of a place, in order: LK_ROWS of them are shown at a time,
   and the pager row below them turns to the others. */
static struct {
    Object *group, *owner;
    Object *pager, *pages, *prev, *next;
    int label_max;
    lk_row r[LK_ROWS];
    lk_list all;                   /* the addresses now listed */
    lk_list coming;                /* being collected between lk_begin and lk_end */
    int page;
    char pages_text[48];
} areas[LK_AREAS];

static lk_setup S;
static Object *app_obj;
static struct MsgPort *reply_port;
static int pending;                /* opens handed to a process, not yet replied */
static int urltext = -1;           /* -1 not tried, 0 class missing, 1 in use */

/* The request a link process carries, and its answer. */
typedef struct {
    struct Message m;
    int  result;                   /* LK_R_... */
    char label[96];
    int  area;
    char url[1];                   /* the whole address; the message is allocated to hold it */
} lk_msg;
enum { LK_R_HANDED, LK_R_NOLIB, LK_R_REFUSED };

#define MAX_PENDING 4

static void say(const char *t) { if (S.status) S.status(t); }
#define LOG(...) do { if (S.log) S.log(__VA_ARGS__); } while (0)

/* ---------------------------------------------------------------- objects */

static Object *new_link(lk_row *r)
{
    Object *o = NULL;
    if (urltext != 0) {
        /* Drawing only: its own opening calls OpenURL in this task (the
           window would wait for a browser to start) and keeps 255
           characters of the address, so both are ours. */
        o = MUI_NewObject(MUIC_Urltext,
                          MUIA_Urltext_Url, (IPTR)r->url,
                          MUIA_Urltext_Text, (IPTR)r->label,
                          MUIA_Urltext_DoOpenURL, FALSE,
                          MUIA_Urltext_NoMenu, TRUE,
                          MUIA_Urltext_SetMax, FALSE,
                          MUIA_ShortHelp, (IPTR)r->help,
                          TAG_DONE);
        if (urltext < 0) {
            urltext = o != NULL;
            LOG("links: %s", o ? "Urltext.mcc draws them" : "Urltext.mcc not available, plain text links");
        }
    }
    if (!o)
        o = TextObject,
                MUIA_Text_Contents, (IPTR)r->label,
                MUIA_Text_PreParse, (IPTR)"\33u",
                MUIA_InputMode, MUIV_InputMode_RelVerify,
                MUIA_ShowSelState, FALSE,
                MUIA_ShortHelp, (IPTR)r->help,
                MUIA_CycleChain, 1,
            End;
    return o;
}

static void notify_link(Object *o, int id)
{
    if (!o || !app_obj) return;
    if (urltext == 1)
        /* Zune's Immediate mode (Urltext's) sets Selected on a press and
           never Pressed. */
        DoMethod(o, MUIM_Notify, MUIA_Selected, TRUE, (IPTR)app_obj, 2, MUIM_Application_ReturnID, id);
    else
        DoMethod(o, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app_obj, 2, MUIM_Application_ReturnID, id);
}

Object *lk_area(int area, int label_max)
{
    Object *g;
    int i;
    areas[area].label_max = label_max;
    g = VGroup, MUIA_ShowMe, FALSE, MUIA_Group_VertSpacing, 1, End;
    if (!g) return NULL;
    for (i = 0; i < LK_ROWS; i++) {
        lk_row *r = &areas[area].r[i];
        r->row = HGroup, MUIA_ShowMe, FALSE,
                /* The link as wide as its text, the rest a spacer: Urltext
                   limits its own width, and a row that cannot grow made
                   the whole window narrow (AROS One 1.3). */
                Child, (r->box = HGroup, MUIA_InnerLeft, 0, MUIA_InnerRight, 0,
                        MUIA_InnerTop, 0, MUIA_InnerBottom, 0, MUIA_Group_Spacing, 0,
                        MUIA_HorizWeight, 0,
                        Child, (r->link = TextObject, MUIA_Text_Contents, (IPTR)"", MUIA_Text_SetMin, FALSE, End),
                    End),
                Child, HSpace(0),
                Child, (r->copy = TextObject,
                        ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_Text_Contents, (IPTR)"Copy",
                        MUIA_Text_PreParse, (IPTR)"\33c",
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_HorizWeight, 0,
                        MUIA_CycleChain, 1,
                        MUIA_ShortHelp, (IPTR)"Put the whole address on the clipboard",
                    End),
            End;
        if (r->row) DoMethod(g, OM_ADDMEMBER, (IPTR)r->row);
        r->url = NULL;
    }
    /* More addresses than rows: which ones are shown, and the way to the
       others. */
    areas[area].pager = HGroup, MUIA_ShowMe, FALSE,
            Child, (areas[area].pages = TextObject, MUIA_Text_Contents, (IPTR)"", MUIA_Text_SetMin, FALSE, End),
            Child, HSpace(0),
            Child, (areas[area].prev = TextObject, ButtonFrame, MUIA_Background, MUII_ButtonBack,
                    MUIA_Text_Contents, (IPTR)"Previous", MUIA_Text_PreParse, (IPTR)"\33c",
                    MUIA_InputMode, MUIV_InputMode_RelVerify, MUIA_HorizWeight, 0, MUIA_CycleChain, 1,
                    MUIA_ShortHelp, (IPTR)"The addresses before these", End),
            Child, (areas[area].next = TextObject, ButtonFrame, MUIA_Background, MUII_ButtonBack,
                    MUIA_Text_Contents, (IPTR)"Next", MUIA_Text_PreParse, (IPTR)"\33c",
                    MUIA_InputMode, MUIV_InputMode_RelVerify, MUIA_HorizWeight, 0, MUIA_CycleChain, 1,
                    MUIA_ShortHelp, (IPTR)"The addresses after these", End),
        End;
    if (areas[area].pager) DoMethod(g, OM_ADDMEMBER, (IPTR)areas[area].pager);
    areas[area].group = g;
    return g;
}

void lk_owner(int area, Object *window) { areas[area].owner = window; }

void lk_connect(Object *app)
{
    int a, i;
    app_obj = app;
    for (a = 0; a < LK_AREAS; a++)
        for (i = 0; i < LK_ROWS; i++) {
            lk_row *r = &areas[a].r[i];
            int id = LK_ID(a, i);
            if (r->copy) DoMethod(r->copy, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app_obj, 2,
                                  MUIM_Application_ReturnID, id + 1);
        }
    for (a = 0; a < LK_AREAS; a++) {
        if (areas[a].prev) DoMethod(areas[a].prev, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app_obj, 2,
                                    MUIM_Application_ReturnID, LK_ID(a, LK_ROWS));
        if (areas[a].next) DoMethod(areas[a].next, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app_obj, 2,
                                    MUIM_Application_ReturnID, LK_ID(a, LK_ROWS) + 1);
    }
}

/* The bubble help: the whole address in lines of at most 64 characters,
   so a long one does not make a bubble wider than the screen. */
static char *help_text(const char *url, int openable)
{
    const char *say_ = openable == LK_OPEN_OK ? "Click to open it in your browser; Copy puts it on the clipboard."
                     : openable == LK_OPEN_LONG ? "This address can only be copied: it is too long for AROS to hand to a browser."
                     : "This address can only be copied: it holds characters a browser start would act on.";
    size_t len = strlen(url), k = 0, i;
    char *h = malloc(len + len / 64 + strlen(say_) + 4);
    if (!h) return NULL;
    for (i = 0; i < len; i++) {
        if (i && i % 64 == 0) h[k++] = '\n';
        h[k++] = url[i];
    }
    h[k++] = '\n';
    strcpy(h + k, say_);
    return h;
}

/* A new address for a row: a new link object, since Urltext takes its
   address only when it is made. Swapped like replace_pic in main.c. */
static void set_row(int area, int i, const char *url)
{
    lk_row *r = &areas[area].r[i];
    Object *o, *old = r->link;
    char *old_url = r->url, *old_help = r->help, old_label[sizeof r->label];
    int old_openable = r->openable;
    char *u = strdup(url);
    if (!u) return;
    memcpy(old_label, r->label, sizeof old_label);
    r->url = u;
    r->openable = lk_openable(r->url);
    lk_label(r->url, r->label, sizeof r->label, areas[area].label_max);
    r->help = help_text(r->url, r->openable);
    o = new_link(r);
    if (!o) {
        /* The old link stays, with what it was made with. */
        free(r->url); free(r->help);
        r->url = old_url; r->help = old_help; r->openable = old_openable;
        memcpy(r->label, old_label, sizeof old_label);
        return;
    }
    DoMethod(r->box, MUIM_Group_InitChange);
    if (old) { DoMethod(r->box, OM_REMMEMBER, (IPTR)old); MUI_DisposeObject(old); }
    DoMethod(r->box, OM_ADDMEMBER, (IPTR)o);
    DoMethod(r->box, MUIM_Group_ExitChange);
    /* Only now: the old object showed them until it went. */
    free(old_url); free(old_help);
    r->link = o;
    notify_link(o, LK_ID(area, i));
}

/* The rows for the current page of an area. */
static void show_page(int area)
{
    int i, first = areas[area].page * LK_ROWS, n = areas[area].all.n;
    for (i = 0; i < LK_ROWS; i++) {
        lk_row *r = &areas[area].r[i];
        int k = first + i;
        if (k < n && (!r->url || strcmp(r->url, areas[area].all.v[k]))) set_row(area, i, areas[area].all.v[k]);
        if (k >= n) { free(r->url); r->url = NULL; }
        SetAttrs(r->row, MUIA_ShowMe, k < n && r->url, TAG_DONE);
    }
    if (areas[area].pager) {
        if (n > LK_ROWS) {
            int last = first + LK_ROWS < n ? first + LK_ROWS : n;
            snprintf(areas[area].pages_text, sizeof areas[area].pages_text, "addresses %d-%d of %d",
                     first + 1, last, n);
            SetAttrs(areas[area].pages, MUIA_Text_Contents, (IPTR)areas[area].pages_text, TAG_DONE);
            SetAttrs(areas[area].prev, MUIA_Disabled, first == 0, TAG_DONE);
            SetAttrs(areas[area].next, MUIA_Disabled, last >= n, TAG_DONE);
        }
        SetAttrs(areas[area].pager, MUIA_ShowMe, n > LK_ROWS, TAG_DONE);
    }
    SetAttrs(areas[area].group, MUIA_ShowMe, n > 0, TAG_DONE);
}

void lk_begin(int area)
{
    if (area >= 0 && area < LK_AREAS) lk_list_free(&areas[area].coming);
}

void lk_text(int area, const char *text, size_t n)
{
    if (area >= 0 && area < LK_AREAS) lk_list_text(&areas[area].coming, text, n);
}

void lk_url(int area, const char *field)
{
    if (area >= 0 && area < LK_AREAS) lk_list_url(&areas[area].coming, field);
}

void lk_end(int area)
{
    lk_list none = {0};
    if (area < 0 || area >= LK_AREAS || !areas[area].group) return;
    if (lk_list_same(&areas[area].all, &areas[area].coming)) {
        /* The same addresses: the page the user turned to stays. */
        lk_list_free(&areas[area].coming);
    } else {
        lk_list_free(&areas[area].all);
        areas[area].all = areas[area].coming;
        areas[area].coming = none;
        areas[area].page = 0;
    }
    show_page(area);
}

void lk_show(int area, const char *const *texts, int n)
{
    int t;
    lk_begin(area);
    for (t = 0; t < n; t++) lk_text(area, texts[t], (size_t)-1);
    lk_end(area);
}

void lk_show1(int area, const char *text)
{
    lk_show(area, &text, 1);
}

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

static void open_url(int area, lk_row *r)
{
    lk_msg *m;
    struct Process *p;
    BPTR in, out;
    char line[200];
    if (r->openable != LK_OPEN_OK) {
        snprintf(line, sizeof line, r->openable == LK_OPEN_LONG
                 ? "not opened: %s is too long for AROS to hand to a browser -- use Copy"
                 : "not opened: %s holds characters a browser start would act on -- use Copy", r->label);
        say(line);
        LOG("link open refused (%s): %s", r->openable == LK_OPEN_LONG ? "too long" : "unsafe characters", r->url);
        return;
    }
    if (!reply_port || pending >= MAX_PENDING) {
        say(reply_port ? "still opening earlier links -- try again in a moment" : "links cannot be opened here -- use Copy");
        LOG("link open not started (%s): %s", reply_port ? "busy" : "no port", r->url);
        return;
    }
    m = (lk_msg *)AllocVec(sizeof *m + strlen(r->url), MEMF_PUBLIC | MEMF_CLEAR);
    if (!m) { say("not enough memory to open the link -- use Copy"); return; }
    m->m.mn_ReplyPort = reply_port;
    m->m.mn_Length = sizeof *m;
    m->area = area;
    strcpy(m->url, r->url);
    snprintf(m->label, sizeof m->label, "%s", r->label);
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
        say("could not start opening the link -- use Copy");
        LOG("link open: no process for %s", r->url);
        return;
    }
    pending++;
    PutMsg(&p->pr_MsgPort, &m->m);
    snprintf(line, sizeof line, "opening %s ...", r->label);
    say(line);
    LOG("link open %s", r->url);
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
    snprintf(line, sizeof line, "could not open %s -- use Copy", m->label);
    say(line);
    {
        Object *w = areas[m->area].owner;
        IPTR open = 0;
        if (w) GetAttr(MUIA_Window_Open, w, &open);
        MUI_Request(app_obj, open ? w : NULL, 0, (STRPTR)"PkgManager", (STRPTR)"OK",
                    m->result == LK_R_NOLIB
                    ? (STRPTR)"This address could not be opened:\nopenurl.library is not installed.\n\n%s\n\n"
                              "Copy puts it on the clipboard, to paste into your browser."
                    : (STRPTR)"This address could not be opened:\nOpenURL found no browser to pass it to\n"
                              "(see Prefs/OpenURL).\n\n%s\n\n"
                              "Copy puts it on the clipboard, to paste into your browser.",
                    (IPTR)m->label);
    }
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
    int a, i;
    for (a = 0; a < LK_AREAS; a++) {
        lk_list_free(&areas[a].all);
        lk_list_free(&areas[a].coming);
        for (i = 0; i < LK_ROWS; i++) {
            free(areas[a].r[i].url); areas[a].r[i].url = NULL;
            free(areas[a].r[i].help); areas[a].r[i].help = NULL;
        }
    }
}

int lk_event(unsigned long id)
{
    int a, i;
    lk_row *r;
    char line[200];
    if (id < LK_ID_BASE || id >= LK_ID_END) return 0;
    id -= LK_ID_BASE;
    a = (int)(id / 2) / (LK_ROWS + 1);
    i = (int)(id / 2) % (LK_ROWS + 1);
    if (i == LK_ROWS) {
        /* Previous or Next. */
        int pages = (areas[a].all.n + LK_ROWS - 1) / LK_ROWS;
        if (id % 2 == 0 && areas[a].page > 0) areas[a].page--;
        else if (id % 2 == 1 && areas[a].page + 1 < pages) areas[a].page++;
        show_page(a);
        return 1;
    }
    r = &areas[a].r[i];
    if (!r->url) return 1;
    if (id % 2 == 0) {
        /* Urltext stays Selected after a press (Zune's Immediate mode) and
           is then drawn in a pen that can be the background's; it is the
           window's active object too. Neither should outlast the click. */
        SetAttrs(r->link, MUIA_NoNotify, TRUE, MUIA_Selected, FALSE, TAG_DONE);
        if (areas[a].owner)
            SetAttrs(areas[a].owner, MUIA_Window_ActiveObject, MUIV_Window_ActiveObject_None, TAG_DONE);
        open_url(a, r);
        return 1;
    }
    if (copy_text(r->url)) {
        snprintf(line, sizeof line, "copied %s to the clipboard", r->label);
        LOG("link copy %s: ok", r->url);
    } else {
        snprintf(line, sizeof line, "could not put %s on the clipboard", r->label);
        LOG("link copy %s: failed", r->url);
    }
    say(line);
    return 1;
}

void lk_report(int area, const char *text)
{
    if (area < 0 || area >= LK_AREAS) return;
    lk_list_report(&areas[area].coming, text);
}
