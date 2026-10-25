/* PkgManager -- the Zune front end. Presentation only, as pkg.h says of every
 * front: the rows come from pkg_query/pkg_installed, the work is done by a
 * worker process through libpkg, and the only decisions here are which
 * button is enabled. Pattern after ~/Work/AROS/src/hello2.c. */
#include "job.h"
#include "rexx.h"
#include "rexxcmd.h"
#include "../about.h"
#include "../libpkg/verify.h"
#include "demo.h"
#include "../libpkg/util.h"
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <libraries/mui.h>
#include <clib/alib_protos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <strings.h>

/* PM_TEST_VERSION names a clearly labelled test build, so a self-update
   test can start from a version older than the test release; a release
   never sets it. */
#ifdef PM_TEST_VERSION
#define PM_VERSION PM_TEST_VERSION
#else
#define PM_VERSION "0.6.0"
#endif

enum { ID_SEARCH = 1, ID_INSTALL, ID_REMOVE, ID_UPDATE, ID_CANCEL, ID_SELECT, ID_FILTER,
       ID_UPGRADE, ID_ROLLBACK, ID_PROCEED, ID_DECLINE, ID_ABOUT, ID_ABOUT_CLOSE, ID_OPEN, ID_RUN, ID_VERIFY,
       ID_CHECK, ID_BUY, ID_THUMB0, ID_THUMB1, ID_THUMB2, ID_THUMB3, ID_SELFUPDATE };

static Object *app, *win, *str_search, *cyc_cat, *cyc_state, *lst, *txt_detail,
              *txt_status, *gauge, *btn_install, *btn_remove, *btn_update, *btn_cancel,
              *btn_upgrade, *btn_rollback, *win_confirm, *txt_confirm, *btn_proceed, *btn_decline,
              *win_about, *btn_about, *btn_about_close, *btn_open,
              *txt_catalogue, *btn_check, *txt_release, *btn_selfupdate,
              *btn_run, *btn_verify, *txt_head, *btn_buy,
              *grp_media, *pg_shot, *pic_shot, *txt_noshot, *txt_caption, *grp_thumbs,
              *pic_thumb[DEMO_MAXSHOTS], *txt_provenance, *pg_icon, *pic_icon,
              *box_shot, *box_icon, *box_thumb[DEMO_MAXSHOTS];
static char demo_path[PKG_MAXPATH] = "PROGDIR:Demo/storefront.json";
static int auto_yes;              /* --yes: confirm requesters without asking; for tests */
static pkg_entries rows;          /* the whole query; the list shows a filtered view */
static job cur;
static struct MsgPort *port;      /* the worker's messages arrive here */
static char root[PKG_MAXPATH] = "SYS:Packages";
static char index_path[PKG_MAXPATH];
static const char *index_url = PKG_INDEX_URL;
static int quit_pending;          /* close asked for while a job ran */
static char last_phase[16]; static int last_can_cancel;
static int confirming;            /* a plan is shown and the window waits for the user */
static int rexx_quit;             /* QUIT arrived with nothing running */
/* The shared updates model (pkg_updates in pkg.h), recomputed by every
   refill: after start, every finished job and every search or filter. It
   reads the catalogue and the registry only; no network. */
static pkg_updates upd;
static int start_updates;         /* --updates: open with the Updates filter */

/* Updating arospkg itself (libpkg/selfup.h). The worker finds and stages;
   the window asks and decides; the installation's apkg replaces the files
   after this window has gone, and starts it again. */
static su_job sujob;
static int su_offer;              /* the release check found a newer release */
static int su_confirming;         /* the confirmation shows the update */
static int su_handed_off;         /* apkg has the update: quit now */
static int wb_start;              /* started from the icon: start again from it */
static char log_path[PKG_MAXPATH];
static char demo_arg[PKG_MAXPATH];
static char self_drawer[PKG_MAXPATH];
static char announce_nonce[24];

/* Every operation gets a record and a number, whether the window or a
 * script started it, so a script can follow either. `active` is the one the
 * worker or the confirmation belongs to; the rest are finished. */
#define MAXJOBS 32
static rx_job jobs[MAXJOBS];
static long next_job = 1;
static rx_job *active;

/* WAIT holds the script's message until the job is final. */
#define MAXWAITS 16
static struct { rexx_msg *m; long job; char field[24]; } waits[MAXWAITS];

/* The window's own "a job is out": set when the worker starts, cleared only
 * when its PM_DONE has been handled. Not cur.running, which the worker
 * clears before that message is even sent: in between, a new job could be
 * started and the old outcome would be read as the new job's. */
static int in_flight;
static int buy_showing;           /* the purchase preview is open (demo only) */
/* The purchase preview borrows the confirmation window, so while it is open
   nothing may start that would need that window for a plan. */
static int busy_now(void) { return in_flight || confirming || buy_showing; }

/* --log <file>: every event the window acted on, one line each, for a run
 * that has to prove what the window did rather than show a screenshot of
 * it. A test aid in the spirit of apkg's --progress; off unless asked. */
static FILE *logf;
static void logline(const char *fmt, ...)
{
    va_list ap;
    if (!logf) return;
    va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap);
    fputc('\n', logf); fflush(logf);
}

/* The state selector's entries, in order (state_entries below). */
enum { ST_ALL, ST_INSTALLED, ST_NOT_INSTALLED, ST_UPDATES, ST_RECENT, ST_LAST, ST_DEMO };

/* The news behind the two views, recomputed by every refill that shows one. */
static pkg_news news;
static int news_view;             /* ST_RECENT or ST_LAST while news holds that view */

/* The news row for a list row: the same id and the same build. */
static const pkg_news_row *news_row(const pkg_entry *e)
{
    int i;
    for (i = 0; news_view && i < news.n; i++)
        if (!strcmp(news.rows[i].id, e->id) && !strcmp(news.rows[i].arch, e->arch) && !strcmp(news.rows[i].abi, e->abi))
            return &news.rows[i];
    return NULL;
}

/* ------------------------------------------------------------ list display */

AROS_UFH3S(void, display_func,
    AROS_UFHA(struct Hook *, hook,  A0),
    AROS_UFHA(char **,       array, A2),
    AROS_UFHA(pkg_entry *,   e,     A1))
{
    AROS_USERFUNC_INIT
    (void)hook;
    if (!e) {
        array[0] = (char *)"\33bPackage"; array[1] = (char *)"\33bVersion";
        array[2] = (char *)"\33bState";   array[3] = (char *)"\33bSummary";
    } else {
        const pkg_update_row *u = e->installed ? pkg_updates_find(&upd, e->id) : NULL;
        /* The entry is the catalogue's, so its version is the one on offer.
           For an installed package the column says what is on this machine,
           and where a newer one waits, what it would become. The hook is
           called for one row at a time and MUI draws it before the next, so
           one buffer serves every row. */
        /* Versions are written as everywhere else -- apkg list, outdated,
           show and the detail header: rev_label, "0.4.1-aros2". */
        static char ver[140];
        char a[70], b[70];
        array[0] = e->id; array[1] = e->version;
        if (demo_is_offer(e)) {
            /* The price stands where a version would; the state says what
               it is, so a screenshot of the list cannot be misread. */
            u = NULL;
        } else if (u && u->installed_version[0]) {
            rev_label(a, sizeof a, u->installed_version, u->installed_revision);
            rev_label(b, sizeof b, u->available_version, u->available_revision);
            if (u->newer) snprintf(ver, sizeof ver, "%s -> %s", a, b);
            else snprintf(ver, sizeof ver, "%s", a);
            array[1] = ver;
        } else if (e->installed) {
            rev_label(ver, sizeof ver, e->installed_version, e->installed_revision);
            array[1] = ver;
        } else {
            rev_label(ver, sizeof ver, e->version, e->revision);
            array[1] = ver;
        }
        array[2] = (char *)(demo_is_offer(e) ? "\33bdemo offer" :
                            u && u->state == PKG_UPD_AVAILABLE ? "\33bupdate available" :
                            u && u->state == PKG_UPD_BLOCKED ? "update blocked" :
                            e->installed ? "installed" : (e->ours ? "" : "other ABI"));
        array[3] = e->summary;
        {
            /* In the two news views the state column says what kind of
               news it is, with the catalogue's date. */
            const pkg_news_row *r = demo_is_offer(e) ? NULL : news_row(e);
            static char what[80];
            if (r) {
                /* The date is when the catalogue first published it; an
                   update has no date of its own, so it shows none. */
                const char *d = r->kind == PKG_NEWS_BUILD ? r->first_added :
                                r->kind == PKG_NEWS_UPDATED ? "" : r->app_first_added;
                snprintf(what, sizeof what, "%s%s%s%s", r->kind == PKG_NEWS_BUILD ? "new build" :
                         r->kind == PKG_NEWS_UPDATED ? "updated" :
                         r->kind == PKG_NEWS_RECENT || news_view == ST_RECENT ? "added" :
                         r->returned ? "back" : "new",
                         d[0] ? " " : "", d, e->installed ? ", installed" : "");
                array[2] = what;
            }
        }
    }
    AROS_USERFUNC_EXIT
}
static struct Hook display_hook;

static void status(const char *text)
{
    SetAttrs(txt_status, MUIA_Text_Contents, (IPTR)text, TAG_DONE);
}

static void show_err(const char *what, pkg_status st, const pkg_err *e)
{
    static char buf[640];
    /* As libpkg wrote it; the front does not re-phrase. On one line, because
       the status is a Text object of one line's height. */
    if (st == PKG_OK) snprintf(buf, sizeof buf, "%s", what);
    else snprintf(buf, sizeof buf, "%s: %s%s%s%s%s%s", what, e->summary,
                  e->subject[0] ? " (" : "", e->subject, e->subject[0] ? ")" : "",
                  e->detail[0] ? " -- " : "", e->detail);
    status(buf);
}

/* ---------------------------------------------------------------- filters */

/* The category cycle is built from what the index holds -- the first path
 * component of each category -- so it never offers a choice with nothing in
 * it. Rebuilt on every refill; the chosen name is kept across rebuilds. */
#define MAXCAT 32
static char  cat_names[MAXCAT][32];
static STRPTR cat_entries[MAXCAT + 2];
static char  cat_chosen[32];
/* The fourth entry is relabelled with the count on every refill. */
static char upd_label[32] = "Updates";
/* "Recently added" and "Last refresh" list the shared news model (pkg_news
   in pkg.h), the same answers as apkg recent: the first newest first, the
   second by kind (new, new builds, updated), as apkg lists it. The last,
   "Demo store", is there only when the demo storefront loaded: its offers
   are never mixed into the catalogue's views. */
static STRPTR state_entries[] = { (STRPTR)"All", (STRPTR)"Installed", (STRPTR)"Not installed",
                                  (STRPTR)upd_label, (STRPTR)"Recently added", (STRPTR)"Last refresh",
                                  NULL, NULL };
/* How many of the newest additions "Recently added" lists. */
#define RECENT_ROWS 25

static IPTR state_now(void)
{
    IPTR st = 0;
    GetAttr(MUIA_Cycle_Active, cyc_state, &st);
    return st;
}

static void top_category(const char *cat, char *out, size_t n)
{
    size_t k = strcspn(cat, "/");
    if (k >= n) k = n - 1;
    memcpy(out, cat, k); out[k] = 0;
    if (!out[0]) snprintf(out, n, "%s", "(none)");
}

static void rebuild_categories(const pkg_entries *all)
{
    int i, n = 0, active = 0;
    for (i = 0; i < all->n && n < MAXCAT; i++) {
        char top[32]; int j, seen = 0;
        top_category(all->v[i].category, top, sizeof top);
        for (j = 0; j < n; j++) if (strcmp(cat_names[j], top) == 0) { seen = 1; break; }
        if (!seen) snprintf(cat_names[n++], sizeof cat_names[0], "%s", top);
    }
    /* Alphabetical, so the cycle reads the same whatever the index order. */
    for (i = 1; i < n; i++) {
        char tmp[32]; int j = i;
        snprintf(tmp, sizeof tmp, "%s", cat_names[i]);
        while (j > 0 && strcmp(cat_names[j - 1], tmp) > 0) { memcpy(cat_names[j], cat_names[j - 1], sizeof tmp); j--; }
        memcpy(cat_names[j], tmp, sizeof tmp);
    }
    cat_entries[0] = (STRPTR)"All categories";
    for (i = 0; i < n; i++) {
        cat_entries[i + 1] = (STRPTR)cat_names[i];
        if (strcmp(cat_names[i], cat_chosen) == 0) active = i + 1;
    }
    cat_entries[n + 1] = NULL;
    /* MUIA_NoNotify, because setting the active entry here would otherwise
       fire the same notification a user's choice does, which calls refill,
       which comes back here: measured as a few hundred "filter" events per
       second until the window was closed. The user's choice is the only
       one that should refill. */
    SetAttrs(cyc_cat, MUIA_NoNotify, TRUE, MUIA_Cycle_Entries, (IPTR)cat_entries,
             MUIA_Cycle_Active, active, TAG_DONE);
    if (!active) cat_chosen[0] = 0;
}

static int passes_filters(const pkg_entry *e)
{
    IPTR st = 0;
    if (cat_chosen[0]) {
        char top[32];
        top_category(e->category, top, sizeof top);
        if (strcmp(top, cat_chosen) != 0) return 0;
    }
    GetAttr(MUIA_Cycle_Active, cyc_state, &st);
    if (st == ST_DEMO) return 0;          /* the demo store shows its offers only */
    if (st == ST_RECENT || st == ST_LAST) return news_row(e) != NULL;
    if (st == ST_INSTALLED && !e->installed) return 0;
    if (st == ST_NOT_INSTALLED && e->installed) return 0;
    if (st == ST_UPDATES) {
        const pkg_update_row *u = e->installed ? pkg_updates_find(&upd, e->id) : NULL;
        if (!u || !u->newer) return 0;
    }
    return 1;
}

static int passes_filters_is_demo(void)
{
    IPTR st = 0;
    GetAttr(MUIA_Cycle_Active, cyc_state, &st);
    return st == ST_DEMO;
}

static int passes_filters_is_updates(void)
{
    IPTR st = 0;
    GetAttr(MUIA_Cycle_Active, cyc_state, &st);
    return st == ST_UPDATES;
}

/* ------------------------------------------------------------------ rows */

/* Above the list: how old the catalogue is, and the update count, or the
 * reason there is no count. A missing or unreadable catalogue is said as
 * such, never shown as "0 updates". */
static void show_catalogue(void)
{
    static char line[300];
    IPTR st = 0;
    if (upd.catalogue == PKG_CAT_MISSING)
        snprintf(line, sizeof line, "\33bCatalogue: none yet\33n -- press Update index. Updates cannot be checked.");
    else if (upd.catalogue == PKG_CAT_UNREADABLE)
        snprintf(line, sizeof line, "\33bCatalogue: cannot be read\33n (%s) -- press Update index.", upd.catalogue_error);
    else {
        size_t k = (size_t)snprintf(line, sizeof line, "Catalogue updated %s%s   ",
                                    upd.catalogue_when[0] ? upd.catalogue_when : "(date unknown)",
                                    upd.catalogue_stale ? " \33b(more than two weeks ago)\33n" : "");
        if (!upd.installed) snprintf(line + k, sizeof line - k, "Nothing installed.");
        else if (!upd.newer) snprintf(line + k, sizeof line - k, "Installed packages are up to date.");
        else if (!upd.ready)
            snprintf(line + k, sizeof line - k, "\33b%d update%s\33n, none installable now (see details)",
                     upd.newer, upd.newer == 1 ? "" : "s");
        else if (upd.blocked)
            snprintf(line + k, sizeof line - k, "\33b%d update%s available\33n (%d of them blocked)",
                     upd.newer, upd.newer == 1 ? "" : "s", upd.blocked);
        else
            snprintf(line + k, sizeof line - k, "\33b%d update%s available\33n", upd.newer, upd.newer == 1 ? "" : "s");
    }
    SetAttrs(txt_catalogue, MUIA_Text_Contents, (IPTR)line, TAG_DONE);
    if (upd.catalogue == PKG_CAT_OK && upd.newer) snprintf(upd_label, sizeof upd_label, "Updates (%d)", upd.newer);
    else snprintf(upd_label, sizeof upd_label, "Updates");
    GetAttr(MUIA_Cycle_Active, cyc_state, &st);
    /* NoNotify for the reason given in rebuild_categories. */
    SetAttrs(cyc_state, MUIA_NoNotify, TRUE, MUIA_Cycle_Entries, (IPTR)state_entries,
             MUIA_Cycle_Active, st, TAG_DONE);
}

static void show_detail(void);
static int no_catalogue;          /* the rows are the registry's alone: why (a pkg_status) */
static pkg_err nocat_err;

/* The news for the view in front, from the same calls as apkg recent and
   apkg recent --since-last-update. A last refresh that does not describe
   the catalogue in use lists nothing: its rows belong to another one. */
static pkg_status news_st;
static pkg_err news_err;
static void news_fill(pkg_ctx *c, int have_catalogue)
{
    IPTR stv = state_now();
    pkg_news_free(&news);
    memset(&news, 0, sizeof news);
    news_view = 0; news_st = PKG_OK;
    if (!have_catalogue || (stv != ST_RECENT && stv != ST_LAST)) return;
    memset(&news_err, 0, sizeof news_err);
    if (stv == ST_RECENT) {
        pkg_recent_opts o;
        memset(&o, 0, sizeof o);
        o.limit = RECENT_ROWS; o.builds = 1;
        news_st = pkg_recent(c, index_path, &o, &news, &news_err);
    } else news_st = pkg_news_last(c, index_path, &news, &news_err);
    if (news_st == PKG_OK && (stv == ST_RECENT || (news.recorded && news.current))) news_view = (int)stv;
}

/* The line under the list for the two news views. Missing dates or a
   missing record are said as such, never as "nothing new". */
static void news_status(char *msg, size_t n, int shown)
{
    if (news_st != PKG_OK) { snprintf(msg, n, "%s", news_err.summary); return; }
    if (state_now() == ST_LAST) {
        if (!news.recorded) snprintf(msg, n, "no catalogue update recorded on this machine yet -- press Update index");
        else if (!news.current) snprintf(msg, n, "the catalogue was replaced since the last recorded update -- press Update index");
        else if (news.first) snprintf(msg, n, "update of %s: %s Recently added lists the newest.", news.when, news.first_why);
        else if (!news.n) snprintf(msg, n, "update of %s: nothing new for this machine", news.when);
        else snprintf(msg, n, "update of %s: %d new, %d new build%s, %d updated%s", news.when, news.apps,
                      news.builds, news.builds == 1 ? "" : "s", news.updated, shown < news.n ? " (some filtered out)" : "");
        return;
    }
    if (!news.n && news.unknown_dates)
        snprintf(msg, n, "the catalogue gives no dates for its applications -- press Update index");
    else if (!news.n) snprintf(msg, n, "no dated applications for this machine");
    else {
        size_t k = (size_t)snprintf(msg, n, "the %d most recently added for this machine, newest first", shown);
        if (news.unknown_dates && k < n)
            snprintf(msg + k, n - k, "; %d without a date not listed", news.unknown_dates);
    }
}

/* Re-read the index and the registry, rebuild the category cycle, and show
 * the rows that pass the filters. The old rows stay alive until the list
 * has let go of them. Four outcomes are told apart on purpose: no index yet,
 * an index that cannot be read, an index with nothing in it, and filters
 * that leave nothing -- a window that says "0 packages" for all four sends
 * the user to the wrong fix. */
static void refill(const char *term, const char *keep_id, const char *why)
{
    pkg_ctx *c = NULL; pkg_err e; pkg_status st;
    pkg_entries fresh, all = {0}; int hidden = 0, i, k, again = -1, shown = 0, inst = 0;
    char msg[200];

    memset(&e, 0, sizeof e);
    st = pkg_open(root, PKG_OPEN_READ, &c, &e);
    if (st != PKG_OK) { show_err("cannot open the package root", st, &e); return; }
    st = pkg_query(c, index_path, term, &fresh, &hidden, &e);
    {
        pkg_err e3; memset(&e3, 0, sizeof e3);
        pkg_updates_free(&upd);
        if (pkg_updates_get(c, index_path, 1, &upd, &e3) != PKG_OK) {
            /* Zeroed it would read as an OK catalogue with nothing installed. */
            memset(&upd, 0, sizeof upd);
            upd.catalogue = PKG_CAT_UNREADABLE;
            snprintf(upd.catalogue_error, sizeof upd.catalogue_error, "%s", e3.summary);
        }
    }
    /* The category cycle lists what the whole index holds, not what the
       current search left: a search with no hits used to empty the cycle
       and forget the chosen category with it. */
    if (st == PKG_OK && term && *term) {
        pkg_err e2; memset(&e2, 0, sizeof e2);
        if (pkg_query(c, index_path, NULL, &all, NULL, &e2) != PKG_OK) all.n = 0;
    }
    /* Without a readable catalogue the machine still has what is installed,
       and Run, Open folder, Verify, Remove and Roll back need no catalogue:
       the rows come from the registry alone. */
    no_catalogue = 0;
    if (st != PKG_OK) {
        pkg_err e2; memset(&e2, 0, sizeof e2);
        if (pkg_installed(c, NULL, &fresh, &e2) == PKG_OK) {
            no_catalogue = st;
            nocat_err = e;
            st = PKG_OK;
            if (term && *term) {
                /* The search still applies, over the ids. */
                int a, b = 0;
                for (a = 0; a < fresh.n; a++)
                    if (strstr(fresh.v[a].id, term)) fresh.v[b++] = fresh.v[a];
                fresh.n = b;
            }
        }
    }
    news_fill(c, st == PKG_OK && !no_catalogue);
    pkg_close(c);

    SetAttrs(lst, MUIA_List_Quiet, TRUE, TAG_DONE);
    DoMethod(lst, MUIM_List_Clear);
    pkg_entries_free(&rows);
    if (st == PKG_OK) {
        rows = fresh;
        rebuild_categories(all.n ? &all : &rows);
        pkg_entries_free(&all);
        for (i = 0; i < rows.n; i++) {
            if (rows.v[i].installed) inst++;
            /* The news views list in the news model's order, newest first. */
            if (news_view || !passes_filters(&rows.v[i])) continue;
            DoMethod(lst, MUIM_List_InsertSingle, (IPTR)&rows.v[i], MUIV_List_Insert_Bottom);
            if (keep_id && strcmp(rows.v[i].id, keep_id) == 0) again = shown;
            shown++;
        }
        for (k = 0; news_view && k < news.n; k++)
            for (i = 0; i < rows.n; i++) {
                if (strcmp(rows.v[i].id, news.rows[k].id) || strcmp(rows.v[i].arch, news.rows[k].arch)
                    || strcmp(rows.v[i].abi, news.rows[k].abi) || !passes_filters(&rows.v[i])) continue;
                DoMethod(lst, MUIM_List_InsertSingle, (IPTR)&rows.v[i], MUIV_List_Insert_Bottom);
                if (keep_id && strcmp(rows.v[i].id, keep_id) == 0) again = shown;
                shown++;
                break;
            }
        {
            IPTR stv = 0;
            GetAttr(MUIA_Cycle_Active, cyc_state, &stv);
            if (stv == ST_DEMO)
                for (i = 0; i < demo_noffers(); i++) {
                    demo_offer *o = demo_offer_at(i);
                    DoMethod(lst, MUIM_List_InsertSingle, (IPTR)&o->e, MUIV_List_Insert_Bottom);
                    if (keep_id && strcmp(o->e.id, keep_id) == 0) again = shown;
                    shown++;
                }
        }
        logline("refill (%s) term=\"%s\" rows=%d shown=%d installed=%d keep=%s updates=%d ready=%d blocked=%d catalogue=%s when=%s",
                why, term ? term : "", rows.n, shown, inst, keep_id ? keep_id : "-",
                upd.newer, upd.ready, upd.blocked, pkg_cat_word(upd.catalogue), upd.catalogue_when);
        if (no_catalogue)
            snprintf(msg, sizeof msg, "%d installed package%s shown; %s", shown, shown == 1 ? "" : "s",
                     no_catalogue == PKG_E_NOT_FOUND ? "no catalogue yet -- press Update index"
                                                     : "the catalogue cannot be read -- press Update index");
        else if (passes_filters_is_demo())
            snprintf(msg, sizeof msg, "%s: %d fictional offer%s", demo_banner(), shown, shown == 1 ? "" : "s");
        else if (rows.n == 0 && hidden == 0 && !(term && *term))
            snprintf(msg, sizeof msg, "the index lists no packages");
        else if (state_now() == ST_RECENT || state_now() == ST_LAST)
            news_status(msg, sizeof msg, shown);
        else if (shown == 0 && passes_filters_is_updates())
            snprintf(msg, sizeof msg, "%s", upd.installed ? "no updates for the installed packages" : "nothing is installed");
        else if (shown == 0)
            snprintf(msg, sizeof msg, "no packages match%s%s%s",
                     term && *term ? " \"" : "", term && *term ? term : "", term && *term ? "\"" : "");
        else
            snprintf(msg, sizeof msg, "%d package%s%s%s%s", shown, shown == 1 ? "" : "s",
                     shown < rows.n ? " shown" : "",
                     term && *term ? " matching " : "", term && *term ? term : "");
        if (hidden) {
            size_t n = strlen(msg);
            snprintf(msg + n, sizeof msg - n, "; %d hidden (not for this machine)", hidden);
        }
        status(msg);
    } else if (st == PKG_E_NOT_FOUND) {
        logline("refill (%s): no index", why);
        status("no index yet -- press Update index");
    } else {
        logline("refill (%s): index unreadable: %s", why, e.summary);
        show_err("the index cannot be read", st, &e);
    }
    show_catalogue();
    SetAttrs(lst, MUIA_List_Quiet, FALSE, TAG_DONE);
    /* The list forgets its selection when cleared; the user did not. */
    if (again >= 0) SetAttrs(lst, MUIA_List_Active, again, TAG_DONE);
    show_detail();
}

static pkg_entry *selected(void)
{
    IPTR act = (IPTR)MUIV_List_Active_Off; pkg_entry *e = NULL;
    GetAttr(MUIA_List_Active, lst, &act);
    if ((LONG)act == MUIV_List_Active_Off) return NULL;
    DoMethod(lst, MUIM_List_GetEntry, act, (IPTR)&e);
    return e;
}

/* Refill with the current search term, keeping the current selection. */
static void refill_keep(const char *why)
{
    IPTR t = 0; pkg_entry *e = selected(); char keep[PKG_MAXID] = "";
    if (e) snprintf(keep, sizeof keep, "%s", e->id);
    GetAttr(MUIA_String_Contents, str_search, &t);
    refill((const char *)t, keep[0] ? keep : NULL, why);
}

/* The string gadget acknowledges on Return and, in Zune, when it loses
 * focus to a click elsewhere. Only a changed term is a search; the rest
 * would refill for nothing and overwrite whatever the status line said. */
static char last_term[256];
static void search_acknowledged(void)
{
    IPTR t = 0;
    GetAttr(MUIA_String_Contents, str_search, &t);
    if (strcmp((const char *)t, last_term) == 0) { logline("search acknowledged, unchanged"); return; }
    snprintf(last_term, sizeof last_term, "%s", (const char *)t);
    refill_keep("search");
}

/* The panel a person reads before pressing Install: what it is, what it
 * needs from this machine (probed now, not copied from the index), and how
 * much comes down the wire. */
/* ------------------------------------------------------------ pictures */

/* The picture panel has fixed places (demo.h): a picture is checked against
 * its place before it is handed to the datatype, so one that would not fit,
 * is missing or is not a PNG shows as a line of text instead and never
 * changes the window's layout. Pictures are local files, read on this task;
 * nothing is downloaded. */
/* Two buffers per place, used in turn. Zune's Dtpic keeps the pointer it
   was given and reloads only when the new name differs from the string at
   that pointer: refilling the same buffer made both strings equal, and the
   old picture stayed (seen on AROS One 1.3, 2026-10-12). */
static char shot_name[2][PKG_MAXPATH], icon_name[2][PKG_MAXPATH];
static char thumb_name[DEMO_MAXSHOTS][2][PKG_MAXPATH];
static int shot_flip, icon_flip, thumb_flip[DEMO_MAXSHOTS];
static char caption[200], provenance[220], noshot[200];
static const demo_media *media_now;
static int shot_now;

static int pic_ok(const char *path, int maxw, int maxh, char *why, size_t n)
{
    unsigned char head[26];
    long size = -1;
    LONG got = 0;
    int w, h;
    BPTR fh = path[0] ? Open((CONST_STRPTR)path, MODE_OLDFILE) : BNULL;
    if (fh) {
        got = Read(fh, head, sizeof head);
        if (Seek(fh, 0, OFFSET_END) >= 0) size = Seek(fh, 0, OFFSET_BEGINNING);
        Close(fh);
    }
    return demo_png_check(head, got > 0 ? (size_t)got : 0, size, maxw, maxh, &w, &h, why, n);
}

/* A new picture object for every change. Setting MUIA_Dtpic_Name on a
   shown Dtpic left the old picture on screen in some cases on AROS One 1.3
   (2026-10-12: the offer showed Folio's screenshot, then its thumbnail),
   while a new object always loads its file when it is set up. Each picture
   sits alone in a small group, so it is swapped without touching the
   layout around it. */
static Object *replace_pic(Object *box, Object *old, const char *name, int thumb)
{
    Object *n = MUI_NewObject(MUIC_Dtpic, MUIA_Dtpic_Name, (IPTR)name,
                              thumb ? MUIA_InputMode : TAG_IGNORE, MUIV_InputMode_RelVerify, TAG_DONE);
    if (!n) return old;
    DoMethod(box, MUIM_Group_InitChange);
    if (old) { DoMethod(box, OM_REMMEMBER, (IPTR)old); MUI_DisposeObject(old); }
    DoMethod(box, OM_ADDMEMBER, (IPTR)n);
    DoMethod(box, MUIM_Group_ExitChange);
    if (thumb)
        DoMethod(n, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_THUMB0 + (thumb - 1));
    return n;
}

static void show_shot(int i)
{
    char why[160];
    const demo_media *m = media_now;
    if (!m || i < 0 || i >= m->nshots) return;
    shot_now = i;
    if (pic_ok(m->shots[i].file, DEMO_SHOT_W, DEMO_SHOT_H, why, sizeof why) == 0) {
        shot_flip ^= 1;
        snprintf(shot_name[shot_flip], sizeof shot_name[0], "%s", m->shots[i].file);
        pic_shot = replace_pic(box_shot, pic_shot, shot_name[shot_flip], 0);
        SetAttrs(pg_shot, MUIA_Group_ActivePage, 0, TAG_DONE);
    } else {
        snprintf(noshot, sizeof noshot, "Picture not shown:\n%s", why);
        SetAttrs(txt_noshot, MUIA_Text_Contents, (IPTR)noshot, TAG_DONE);
        SetAttrs(pg_shot, MUIA_Group_ActivePage, 1, TAG_DONE);
        logline("picture %s not shown: %s", m->shots[i].file, why);
    }
    snprintf(caption, sizeof caption, "%s%s (%d of %d)", m->mockup ? "\33bMOCKUP\33n " : "",
             m->shots[i].caption, i + 1, m->nshots);
    SetAttrs(txt_caption, MUIA_Text_Contents, (IPTR)caption, TAG_DONE);
}

/* `icon_file`: the installed program's own icon when it is a PNG icon (as
   Folio's and GrafX2's are), else the demo set's. */
static void show_media(const char *id, const char *icon_file)
{
    char why[160];
    int i;
    media_now = demo_media_for(id);
    if (!media_now) {
        /* No pictures: text only. The program's own icon is still shown
           when it is installed and has a PNG icon. */
        if (icon_file && icon_file[0] && pic_ok(icon_file, DEMO_ICON_W, DEMO_ICON_H, why, sizeof why) == 0) {
            icon_flip ^= 1;
            snprintf(icon_name[icon_flip], sizeof icon_name[0], "%s", icon_file);
            pic_icon = replace_pic(box_icon, pic_icon, icon_name[icon_flip], 0);
            SetAttrs(pg_icon, MUIA_Group_ActivePage, 0, TAG_DONE);
        } else SetAttrs(pg_icon, MUIA_Group_ActivePage, 1, TAG_DONE);
        SetAttrs(grp_media, MUIA_ShowMe, FALSE, TAG_DONE);
        return;
    }
    for (i = 0; i < DEMO_MAXSHOTS; i++) {
        int on = i < media_now->nshots
                 && pic_ok(media_now->shots[i].thumb, DEMO_THUMB_W, DEMO_THUMB_H, why, sizeof why) == 0;
        /* Hidden first, then named, then shown: a shown thumbnail kept its
           old picture after a new name on AROS One 1.3 (the offer showed
           Folio's thumbnail), while a hidden one loads its picture afresh
           when it is shown, in Dtpic's setup. */
        if (on) {
            thumb_flip[i] ^= 1;
            snprintf(thumb_name[i][thumb_flip[i]], sizeof thumb_name[0][0], "%s", media_now->shots[i].thumb);
            pic_thumb[i] = replace_pic(box_thumb[i], pic_thumb[i], thumb_name[i][thumb_flip[i]], i + 1);
        }
        SetAttrs(box_thumb[i], MUIA_ShowMe, on, TAG_DONE);
    }
    snprintf(provenance, sizeof provenance, "%s", media_now->provenance);
    SetAttrs(txt_provenance, MUIA_Text_Contents, (IPTR)provenance, TAG_DONE);
    if (icon_file && icon_file[0] && pic_ok(icon_file, DEMO_ICON_W, DEMO_ICON_H, why, sizeof why) == 0) {
        icon_flip ^= 1;
        snprintf(icon_name[icon_flip], sizeof icon_name[0], "%s", icon_file);
        pic_icon = replace_pic(box_icon, pic_icon, icon_name[icon_flip], 0);
        SetAttrs(pg_icon, MUIA_Group_ActivePage, 0, TAG_DONE);
    } else if (media_now->icon[0] && pic_ok(media_now->icon, DEMO_ICON_W, DEMO_ICON_H, why, sizeof why) == 0) {
        icon_flip ^= 1;
        snprintf(icon_name[icon_flip], sizeof icon_name[0], "%s", media_now->icon);
        pic_icon = replace_pic(box_icon, pic_icon, icon_name[icon_flip], 0);
        SetAttrs(pg_icon, MUIA_Group_ActivePage, 0, TAG_DONE);
    } else {
        SetAttrs(pg_icon, MUIA_Group_ActivePage, 1, TAG_DONE);
    }
    if (media_now->nshots) show_shot(0);
    else {
        snprintf(noshot, sizeof noshot, "No screenshots for this application.");
        SetAttrs(txt_noshot, MUIA_Text_Contents, (IPTR)noshot, TAG_DONE);
        SetAttrs(pg_shot, MUIA_Group_ActivePage, 1, TAG_DONE);
        SetAttrs(txt_caption, MUIA_Text_Contents, (IPTR)"", TAG_DONE);
    }
    SetAttrs(grp_media, MUIA_ShowMe, TRUE, TAG_DONE);
}

/* The purchase preview: what buying could look like, said to be a demo in
   its first line, with nothing to fill in and only a Close button. It
   reuses the confirmation window, without Proceed. */
static void buy_preview(const demo_offer *o)
{
    static char text[1400];
    snprintf(text, sizeof text,
             "\33b%s\33n\n"
             "Nothing is bought, installed or sent from here.\n\n"
             "%s\n%s %s, sold by %s\n\n"
             "How buying could work in a later version:\n"
             "1. PkgManager shows the seller, the price and the licence terms.\n"
             "2. You pay on the seller's own web page, in your browser.\n"
             "   PkgManager would never ask for card or account details.\n"
             "3. The seller gives you a licence or a download; PkgManager then\n"
             "   installs the package as it installs any other.\n\n"
             "This window asks for nothing and has no way to pay.",
             demo_banner(), o->name, o->price, o->currency, o->seller);
    buy_showing = 1;
    SetAttrs(btn_proceed, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(btn_decline, MUIA_Text_Contents, (IPTR)"Close", TAG_DONE);
    SetAttrs(win_confirm, MUIA_Window_Title, (IPTR)"PkgManager: demo purchase", MUIA_Window_Open, TRUE, TAG_DONE);
    SetAttrs(txt_confirm, MUIA_Floattext_Text, (IPTR)text, TAG_DONE);
    logline("demo purchase preview for %s (%s %s); nothing bought", o->e.id, o->price, o->currency);
    status(demo_banner());
}

static void buy_close(void)
{
    buy_showing = 0;
    SetAttrs(win_confirm, MUIA_Window_Open, FALSE, MUIA_Window_Title, (IPTR)"PkgManager: confirm", TAG_DONE);
    SetAttrs(btn_proceed, MUIA_ShowMe, TRUE, TAG_DONE);
    SetAttrs(btn_decline, MUIA_Text_Contents, (IPTR)"Cancel", TAG_DONE);
    logline("demo purchase preview closed");
    show_detail();
}

/* The detail pane for a demo offer: what it is, first, and that it is not
   a package. */
static void show_offer(const demo_offer *o)
{
    static char head[400], body[1200];
    snprintf(head, sizeof head, "\33b%s\33n   %s %s\n\33b%s\33n\nNot in the catalogue; it cannot be installed.",
             o->name, o->price, o->currency, demo_banner());
    snprintf(body, sizeof body, "%s\n\nSeller: %s\nPrice: %s %s (fictional)\nCategory: %s\n\n%s",
             o->e.summary, o->seller, o->price, o->currency, o->e.category, o->description);
    SetAttrs(txt_head, MUIA_Text_Contents, (IPTR)head, TAG_DONE);
    SetAttrs(txt_detail, MUIA_Floattext_Text, (IPTR)body, TAG_DONE);
    SetAttrs(btn_install,  MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_remove,   MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_upgrade,  MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_rollback, MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_open,     MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_run,      MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_verify,   MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_buy,      MUIA_Disabled, busy_now(), TAG_DONE);
    show_media(o->e.id, NULL);
}

/* The last Verify, kept for the package it was made for and shown in its
   detail pane until another package is verified or anything is changed. */
static struct { char id[PKG_MAXID]; char text[2048]; } last_verify;

static void verify_store(const char *id, pkg_status st, const pkg_verify_result *v,
                         const char *listing, const char *why)
{
    size_t k;
    snprintf(last_verify.id, sizeof last_verify.id, "%s", id);
    if (st != PKG_OK) {
        snprintf(last_verify.text, sizeof last_verify.text, "\33bVerify:\33n could not check -- %s\n", why);
        return;
    }
    k = (size_t)snprintf(last_verify.text, sizeof last_verify.text,
                         "\33bVerify:\33n %s\n%d file%s as installed, %d changed locally, %d missing, "
                         "%d in the drawer that are not the package's; drawer icon: %s\n",
                         v->changed || v->missing ? "differences found" : "every file is as installed",
                         v->ok, v->ok == 1 ? "" : "s", v->changed, v->missing, v->unknown, v->icon);
    /* Only the lines that need attention; "ok" lines would bury them. */
    if (listing) {
        const char *p = listing;
        int shown = 0;
        while (*p && k + 80 < sizeof last_verify.text) {
            size_t n = strcspn(p, "\n");
            if (strncmp(p, "ok", 2) != 0 && shown < 12) {
                k += (size_t)snprintf(last_verify.text + k, sizeof last_verify.text - k, "  %.*s\n", (int)n, p);
                shown++;
            }
            p += n; if (*p) p++;
        }
    }
}

static void show_detail(void)
{
    static char buf[6144], head[700];
    char icon_file[PKG_MAXPATH] = "";
    int run_ready;
    char reqs[1024] = "";
    pkg_entry *e = selected();
    pkg_details det;
    size_t k;

    if (!e) {
        SetAttrs(txt_detail, MUIA_Floattext_Text, (IPTR)"", TAG_DONE);
        SetAttrs(btn_install,  MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(btn_remove,   MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(btn_upgrade,  MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(btn_rollback, MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(btn_open,     MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(btn_run,      MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(btn_verify,   MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(btn_buy,      MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(txt_head, MUIA_Text_Contents, (IPTR)"", TAG_DONE);
        SetAttrs(grp_media, MUIA_ShowMe, FALSE, TAG_DONE);
        SetAttrs(pg_icon, MUIA_Group_ActivePage, 1, TAG_DONE);
        return;
    }
    if (demo_is_offer(e)) { show_offer(demo_offer_of(e)); return; }
    SetAttrs(btn_buy, MUIA_Disabled, TRUE, TAG_DONE);
    memset(&det, 0, sizeof det);
    {
        /* Whether it runs here comes from the same call `apkg show` and
           ARexx INFO make, so the three cannot disagree. */
        pkg_ctx *c = NULL; pkg_err err; memset(&err, 0, sizeof err);
        if (pkg_open(root, PKG_OPEN_READ, &c, &err) == PKG_OK) {
            if (pkg_details_get(c, index_path, e->id, &det, &err) != PKG_OK) {
                det.compat = PKG_COMPAT_UNDETERMINED;
                snprintf(det.compat_why, sizeof det.compat_why, "%s", err.summary);
            }
            if (e->requires[0] && pkg_requirements(c, index_path, e->id, reqs, sizeof reqs, &err) != PKG_OK)
                snprintf(reqs, sizeof reqs, "%s", err.summary);
            pkg_close(c);
        }
    }
    {
        /* The revision is shown only when the index states one: "2.2" is
           upstream's string and must appear exactly as upstream wrote it. */
        /* The header names what is ON THIS MACHINE when the package is
           installed -- the registry's version and revision -- and the
           index's otherwise. Showing the index's revision for an installed
           package read as "2.2-aros2 installed" while revision 0 was what
           had been rolled back to. */
        char ver[80];
        rev_label(ver, sizeof ver, e->installed ? e->installed_version : e->version,
                  e->installed ? e->installed_revision : e->revision);
        /* The header stays in view whatever the pane below scrolls to, and
           whatever the picture panel beside it holds: what it is, whether
           it is installed, whether it runs here and how it starts. */
        snprintf(head, sizeof head, "\33b%s\33n %s   %s\n", e->id, ver,
                 e->installed ? "\33binstalled\33n" : (e->ours ? "not installed" : "\33bother ABI\33n"));
        buf[0] = 0; k = 0;
        /* Where it is, from its registry entry: what Open folder opens. */
        if (e->installed && det.installed_dir[0])
            k += (size_t)snprintf(buf + k, sizeof buf - k, "location: %s\n", det.installed_dir);
    }
    /* Whether Run can start it, from the same call as `apkg run --check`:
       the drawer's own description, checked on the disk now. */
    run_ready = 0;
    if (e->installed) {
        size_t k2;
        pkg_app_info ai;
        pkg_ctx *c = NULL; pkg_err err; memset(&err, 0, sizeof err);
        memset(&ai, 0, sizeof ai);
        if (pkg_open(root, PKG_OPEN_READ, &c, &err) == PKG_OK) {
            if (pkg_app_info_get(c, e->id, &ai, &err) == PKG_OK) {
                run_ready = ai.state == PKG_RUN_READY;
                if (ai.program[0]) snprintf(icon_file, sizeof icon_file, "%s.info", ai.program);
                k2 = strlen(head);
                if (run_ready && ai.launch.mode == PKG_LAUNCH_WORKBENCH && ai.stack > 0)
                    snprintf(head + k2, sizeof head - k2, "start: from its icon, stack %ld bytes\n", ai.stack);
                else if (run_ready && ai.launch.mode == PKG_LAUNCH_WORKBENCH)
                    snprintf(head + k2, sizeof head - k2, "start: from its icon\n");
                else if (run_ready)
                    snprintf(head + k2, sizeof head - k2, "start: as a Shell command, in a window of its own\n");
                else if (ai.state == PKG_RUN_NO_DESCRIPTION)
                    snprintf(head + k2, sizeof head - k2, "start: not described by the package -- use Open folder\n");
                else
                    snprintf(head + k2, sizeof head - k2, "\33bcannot start:\33n %s\n", ai.why);
                logline("detail %s: run %s%s%s", e->id, pkg_run_word(ai.state), run_ready ? "" : " why=", run_ready ? "" : ai.why);
            }
            pkg_close(c);
        }
    } else if (det.in_index && det.launch.mode != PKG_LAUNCH_NONE) {
        size_t k2 = strlen(head);
        snprintf(head + k2, sizeof head - k2, "start: %s, once installed\n",
                 det.launch.mode == PKG_LAUNCH_WORKBENCH ? "from its icon" : "as a Shell command");
    }
    {
        /* The two questions the buttons need, answered by the library. */
        char why[240]; int up = 0, back = 0;
        if (e->installed) {
            /* The update state from the shared model: the same answer as
               the list, apkg outdated and the ScreenBar widget. */
            const pkg_update_row *u = pkg_updates_find(&upd, e->id);
            size_t n2 = strlen(buf);
            char a[80], b[80];
            up = u && u->state == PKG_UPD_AVAILABLE;
            if (!u) snprintf(why, sizeof why, "not known");
            else snprintf(why, sizeof why, "%s", u->why);
            if (u) {
                rev_label(a, sizeof a, u->installed_version, u->installed_revision);
                if (u->available_version[0]) rev_label(b, sizeof b, u->available_version, u->available_revision);
                else snprintf(b, sizeof b, "-");
                if (u->newer)
                    snprintf(buf + n2, sizeof buf - n2, "\33bupdate %s:\33n %s -> %s%s%s\n",
                             up ? "available" : "blocked", a, b, up ? "" : " -- ", up ? "" : u->why);
                else if (u->state != PKG_UPD_CURRENT)
                    snprintf(buf + n2, sizeof buf - n2, "installed %s, catalogue %s -- %s\n", a, b, u->why);
            }
            {
                pkg_ctx *c = NULL; pkg_err err; memset(&err, 0, sizeof err);
                if (pkg_open(root, PKG_OPEN_READ, &c, &err) == PKG_OK) {
                    back = pkg_can_rollback(c, e->id, why, sizeof why);
                    pkg_close(c);
                }
                /* An upgrade happened but the way back is gone: say so where
                   the greyed button is, rather than leave it unexplained. */
                if (!back && strstr(why, "archive")) {
                    size_t n2 = strlen(buf);
                    snprintf(buf + n2, sizeof buf - n2, "\33brollback unavailable:\33n %s\n", why);
                }
            }
            {
                char a[80], b[80];
                rev_label(a, sizeof a, e->installed_version, e->installed_revision);
                rev_label(b, sizeof b, e->version, e->revision);
                logline("detail %s: installed %s, index %s, can_upgrade=%d can_rollback=%d%s%s",
                        e->id, a, b, up, back, (!up || !back) ? " why=" : "", (!up || !back) ? why : "");
            }
        }
        SetAttrs(btn_upgrade,  MUIA_Disabled, busy_now() || !up,   TAG_DONE);
        SetAttrs(btn_rollback, MUIA_Disabled, busy_now() || !back, TAG_DONE);
    }
    k = strlen(buf);
    k += (size_t)snprintf(buf + k, sizeof buf - k, "%s\n%s   %s/%s   %s   %ld KB download\n",
                          e->summary, e->category, e->arch, e->abi[0] ? e->abi : "?", e->kind,
                          e->size > 0 ? (e->size + 1023) / 1024 : 0L);
    /* Right after the description: "needs the game data" is what decides
       whether to install, and the pane shows eight lines without scrolling. */
    k = strlen(buf);
    /* The package's notes: the catalogue's current text, or the copy kept
       at installation when the catalogue no longer has it. They are plain
       printable ASCII (the parser refuses anything else), so they cannot
       carry Floattext's escape codes. */
    if (det.in_index && det.notes_bad)
        snprintf(buf + k, sizeof buf - k, "\33bNotes from the package\33n\nnot shown: they are not plain lines this version can display\n");
    else if (det.in_index ? det.notes[0] : det.notes_recorded[0])
        snprintf(buf + k, sizeof buf - k, "\33b%s\33n\n%s\n",
                 det.in_index ? "Notes from the package" : "Notes recorded at installation",
                 det.in_index ? det.notes : det.notes_recorded);
    {
        size_t k2 = strlen(head);
        if (det.compat == PKG_COMPAT_NATIVE)
            snprintf(head + k2, sizeof head - k2, "compatibility: native");
        else
            snprintf(head + k2, sizeof head - k2, "\33bcompatibility: %s\33n -- %s", pkg_compat_word(det.compat), det.compat_why);
    }
    k = strlen(buf);
    if (det.compat != PKG_COMPAT_NATIVE)
        snprintf(buf + k, sizeof buf - k, "requirements: not checked");
    else if (e->requires[0])
        snprintf(buf + k, sizeof buf - k, "requirements: %s, probed now:\n%s", det.requirements, reqs[0] ? reqs : e->requires);
    else
        snprintf(buf + k, sizeof buf - k, "requirements: none stated");
    if (det.in_index) {
        /* Two separate facts: what the catalogue's automatic checks found
           about these bytes, and who started the program where. */
        char text[600]; int i;
        pkg_review_text(&det, text, sizeof text);
        k = strlen(buf);
        k += (size_t)snprintf(buf + k, sizeof buf - k, "\n\33bstatus:\33n %s\n", text);
        if (!det.ntests && k < sizeof buf)
            k += (size_t)snprintf(buf + k, sizeof buf - k, "runtime tests: none recorded for this archive\n");
        for (i = 0; i < det.ntests && i < PKG_TESTS_MAX && k < sizeof buf; i++)
            k += (size_t)snprintf(buf + k, sizeof buf - k, "runtime test: %s\n", det.tests[i]);
        pkg_added_text(&det, text, sizeof text);
        if (k < sizeof buf)
            k += (size_t)snprintf(buf + k, sizeof buf - k, "added: %s\ncatalogue: %s", text,
                                  det.catalogue[0] ? det.catalogue : "the catalogue file on this machine");
    }
    {
        /* What it needs, recommends, conflicts with and replaces, from the
           same call apkg show makes. */
        pkg_relation *rr = (pkg_relation *)malloc(sizeof *rr * PKG_REL_ROWS);
        int nr = 0, i;
        pkg_ctx *c = NULL; pkg_err err; memset(&err, 0, sizeof err);
        if (rr && pkg_open(root, PKG_OPEN_READ, &c, &err) == PKG_OK) {
            pkg_relations_get(c, index_path, e->id, rr, PKG_REL_ROWS, &nr, &err);
            pkg_close(c);
        }
        for (i = 0; rr && i < nr; i++) {
            char st[240], *hint;
            snprintf(st, sizeof st, "%s", rr[i].state);
            /* The window's way to install it, not the Shell's. */
            if ((hint = strstr(st, " (apkg install ")) != NULL) snprintf(hint, sizeof st - (size_t)(hint - st), " (select it and press Install)");
            k = strlen(buf);
            if (k < sizeof buf)
                snprintf(buf + k, sizeof buf - k, "\n\33b%s\33n %s%s%s%s%s: %s",
                         rr[i].kind == PKG_REL_DEPENDS ? "needs" : rr[i].kind == PKG_REL_RECOMMENDS ? "recommends" :
                         rr[i].kind == PKG_REL_CONFLICTS ? "conflicts with" : rr[i].kind == PKG_REL_REPLACES ? "replaces" :
                         "needed by",
                         rr[i].id, rr[i].min_version[0] ? " " : "", rr[i].min_version[0] ? ">= " : "", rr[i].min_version,
                         rr[i].reason[0] ? "" : "", st);
            if (rr[i].kind == PKG_REL_RECOMMENDS && rr[i].reason[0] && (k = strlen(buf)) < sizeof buf)
                snprintf(buf + k, sizeof buf - k, "\n  why: %s", rr[i].reason);
        }
        free(rr);
    }
    if (e->installed && !strcmp(last_verify.id, e->id)) {
        /* First, where it is seen without scrolling: it answers the button
           that was just pressed. */
        size_t vl = strlen(last_verify.text), bl = strlen(buf);
        if (vl + bl + 2 < sizeof buf) {
            memmove(buf + vl + 1, buf, bl + 1);
            memcpy(buf, last_verify.text, vl);
            buf[vl] = '\n';
        }
    }
    SetAttrs(txt_head, MUIA_Text_Contents, (IPTR)head, TAG_DONE);
    SetAttrs(txt_detail, MUIA_Floattext_Text, (IPTR)buf, TAG_DONE);
    show_media(e->id, icon_file);
    SetAttrs(btn_open, MUIA_Disabled, busy_now() || !e->installed, TAG_DONE);
    SetAttrs(btn_run, MUIA_Disabled, busy_now() || !run_ready, TAG_DONE);
    SetAttrs(btn_verify, MUIA_Disabled, busy_now() || !e->installed, TAG_DONE);
    SetAttrs(btn_install, MUIA_Disabled, busy_now() || e->installed || !e->ours, TAG_DONE);
    SetAttrs(btn_remove,  MUIA_Disabled, busy_now() || !e->installed, TAG_DONE);
}

/* Open folder: the drawer the registry entry records, through the same call
   as `apkg open`. It reads only; the registry and the drawer are not touched. */
static void open_folder(const char *id)
{
    char where[PKG_MAXPATH], line[PKG_MAXPATH + 200];
    pkg_ctx *c = NULL; pkg_err err; pkg_status st;
    memset(&err, 0, sizeof err);
    where[0] = 0;
    st = pkg_open(root, PKG_OPEN_READ, &c, &err);
    if (st == PKG_OK) { st = pkg_open_folder(c, id, where, sizeof where, &err); pkg_close(c); }
    if (st == PKG_OK) snprintf(line, sizeof line, "opened %s", where);
    else snprintf(line, sizeof line, "could not open the folder of %s: %s%s%s", id, err.summary,
                  where[0] ? " -- " : "", where);
    logline("open folder %s: %s", id, line);
    status(line);
}

/* ------------------------------------------------------------------ jobs */

static void busy(int on)
{
    SetAttrs(btn_update,  MUIA_Disabled, on, TAG_DONE);
    SetAttrs(btn_check,   MUIA_Disabled, on, TAG_DONE);
    SetAttrs(btn_selfupdate, MUIA_Disabled, on || !su_offer, TAG_DONE);
    SetAttrs(btn_cancel,  MUIA_Disabled, TRUE, TAG_DONE);   /* until an event says otherwise */
    SetAttrs(gauge, MUIA_Gauge_Current, 0, MUIA_Gauge_InfoText, (IPTR)(on ? "starting" : ""), TAG_DONE);
    if (on) {
        SetAttrs(btn_install,  MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(btn_remove,   MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(btn_upgrade,  MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(btn_rollback, MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(btn_open,     MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(btn_run,      MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(btn_verify,   MUIA_Disabled, TRUE, TAG_DONE);
    } else show_detail();
}

static const char *opname(job_kind k)
{
    switch (k) {
    case JOB_INSTALL: return "install";
    case JOB_REMOVE:  return "remove";
    case JOB_UPDATE:  return "update";
    case JOB_PREVIEW_UPGRADE: case JOB_UPGRADE: return "upgrade";
    case JOB_PREVIEW_ROLLBACK: case JOB_ROLLBACK: return "rollback";
    case JOB_CHECK_RELEASE: return "check";
    case JOB_VERIFY: return "verify";
    case JOB_RUN: return "run";
    case JOB_SU_LOCATE: case JOB_SU_STAGE: return "self-update";
    default: return "none";
    }
}

/* 0 means the latest. Only the last MAXJOBS are kept. */
static rx_job *find_job(long id)
{
    rx_job *j;
    if (id <= 0) id = next_job - 1;
    if (id <= 0 || id >= next_job || id < next_job - MAXJOBS) return NULL;
    j = &jobs[(id - 1) % MAXJOBS];
    return j->id == id ? j : NULL;
}

static void err_text(char *out, size_t n, const pkg_err *e)
{
    snprintf(out, n, "%s%s%s%s%s%s", e->summary,
             e->subject[0] ? " (" : "", e->subject, e->subject[0] ? ")" : "",
             e->detail[0] ? " -- " : "", e->detail);
}

static void reply_job(rexx_msg *m, const rx_job *j, const char *field);

/* A job becomes final exactly once, here; every script waiting on it is
 * answered with the outcome. */
static void finish(rx_job *j, rx_jobstate st, const char *error, const char *message)
{
    int i;
    j->state = st;
    j->can_cancel = 0;
    snprintf(j->error, sizeof j->error, "%s", error ? error : "");
    snprintf(j->message, sizeof j->message, "%s", message ? message : "");
    logline("job %ld %s %s %s%s%s%s%s", j->id, j->op, j->package[0] ? j->package : "-",
            rx_statename(st), j->error[0] ? " " : "", j->error, j->message[0] ? ": " : "", j->message);
    for (i = 0; i < MAXWAITS; i++)
        if (waits[i].m && waits[i].job == j->id) {
            rexx_msg *m = waits[i].m;
            waits[i].m = NULL;
            reply_job(m, j, waits[i].field);
        }
    if (j == active) active = NULL;
}

static int install_confirmed;     /* the plan in the confirmation window was accepted */
static char install_sig[700];     /* that plan's steps (plan_signature) */

static int start(job_kind kind, const char *id)
{
    if (in_flight) return -1;
    memset(&cur, 0, sizeof cur);
    cur.kind = kind;
    cur.confirmed = kind == JOB_INSTALL && install_confirmed;
    if (cur.confirmed) snprintf(cur.plan_sig, sizeof cur.plan_sig, "%s", install_sig);
    install_confirmed = 0;
    cur.seq = active ? active->id : 0;
    snprintf(cur.id, sizeof cur.id, "%s", id ? id : "");
    snprintf(cur.root, sizeof cur.root, "%s", root);
    snprintf(cur.index, sizeof cur.index, "%s", index_path);
    snprintf(cur.index_url, sizeof cur.index_url, "%s", index_url);
    snprintf(cur.current, sizeof cur.current, "%s", PM_VERSION);
    cur.reply = port;
    cur.su = &sujob;
    last_phase[0] = 0; last_can_cancel = 0;
    busy(1);
    status(kind == JOB_INSTALL ? "installing..." : kind == JOB_REMOVE ? "removing..." :
           kind == JOB_UPDATE ? "updating the index..." :
           kind == JOB_UPGRADE ? "upgrading..." : kind == JOB_ROLLBACK ? "rolling back..." :
           kind == JOB_PREVIEW_UPGRADE ? "working out what the upgrade would do..." :
           kind == JOB_CHECK_RELEASE ? "asking for the latest release..." :
           kind == JOB_VERIFY ? "checking the installed files..." :
           kind == JOB_RUN ? "starting..." :
           kind == JOB_SU_LOCATE ? "looking at this installation of arospkg..." :
           kind == JOB_SU_STAGE ? "downloading and checking the new arospkg..." :
           "working out what the rollback would do...");
    if (job_start(&cur) != 0) { busy(0); status("could not start the worker process"); return -1; }
    in_flight = 1;
    return 0;
}

/* Every operation starts here, from a button or from a script. */
static int install_gate(rx_job *j);
static void proceed(const char *how);

static rx_job *begin(job_kind kind, const char *pkg, const char *origin)
{
    rx_job *j;
    if (busy_now()) return NULL;
    j = &jobs[(next_job - 1) % MAXJOBS];
    memset(j, 0, sizeof *j);
    j->id = next_job++;
    snprintf(j->op, sizeof j->op, "%s", opname(kind));
    snprintf(j->package, sizeof j->package, "%s", pkg ? pkg : "");
    snprintf(j->origin, sizeof j->origin, "%s", origin);
    j->state = (kind == JOB_PREVIEW_UPGRADE || kind == JOB_PREVIEW_ROLLBACK) ? RXJ_PREVIEW : RXJ_RUNNING;
    active = j;
    logline("job %ld %s %s from %s", j->id, j->op, j->package[0] ? j->package : "-", origin);
    if (kind == JOB_INSTALL && install_gate(j)) return j;
    if (start(kind, pkg) != 0) finish(j, RXJ_FAILED, "CANNOTSTART", "could not start the worker process");
    return j;
}

/* Before an install: its plan, from the same call apkg makes. A refusal
 * ends the job here; a plan that changes more than the package (what it
 * needs, what it replaces) waits in the confirmation window. Returns 1
 * when the job was settled or is waiting, 0 to start it at once. */
static int install_gate(rx_job *j)
{
    static char text[3000];
    pkg_install_plan *pl = (pkg_install_plan *)calloc(1, sizeof *pl);
    pkg_ctx *c = NULL; pkg_err err; pkg_status st;
    size_t k; int i;
    memset(&err, 0, sizeof err);
    if (!pl) return 0;
    if ((st = pkg_open(root, PKG_OPEN_READ, &c, &err)) == PKG_OK) {
        st = pkg_plan_install(c, index_path, j->package, pl, &err);
        pkg_close(c);
    }
    if (st != PKG_OK || !pl->ok) {
        char why[800];
        snprintf(why, sizeof why, "%s", st != PKG_OK ? err.summary : pl->why);
        logline("install %s refused: %s", j->package, why);
        finish(j, RXJ_FAILED, rx_statusword(st != PKG_OK ? st : pl->status), why);
        if (st == PKG_OK) {
            err.status = pl->status; snprintf(err.summary, sizeof err.summary, "cannot install %s", j->package);
            snprintf(err.detail, sizeof err.detail, "%s", pl->why);
        }
        show_err("cannot install", err.status, &err);
        free(pl);
        return 1;
    }
    if (!pl->extra) { free(pl); return 0; }
    k = (size_t)snprintf(text, sizeof text, "Install %s\n\nThis changes more than %s:\n\n", j->package, j->package);
    for (i = 0; i < pl->nops && k < sizeof text; i++) {
        const pkg_plan_op *o = &pl->ops[i];
        char v[80];
        rev_label(v, sizeof v, o->version, o->revision);
        k += (size_t)snprintf(text + k, sizeof text - k, "%d. %s %s %s%s%s\n", i + 1,
                              o->kind == PKG_OP_DEPENDENCY ? "install (needed)" : o->kind == PKG_OP_INSTALL ? "install" :
                              "remove (replaced)", o->id, v, o->note[0] ? "\n     " : "", o->note);
    }
    if (k < sizeof text)
        snprintf(text + k, sizeof text - k, "\nNothing else is removed. Files you changed in a removed package are kept.\n\nProceed?");
    logline("install %s: plan of %d steps waits for confirmation", j->package, pl->nops);
    plan_signature(pl, install_sig, sizeof install_sig);
    free(pl);
    j->state = RXJ_CONFIRM;
    snprintf(j->message, sizeof j->message, "the install plan waits for confirmation");
    confirming = 1;
    status("waiting for your decision");
    if (auto_yes) { proceed("confirmed automatically (--yes)"); return 1; }
    SetAttrs(win_confirm, MUIA_Window_Open, TRUE, TAG_DONE);
    SetAttrs(txt_confirm, MUIA_Floattext_Text, (IPTR)text, TAG_DONE);
    return 1;
}

/* The plan is shown in a window of its own rather than a requester: a
 * requester runs its own loop until it is answered, and the ARexx port --
 * served from this loop -- would stop answering for as long as the user
 * reads. */
static void confirm_close(void)
{
    SetAttrs(win_confirm, MUIA_Window_Open, FALSE, TAG_DONE);
    confirming = 0;
}

static void su_proceed(void);
static void su_decline(const char *how);

static void proceed(const char *how)
{
    rx_job *j = active;
    if (su_confirming) { su_proceed(); return; }
    if (!confirming || !j) return;
    confirm_close();
    logline("%s %s %s", j->op, j->package, how);
    j->state = RXJ_RUNNING;
    j->phase[0] = 0; j->done = j->total = 0; j->can_cancel = 0;
    if (!strcmp(j->op, "install")) install_confirmed = 1;
    if (start(!strcmp(j->op, "install") ? JOB_INSTALL : !strcmp(j->op, "upgrade") ? JOB_UPGRADE : JOB_ROLLBACK,
              j->package) != 0)
        finish(j, RXJ_FAILED, "CANNOTSTART", "could not start the worker process");
}

static void decline(const char *how)
{
    rx_job *j = active;
    if (su_confirming) { su_decline(how); return; }
    if (!confirming || !j) return;
    confirm_close();
    logline("%s %s %s", j->op, j->package, how);
    finish(j, RXJ_DECLINED, "", "nothing changed");
    busy(0);
    status("nothing changed");
}


/* ------------------------------------------------------ updating arospkg */

/* Said where the question was asked, in About, and on the status line. */
/* About's text is five lines of about fifty characters; a longer line is
   broken at a space rather than cut off at the frame. */
static void about_text(const char *in)
{
    static char text[900];
    size_t k = 0, col = 0, last = 0;
    int lines = 1;
    for (; *in && k + 2 < sizeof text; in++) {
        if (*in == '\n') { text[k++] = '\n'; col = 0; last = 0; lines++; continue; }
        if (*in == ' ') last = k;
        text[k++] = *in;
        if (++col > 50 && last && lines < 6) { text[last] = '\n'; col = k - last - 1; last = 0; lines++; }
    }
    text[k] = 0;
    SetAttrs(txt_release, MUIA_Text_Contents, (IPTR)text, TAG_DONE);
}

static void su_say(const char *line, const char *about)
{
    about_text(about ? about : line);
    status(line);
    logline("self-update: %s", line);
}

/* One Shell argument, quoted as ReadArgs reads it back. */
static void su_quote(char *out, size_t n, const char *arg)
{
    size_t k = 0;
    if (n < 3) { if (n) out[0] = 0; return; }
    out[k++] = '"';
    for (; *arg && k + 3 < n; arg++) {
        if (*arg == '"' || *arg == '*') out[k++] = '*';
        out[k++] = *arg;
    }
    out[k++] = '"';
    out[k] = 0;
}

/* Run a command and give back the first line it printed: whether an apkg
   understands the hand-off this window writes, before the window closes
   for good. */
static int su_first_line(const char *cmd, char *out, size_t n)
{
    char file[64], *t;
    struct Process *me = (struct Process *)FindTask(NULL);
    BPTR o, in, home, td, old;
    LONG rc;
    snprintf(file, sizeof file, "%sarospkg-proto-%s", u_exists("T:") ? "T:" : "RAM:", announce_nonce);
    if (!(o = Open((CONST_STRPTR)file, MODE_NEWFILE))) return -1;
    if (!(in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE))) { Close(o); return -1; }
    /* Even a command that ends at once keeps a copy of the caller's home
       drawer locked until a reboot (~/Work/AROS/CLAUDE.md): this window's
       drawer is the one about to be replaced. */
    td = Lock((CONST_STRPTR)(u_exists("T:") ? "T:" : "RAM:"), SHARED_LOCK);
    home = me->pr_HomeDir; me->pr_HomeDir = BNULL;
    old = td ? CurrentDir(td) : BNULL;
    rc = SystemTags((CONST_STRPTR)cmd, SYS_Output, (IPTR)o, SYS_Input, (IPTR)in, TAG_DONE);
    if (td) { CurrentDir(old); UnLock(td); }
    me->pr_HomeDir = home;
    Close(in);
    Close(o);
    t = u_read_all(file, NULL);
    u_delete(file);
    if (rc != 0 || !t) { free(t); return -1; }
    t[strcspn(t, "\r\n")] = 0;
    snprintf(out, n, "%s", t);
    free(t);
    return 0;
}

/* The apkg that finishes the update: the installation's own, or the new
   one just checked, whichever reads this window's hand-off. */
static int su_helper(char *out, size_t n)
{
    char cand[5][PKG_MAXPATH], cmd[PKG_MAXPATH + 64], line[80], want[40];
    int i;
    snprintf(want, sizeof want, "arospkg-self-update %d", SU_PROTOCOL);
    /* After an interruption the drawer's apkg may be missing, as .new or
       .old beside it; any of them, or C:apkg, will do if it reads this
       hand-off. */
    u_join(cand[0], PKG_MAXPATH, sujob.in.drawer, "apkg");
    u_join(cand[1], PKG_MAXPATH, sujob.st.dir, "new/apkg");
    u_join(cand[2], PKG_MAXPATH, sujob.in.drawer, "apkg.new");
    u_join(cand[3], PKG_MAXPATH, sujob.in.drawer, "apkg.old");
    snprintf(cand[4], PKG_MAXPATH, "C:apkg");
    for (i = 0; i < 5; i++) {
        if (!sujob.st.dir[0] && i == 1) continue;
        char q[PKG_MAXPATH + 8];
        if (!u_exists(cand[i])) continue;
        su_quote(q, sizeof q, cand[i]);
        snprintf(cmd, sizeof cmd, "%s self-update --protocol", q);
        if (su_first_line(cmd, line, sizeof line) == 0 && !strcmp(line, want)) {
            snprintf(out, n, "%s", cand[i]);
            return 0;
        }
        logline("self-update: %s does not read this hand-off (%s)", cand[i], line);
    }
    return -1;
}

/* Hand the update to apkg and leave. apkg waits until this process is
   gone, replaces the files and starts PkgManager again with what it was
   started with. Returns 0 when apkg has it. */
static int su_hand_off(const su_stage *st, const char *what)
{
    su_restart rs;
    pkg_err e;
    char helper[PKG_MAXPATH], file[PKG_MAXPATH + 16], q[PKG_MAXPATH + 8], cmd[2 * PKG_MAXPATH + 64];
    struct Process *me = (struct Process *)FindTask(NULL);
    BPTR t, old, home;
    LONG rc;

    memset(&rs, 0, sizeof rs); memset(&e, 0, sizeof e);
    if (su_helper(helper, sizeof helper) != 0) {
        su_say("cannot update from this window: its apkg does not take the update over",
               "This installation's apkg cannot finish an update started here.\nUpdate from a Shell: apkg self-update");
        return -1;
    }
    rs.wb = wb_start;
    snprintf(rs.program, sizeof rs.program, "%s", sujob.program);
    snprintf(rs.root, sizeof rs.root, "%s", root);
    rs.task = (unsigned long)(IPTR)me;
    if (me->pr_CLI) rs.module = (unsigned long)(IPTR)((struct CommandLineInterface *)BADDR(me->pr_CLI))->cli_Module;
    snprintf(rs.nonce, sizeof rs.nonce, "%s", announce_nonce);
    {
        /* Started again as it was started now: the same root and options. */
        size_t k = (size_t)snprintf(rs.args, sizeof rs.args, "--after-update");
        if (strcmp(root, "SYS:Packages")) { su_quote(q, sizeof q, root); k += (size_t)snprintf(rs.args + k, sizeof rs.args - k, " --root %s", q); }
        if (log_path[0] && k < sizeof rs.args) { su_quote(q, sizeof q, log_path); k += (size_t)snprintf(rs.args + k, sizeof rs.args - k, " --log %s", q); }
        if (demo_arg[0] && k < sizeof rs.args) { su_quote(q, sizeof q, demo_arg); k += (size_t)snprintf(rs.args + k, sizeof rs.args - k, " --demo %s", q); }
        if (start_updates && k < sizeof rs.args) snprintf(rs.args + k, sizeof rs.args - k, " --updates");
    }
    u_join(file, sizeof file, st->dir, "handoff.json");
    if (su_handoff_save(file, &sujob.in, st, &rs, &e) != PKG_OK) { show_err("cannot update arospkg", e.status, &e); return -1; }
    su_quote(q, sizeof q, helper);
    {
        char qf[PKG_MAXPATH + 24];
        su_quote(qf, sizeof qf, file);
        snprintf(cmd, sizeof cmd, "%s self-update --apply %s", q, qf);
    }
    /* Its own current directory in T:, and no copy of this window's home
       drawer: either would hold a lock in the installation it replaces. */
    {
        BPTR nin = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE), nout = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
        if (!nin || !nout || !(t = Lock((CONST_STRPTR)(u_exists("T:") ? "T:" : "RAM:"), SHARED_LOCK))) {
            if (nin) Close(nin);
            if (nout) Close(nout);
            su_say("cannot start apkg to finish the update; nothing was changed", NULL);
            return -1;
        }
        home = me->pr_HomeDir; me->pr_HomeDir = BNULL;
        old = CurrentDir(t);
        rc = SystemTags((CONST_STRPTR)cmd, SYS_Input, (IPTR)nin, SYS_Output, (IPTR)nout,
                        SYS_Asynch, TRUE, NP_StackSize, 262144, TAG_DONE);
        CurrentDir(old);
        me->pr_HomeDir = home;
        UnLock(t);
        if (rc == -1) {
            Close(nin); Close(nout);
            su_say("cannot start apkg to finish the update; nothing was changed", NULL);
            return -1;
        }
    }
    logline("self-update: %s handed to %s (%s); quitting", what, helper, file);
    su_handed_off = 1;
    return 0;
}

static void su_done(job_kind kind, pm_msg *m, const char *why)
{
    char line[900];
    su_install *in = &sujob.in;
    if (kind == JOB_SU_LOCATE) {
        if (m->status != PKG_OK) {
            snprintf(line, sizeof line, "cannot update arospkg: %s", why);
            su_say(line, NULL); busy(0); return;
        }
        if (sujob.where.kind != SU_WHERE_HOME) {
            /* Only the drawer of a release or of the package is replaced
               from here; a renamed or moved PkgManager is updated by hand. */
            su_say("cannot update arospkg from here: this PkgManager is not in an arospkg drawer "
                   "(no apkg, BUILD.txt or package record beside it)", NULL);
            busy(0); return;
        }
        if (sujob.c_path[0]) {
            /* C:apkg is where the release README puts a copy; whether it is
               this installation's is the user's to say, once. */
            LONG a;
            pkg_err e;
            memset(&e, 0, sizeof e);
            status("waiting for your decision");
            if (auto_yes) a = 1;
            else a = MUI_Request(app, win_about, 0, (STRPTR)"Update arospkg",
                                 (STRPTR)"Update it too|Leave it|Cancel",
                                 (STRPTR)"%s (%s) is another apkg on this machine.\n"
                                         "It is not recorded with any installation.\n\n"
                                         "Update it together with this PkgManager?",
                                 (IPTR)sujob.c_path, (IPTR)sujob.c_version);
            if (a == 0) { su_say("update cancelled; nothing was changed", NULL); busy(0); return; }
            if (su_record(sujob.c_path, a == 1 ? in->drawer : "", &e) != PKG_OK) {
                snprintf(line, sizeof line, "cannot update arospkg: %s (%s); nothing was changed", e.summary, e.subject);
                su_say(line, NULL); busy(0); return;
            }
            if (a == 1 && su_add_copy(in, sujob.c_path) != 0) {
                snprintf(line, sizeof line, "cannot update arospkg: cannot read %s (recorded as part of this "
                         "installation); no file was changed", sujob.c_path);
                su_say(line, NULL); busy(0); return;
            }
            logline("self-update: %s recorded as %s", sujob.c_path, a == 1 ? "part of this installation" : "on its own");
        }
        snprintf(sujob.stage_dir, sizeof sujob.stage_dir, "%sarospkg-update-%s", u_exists("T:") ? "T:" : "RAM:", announce_nonce);
        SetAttrs(txt_release, MUIA_Text_Contents, (IPTR)"Downloading and checking the new arospkg...\n"
                 "Progress is shown at the bottom of the main window;\nCancel there stops it. Nothing is changed yet.", TAG_DONE);
        if (!begin(JOB_SU_STAGE, NULL, "window")) busy(0);
        return;
    }
    /* JOB_SU_STAGE */
    if (m->status != PKG_OK) {
        if (sujob.stage_dir[0]) u_deltree(sujob.stage_dir);
        if (m->status == PKG_E_CANCELLED) su_say("update cancelled; nothing was changed", NULL);
        else { snprintf(line, sizeof line, "cannot update arospkg: %s", why); su_say(line, NULL); }
        busy(0); return;
    }
    if (!sujob.st.nreplace && !sujob.st.managed) {
        u_deltree(sujob.stage_dir);
        su_say("arospkg is up to date: every file already is the release's", NULL);
        busy(0); return;
    }
    {
        static char text[2400];
        size_t k;
        int i;
        /* What happens to the window first: it is what the user has to
           know before pressing Proceed; the files follow. */
        k = (size_t)snprintf(text, sizeof text, "Update arospkg from %s to %s\n\n"
                             /* Broken by hand: Floattext wraps under its own scroller. */
                             "PkgManager closes now and starts again\nwhen the new files are in place.\n"
                             "Icons and settings are kept.\n\n", PM_VERSION, sujob.st.version);
        if (sujob.st.managed && k < sizeof text)
            k += (size_t)snprintf(text + k, sizeof text - k, "The package arospkg in %s is replaced as an upgrade "
                                  "(%s -> %s); apkg rollback arospkg goes back.\n\n",
                                  in->root, in->managed_version, sujob.st.version);
        if (k < sizeof text)
            k += (size_t)snprintf(text + k, sizeof text - k, "In %s:\n  ", in->drawer);
        for (i = 0; i < in->n && k < sizeof text; i++)
            if (in->f[i].act == SU_REPLACE && !in->f[i].copy)
                k += (size_t)snprintf(text + k, sizeof text - k, "%s ", su_role_name(in->f[i].role));
        for (i = 0; i < in->n && k < sizeof text; i++)
            if (in->f[i].act == SU_REPLACE && in->f[i].copy)
                k += (size_t)snprintf(text + k, sizeof text - k, "\nAlso %s (%s)", in->f[i].path,
                                      in->f[i].version[0] ? in->f[i].version : "?");
        if (k < sizeof text)
            snprintf(text + k, sizeof text - k, "\n\nThe previous files stay beside them, named .old.\n\nProceed?");
        logline("self-update: plan to %s, %d file(s)%s, waits for confirmation", sujob.st.version,
                sujob.st.nreplace, sujob.st.managed ? " and the package" : "");
        su_confirming = confirming = 1;
        status("waiting for your decision");
        SetAttrs(btn_cancel, MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(gauge, MUIA_Gauge_InfoText, (IPTR)"waiting for your decision", TAG_DONE);
        if (auto_yes) { su_proceed(); return; }
        SetAttrs(win_confirm, MUIA_Window_Open, TRUE, TAG_DONE);
        SetAttrs(txt_confirm, MUIA_Floattext_Text, (IPTR)text, TAG_DONE);
    }
}

static void su_proceed(void)
{
    confirm_close();
    su_confirming = 0;
    logline("self-update: confirmed");
    if (su_hand_off(&sujob.st, "the update") != 0) { u_deltree(sujob.stage_dir); busy(0); }
}

static void su_decline(const char *how)
{
    confirm_close();
    su_confirming = 0;
    u_deltree(sujob.stage_dir);
    logline("self-update: %s", how);
    su_say("update cancelled; nothing was changed", NULL);
    busy(0);
}

/* Update arospkg, pressed in About. */
static void su_begin(void)
{
    char run[SU_MAXCAND][PKG_MAXPATH];
    int un = 0, n = su_running_pkgmanagers(run, SU_MAXCAND, &un), i, mine = 0;
    for (i = 0; i < n; i++) if (!strcasecmp(run[i], self_drawer)) mine++;
    if (mine > 1 || un) {
        su_say("close the other PkgManager windows first: they would keep running the old version", NULL);
        return;
    }
    if (su_journal_pending()) {
        su_say("an earlier update of arospkg did not finish: start PkgManager again to repair it", NULL);
        return;
    }
    memset(&sujob.in, 0, sizeof sujob.in);
    memset(&sujob.st, 0, sizeof sujob.st);
    sujob.stage_dir[0] = 0;
    begin(JOB_SU_LOCATE, NULL, "window");
}

/* At the start: what the last update did, or an update left unfinished. */
static void su_at_start(void)
{
    su_result r;
    if (su_result_take(&r)) {
        static char line[300];
        logline("self-update result: ok=%d changed=%d %s -> %s: %s | %s", r.ok, r.changed, r.from, r.to, r.summary, r.detail);
        snprintf(line, sizeof line, "%s", r.summary);
        status(line);
        SetAttrs(txt_release, MUIA_Text_Contents, (IPTR)line, TAG_DONE);
        if (!auto_yes)
            MUI_Request(app, win, 0, (STRPTR)"arospkg update", (STRPTR)"OK", (STRPTR)"%s\n\n%s",
                        (IPTR)r.summary, (IPTR)r.detail);
    }
    if (su_journal_pending()) {
        /* Not repaired automatically under --yes: a repair that cannot
           succeed would start this window again, and again. */
        LONG a = auto_yes ? 0 : MUI_Request(app, win, 0, (STRPTR)"arospkg update", (STRPTR)"Repair now|Later",
                                            (STRPTR)"An update of arospkg did not finish.\n\n"
                                                    "PkgManager closes, the update is finished or undone,\n"
                                                    "and PkgManager starts again.", NULL);
        logline("self-update: unfinished update found; %s", a ? "repair" : "later");
        if (a) {
            su_stage none;
            memset(&none, 0, sizeof none);
            memset(&sujob.in, 0, sizeof sujob.in);
            snprintf(sujob.in.drawer, sizeof sujob.in.drawer, "%s", self_drawer);
            snprintf(none.dir, sizeof none.dir, "%sarospkg-repair-%s", u_exists("T:") ? "T:" : "RAM:", announce_nonce);
            sujob.st = none;
            if (u_mkdir(none.dir) != 0 || su_hand_off(&none, "the repair") != 0) u_deltree(none.dir);
        } else status("an update of arospkg did not finish: start PkgManager again to repair it");
    }
}

/* Returns 1 when the window should now quit. */
static void update_news_status(const char *done);

static int on_message(pm_msg *m)
{
    char info[80];
    /* Every message names its job. One for any other job than the one out
       cannot happen while in_flight holds the next one back; if it ever
       does, it is logged and not read as this job's. */
    if (!in_flight || m->seq != cur.seq) {
        logline("message for job %ld ignored: job %ld is out%s", m->seq, cur.seq, in_flight ? "" : " (none)");
        return 0;
    }
    if (m->kind == PM_PROGRESS) {
        unsigned long pct = m->total ? (m->done * 100UL) / m->total : (m->done ? 100 : 0);
        if (m->total) snprintf(info, sizeof info, "%s %lu%%", m->phase, pct);
        else snprintf(info, sizeof info, "%s", m->phase);
        SetAttrs(gauge, MUIA_Gauge_Current, pct, MUIA_Gauge_InfoText, (IPTR)info, TAG_DONE);
        /* Stated by the event, not inferred from the phase. */
        SetAttrs(btn_cancel, MUIA_Disabled, !m->can_cancel || quit_pending, TAG_DONE);
        snprintf(last_phase, sizeof last_phase, "%s", m->phase); last_can_cancel = m->can_cancel;
        if (active) {
            snprintf(active->phase, sizeof active->phase, "%s", m->phase);
            active->done = m->done; active->total = m->total; active->can_cancel = m->can_cancel;
        }
        logline("event %s %s %lu/%lu can_cancel=%d cancel_button=%s", m->id, m->phase,
                m->done, m->total, m->can_cancel, m->can_cancel && !quit_pending ? "enabled" : "disabled");
        return 0;
    }
    if (m->kind == PM_DONE) {
        static const char *verb[] = { "none", "install", "remove", "update",
                                      "preview upgrade", "upgrade", "preview rollback", "rollback",
                                      "check for a newer version", "verify", "start",
                                      "update arospkg", "update arospkg" };
        static const char *past[] = { "", "installed", "removed", "index updated",
                                      "", "upgraded", "", "rolled back", "checked", "verified", "started",
                                      "", "" };
        job_kind kind = cur.kind;
        rx_job *j = active;
        char line[160], why[640];
        snprintf(line, sizeof line, "%s %s", past[kind], cur.id);
        logline("done %s %s status=%d%s%s", verb[kind], cur.id, (int)m->status,
                m->status == PKG_OK ? "" : " ", m->status == PKG_OK ? "" : m->err.summary);
        in_flight = 0;
        err_text(why, sizeof why, &m->err);
        if (kind == JOB_CHECK_RELEASE) {
            /* The answer goes where the question was asked, in About. It
               says plainly that the window is not replaced by anything. */
            static char text[600];
            const pkg_release *r = &m->rel;
            if (m->status != PKG_OK) snprintf(text, sizeof text, "Could not check: %s", why);
            else if (r->found != 1)
                snprintf(text, sizeof text, "The latest stable release (%s) has no archive for %s.\n"
                         "This PkgManager %s stays as it is.", r->other_version[0] ? r->other_version : "?",
                         r->target, PM_VERSION);
            else {
                /* The whole installation, not this window alone: its apkg,
                   the copies tied to it and a package record can be behind
                   while PkgManager is current. Nothing newer is moved back. */
                const su_install *in = &sujob.in;
                int older = 0, newer = 0, i, shown = 0;
                /* A package already at the release: its own files are verify's
                   business, as the plan says (su_plan). */
                int pkg_current = in->home == SU_HOME_MANAGED && in->managed_version[0]
                                  && pkg_release_vercmp(in->managed_version, r->version) >= 0;
                size_t k = (size_t)snprintf(text, sizeof text, "Latest stable release: %s\n", r->version);
                if (sujob.located && sujob.where.kind == SU_WHERE_HOME) {
                    for (i = 0; i < in->n; i++) {
                        const su_file *f = &in->f[i];
                        const char *w;
                        if (f->role != SU_APKG && f->role != SU_PKGMANAGER) continue;
                        w = su_state_word(f, r->version);
                        if (!(pkg_current && !f->copy)) {
                            if (!strcmp(w, "older") || !strcmp(w, "unknown")) older++;
                            if (!strcmp(w, "newer")) newer++;
                        }
                        if (k < sizeof text)
                            k += (size_t)snprintf(text + k, sizeof text - k, "%s%s %s (%s)", shown++ ? ", " : "",
                                                  f->copy ? (const char *)FilePart((STRPTR)f->path) : su_role_name(f->role),
                                                  f->version[0] ? f->version : "?", w);
                    }
                    if (su_managed_behind(in, r->version)) older++;
                    if (in->home == SU_HOME_MANAGED && in->managed_version[0]
                        && pkg_release_vercmp(in->managed_version, r->version) > 0) newer++;
                } else if (r->cmp < 0) older++;
                else if (r->cmp > 0) newer++;
                if (k < sizeof text)
                    snprintf(text + k, sizeof text - k, "\n%s", newer ? "Part of this installation is newer than the release; nothing is offered."
                             : older ? "Update arospkg brings everything to this release."
                             : "Everything is up to date.");
                su_offer = !newer && older > 0;
            }
            about_text(text);
            if (m->status != PKG_OK || r->found != 1) su_offer = 0;
            logline("release check: status=%d found=%d version=%s other=%s cmp=%d", (int)m->status,
                    r->found, r->version, r->other_version, r->cmp);
            if (j) {
                if (m->status == PKG_OK) finish(j, RXJ_DONE, "", text);
                else finish(j, RXJ_FAILED, rx_statusword(m->status), why);
            }
            if (quit_pending) return 1;
            busy(0);
            status(m->status == PKG_OK ? "release checked: see About" : "could not check for a newer version");
            return 0;
        }
        if (kind == JOB_SU_LOCATE || kind == JOB_SU_STAGE) {
            if (j) {
                if (m->status == PKG_OK) finish(j, RXJ_DONE, "", "");
                else finish(j, m->status == PKG_E_CANCELLED ? RXJ_CANCELLED : RXJ_FAILED, rx_statusword(m->status), why);
            }
            if (quit_pending) { if (sujob.stage_dir[0]) u_deltree(sujob.stage_dir); return 1; }
            su_done(kind, m, why);
            return 0;
        }
        if (kind == JOB_VERIFY || kind == JOB_RUN) {
            /* Neither changes anything, so the list stays as it is; the
               outcome goes to the status line, and verify's to the detail
               pane, where it stays while the package is selected. */
            char text[300];
            if (kind == JOB_VERIFY) {
                verify_store(cur.id, m->status, &m->ver, m->listing, why);
                if (m->status == PKG_OK)
                    snprintf(text, sizeof text, "verified %s: %d as installed, %d changed locally, %d missing, %d not the package's",
                             cur.id, m->ver.ok, m->ver.changed, m->ver.missing, m->ver.unknown);
            } else if (m->status == PKG_OK) {
                snprintf(text, sizeof text, "%s", m->run.how);
            }
            if (m->listing) FreeVec(m->listing);
            m->listing = NULL;
            if (j) {
                if (m->status == PKG_OK) finish(j, RXJ_DONE, "", text);
                else finish(j, RXJ_FAILED, rx_statusword(m->status), why);
            }
            if (quit_pending) return 1;
            busy(0);
            if (m->status == PKG_OK) status(text);
            else show_err(verb[kind], m->status, &m->err);
            return 0;
        }
        if (kind == JOB_PREVIEW_UPGRADE || kind == JOB_PREVIEW_ROLLBACK) {
            /* The plan came back as data. Show it, and ask -- or say why not. */
            const pkg_plan *p = &m->plan;
            const char *what = kind == JOB_PREVIEW_UPGRADE ? "upgrade" : "rollback";
            char text[640], a[80], b[80], keep[PKG_MAXID];
            snprintf(keep, sizeof keep, "%s", cur.id);
            if (m->status != PKG_OK) {
                if (j) finish(j, m->status == PKG_E_CANCELLED ? RXJ_CANCELLED : RXJ_FAILED,
                              rx_statusword(m->status), why);
                if (quit_pending) return 1;
                busy(0);
                show_err(what, m->status, &m->err);
                return 0;
            }
            rev_label(a, sizeof a, p->from_version, p->from_revision);
            rev_label(b, sizeof b, p->to_version, p->to_revision);
            logline("plan %s %s: %s -> %s allowed=%d add=%d replace=%d remove=%d kept=%d unchanged=%d conflicts=%d%s%s",
                    what, keep, a, b, p->allowed, p->add, p->replace, p->remove, p->kept, p->unchanged,
                    p->conflicts, p->why[0] ? " why=" : "", p->why);
            if (!p->allowed || p->conflicts) {
                snprintf(text, sizeof text, "%s not possible: %s", what, p->why);
                if (j) finish(j, RXJ_FAILED, p->conflicts ? "CONFLICT" : "NOTALLOWED", p->why);
                if (quit_pending) return 1;
                busy(0);
                status(text);
                return 0;
            }
            if (quit_pending) {
                if (j) finish(j, RXJ_DECLINED, "", "the window was closed before the plan was confirmed");
                return 1;
            }
            if (j) {
                j->state = RXJ_CONFIRM;
                j->phase[0] = 0; j->can_cancel = 0;
                snprintf(j->message, sizeof j->message,
                         "from %s to %s: %d added, %d replaced, %d removed, %d kept as changed, %d unchanged",
                         a, b, p->add, p->replace, p->remove, p->kept, p->unchanged);
            }
            /* Files a running program may read or write are not promised
               anything: the drawer is swapped underneath it. Say so. */
            snprintf(text, sizeof text,
                     "%s %s\n\nfrom %s to %s\n\n"
                     "%d file%s added, %d replaced, %d removed\n%d kept as you changed %s, %d unchanged\n\n"
                     "Files you changed are kept.\nIf %s is running, close it first.\n\nProceed?",
                     kind == JOB_PREVIEW_UPGRADE ? "Upgrade" : "Roll back", keep, a, b,
                     p->add, p->add == 1 ? "" : "s", p->replace, p->remove,
                     p->kept, p->kept == 1 ? "it" : "them", p->unchanged, keep);
            confirming = 1;
            SetAttrs(gauge, MUIA_Gauge_Current, 0, MUIA_Gauge_InfoText, (IPTR)"waiting for your decision", TAG_DONE);
            status("waiting for your decision");
            if (auto_yes) { proceed("confirmed automatically (--yes)"); return 0; }
            /* Open first: Floattext lays its text out only when it has a
               window, and text set before that stays blank. */
            SetAttrs(win_confirm, MUIA_Window_Open, TRUE, TAG_DONE);
            SetAttrs(txt_confirm, MUIA_Floattext_Text, (IPTR)text, TAG_DONE);
            return 0;
        }
        if (j) {
            if (m->status == PKG_OK) finish(j, RXJ_DONE, "", line);
            else finish(j, m->status == PKG_E_CANCELLED ? RXJ_CANCELLED : RXJ_FAILED,
                        rx_statusword(m->status), why);
        }
        if (quit_pending) return 1;       /* the worker is gone; see worker.c */
        if (m->note[0]) logline("finished an interrupted replacement: %s", m->note);
        busy(0);
        {
            IPTR t = 0; char keep[PKG_MAXID];
            snprintf(keep, sizeof keep, "%s", cur.id);
            GetAttr(MUIA_String_Contents, str_search, &t);
            refill((const char *)t, keep[0] ? keep : NULL, "done");
        }
        /* After the refill, which writes its own count: the outcome is what
           the user is waiting to read. */
        if (m->status == PKG_OK && kind == JOB_REMOVE && (m->rem.kept || m->rem.others || m->rem.icon[0])) {
            /* What removal promises is to keep what the user changed or
               added; say what that was, as apkg remove does. */
            static char kept[600];
            size_t k = (size_t)snprintf(kept, sizeof kept, "removed %s; kept in %s:", cur.id,
                                        m->rem.dir[0] ? m->rem.dir : "its drawer");
            int i;
            for (i = 0; i < m->rem.named && k + 40 < sizeof kept; i++)
                k += (size_t)snprintf(kept + k, sizeof kept - k, " %s (changed)", m->rem.names[i]);
            for (i = 0; i < m->rem.others_named && k + 40 < sizeof kept; i++)
                k += (size_t)snprintf(kept + k, sizeof kept - k, " %s (yours)", m->rem.other_names[i]);
            if (m->rem.icon[0] && k + 40 < sizeof kept)
                snprintf(kept + k, sizeof kept - k, " and the changed drawer icon");
            logline("%s", kept);
            status(kept);
        }
        else if (m->status == PKG_OK && kind != JOB_UPDATE && m->note[0]) {
            /* Said where the user looks: an old package was removed now. */
            static char both[600];
            snprintf(both, sizeof both, "%s; finished an interrupted replacement: %s", line, m->note);
            status(both);
        }
        else if (m->status == PKG_OK && kind != JOB_UPDATE) status(line);
        else if (m->status == PKG_OK) update_news_status(past[kind]);
        else show_err(verb[kind], m->status, &m->err);
    }
    return 0;
}

/* After Update index: what the new catalogue brought for this machine, in
   one line, from the record apkg recent --since-last-update reads. The
   views behind it are Last refresh and Recently added. */
static void update_news_status(const char *done)
{
    static char line[400];
    pkg_ctx *c = NULL; pkg_err e; pkg_news nw;
    memset(&e, 0, sizeof e);
    memset(&nw, 0, sizeof nw);
    if (pkg_open(root, PKG_OPEN_READ, &c, &e) == PKG_OK) {
        pkg_news_last(c, index_path, &nw, &e);
        pkg_close(c);
    }
    if (!nw.recorded || !nw.current)
        snprintf(line, sizeof line, "%s; what is new in it could not be worked out", done);
    else if (nw.first)
        snprintf(line, sizeof line, "%s. %s Choose \"Recently added\" to see the newest.", done, nw.first_why);
    else if (!nw.n)
        snprintf(line, sizeof line, "%s; nothing new for this machine", done);
    else
        snprintf(line, sizeof line, "%s: %d new application%s, %d new build%s, %d updated -- choose \"Last refresh\" to list them",
                 done, nw.apps, nw.apps == 1 ? "" : "s", nw.builds, nw.builds == 1 ? "" : "s", nw.updated);
    logline("news after update: recorded=%d current=%d first=%d new=%d builds=%d updated=%d",
            nw.recorded, nw.current, nw.first, nw.apps, nw.builds, nw.updated);
    pkg_news_free(&nw);
    status(line);
}

/* A close while a job runs: cancel it where cancelling is free, and leave
 * when it reports done -- never before, because the worker process is
 * still running this program's code until then. */
static void close_requested(void)
{
    if (!in_flight) return;
    quit_pending = 1;
    cur.cancel = 1;
    if (active) active->cancel_asked = 1;
    logline("close requested during %s can_cancel=%d", last_phase[0] ? last_phase : "start", last_can_cancel);
    status(last_can_cancel ? "closing: cancelling the download..."
                           : "closing when the current operation finishes...");
    SetAttrs(btn_cancel, MUIA_Disabled, TRUE, TAG_DONE);
}

/* The worker outliving its last message is not expected; if it happens, the
 * window says so and keeps waiting, because unloading the code it runs is
 * the one thing that must not happen. */
static void still_waiting(long ms)
{
    char line[96];
    snprintf(line, sizeof line, "waiting for the worker to end (%ld s)...", ms / 1000);
    status(line);
    logline("worker task still present %ld ms after done", ms);
}

/* ----------------------------------------------------------------- ARexx */

/* What a command does is decided here, on this task, between two Zune
 * events: queries read through libpkg exactly as the window does, and
 * operations go through begin() like the buttons, so a script is held to
 * the same rules -- one operation at a time, the same worker, the same
 * confirmation. */

static char rxout[RX_MAXRESULT + 1];

/* A script polling JOB in a loop sends the same line thousands of times;
 * the log keeps the first and then says how many followed, so a test's
 * event log stays readable. `quiet` silences the reply of a repeat. */
static char last_cmd[128];
static long repeats;
static int quiet;

static void log_command(const char *line)
{
    if (strlen(line) < sizeof last_cmd && !strcmp(line, last_cmd)) { repeats++; quiet = 1; return; }
    if (repeats) logline("arexx> (the line above %ld more times)", repeats);
    repeats = 0; quiet = 0;
    snprintf(last_cmd, sizeof last_cmd, "%s", line);
    logline("arexx> %s", line);
}

/* The binary's own hash, as apkg --version reports it: a run report names
 * the binary that answered, not the source it was built from. Once. */
static const char *self_sha(void)
{
    static char sha[65];
    if (!sha[0] && pkg_self_sha256(sha) != 0) snprintf(sha, sizeof sha, "unknown");
    return sha;
}

static void rx_ok(rexx_msg *m, const char *result)
{
    char head[72]; size_t i;
    snprintf(head, sizeof head, "%s", result);
    for (i = 0; head[i]; i++) if (head[i] == '\n') head[i] = '|';
    if (rexx_reply(m, RX_RC_OK, result, NULL) != RX_RC_OK)
        logline("arexx< rc=%d NOMEM no memory for the answer (len=%lu)", RX_RC_ERROR, (unsigned long)strlen(result));
    else if (!quiet)
        logline("arexx< rc=0 len=%lu %s%s", (unsigned long)strlen(result), head,
                strlen(result) >= sizeof head ? "..." : "");
}

static void rx_fail(rexx_msg *m, int rc, const char *fmt, ...)
{
    char error[600];
    va_list ap;
    va_start(ap, fmt); vsnprintf(error, sizeof error, fmt, ap); va_end(ap);
    if (!quiet) logline("arexx< rc=%d %s", rc, error);
    rexx_reply(m, rc, NULL, error);
}

static void reply_job(rexx_msg *m, const rx_job *j, const char *field)
{
    static char out[2048];
    if (rx_format_job(j, field, out, sizeof out) != 0)
        rx_fail(m, RX_RC_FAIL, "BADARGS unknown field \"%s\"; fields: %s", field, rx_job_fields());
    else rx_ok(m, out);
}

static pkg_ctx *rx_open(rexx_msg *m)
{
    pkg_ctx *c = NULL; pkg_err e; pkg_status st;
    memset(&e, 0, sizeof e);
    st = pkg_open(root, PKG_OPEN_READ, &c, &e);
    if (st != PKG_OK) {
        char t[600]; err_text(t, sizeof t, &e);
        rx_fail(m, RX_RC_ERROR, "%s %s", rx_statusword(st), t);
        return NULL;
    }
    return c;
}

/* The row for `id`, as `apkg show` finds it: the catalogue's entry for this
 * machine (or the one show would describe), else what the registry records.
 * 1 = found. `compat` and `requirements` (may be NULL) get the verdicts. */
static int lookup(pkg_ctx *c, const char *id, pkg_entry *out, int *in_index, pkg_compat *compat,
                  char *requirements, size_t rn)
{
    pkg_details det; pkg_err e;
    memset(&e, 0, sizeof e);
    *in_index = 0;
    if (pkg_details_get(c, index_path, id, &det, &e) != PKG_OK) return 0;
    *out = det.e;
    *in_index = det.in_index;
    if (compat) *compat = det.compat;
    if (requirements) snprintf(requirements, rn, "%s", det.requirements);
    return 1;
}

static void rx_list(rexx_msg *m, const rx_req *r)
{
    pkg_ctx *c = rx_open(m); pkg_entries rows = { 0 }; pkg_err e; pkg_status st;
    size_t k = 0; int i, over = 0;
    if (!c) return;
    memset(&e, 0, sizeof e);
    if (r->installed && !r->match[0]) {
        st = pkg_installed(c, index_path, &rows, &e);
        if (st != PKG_OK) { pkg_entries_free(&rows); memset(&rows, 0, sizeof rows); st = pkg_installed(c, NULL, &rows, &e); }
    } else st = pkg_query(c, index_path, r->match[0] ? r->match : NULL, &rows, NULL, &e);
    pkg_close(c);
    if (st == PKG_E_NOT_FOUND && !r->installed) { rx_fail(m, RX_RC_ERROR, "NOINDEX there is no catalogue yet; UPDATE fetches it"); pkg_entries_free(&rows); return; }
    if (st != PKG_OK) { char t[600]; err_text(t, sizeof t, &e); rx_fail(m, RX_RC_ERROR, "%s %s", rx_statusword(st), t); pkg_entries_free(&rows); return; }
    rxout[0] = 0;
    for (i = 0; i < rows.n && !over; i++) {
        size_t n = strlen(rows.v[i].id);
        if (r->installed && !rows.v[i].installed) continue;
        if (k + n + 2 > sizeof rxout) { over = 1; break; }
        if (k) rxout[k++] = ' ';
        memcpy(rxout + k, rows.v[i].id, n + 1); k += n;
    }
    pkg_entries_free(&rows);
    if (over) rx_fail(m, RX_RC_ERROR, "TOOLONG the list does not fit in one ARexx result; narrow it with MATCH");
    else rx_ok(m, rxout);
}

static void rx_info(rexx_msg *m, const rx_req *r)
{
    pkg_ctx *c = rx_open(m); pkg_entry e; int in_index, up = 0, back = 0, f;
    char why[240]; pkg_compat rv = PKG_COMPAT_UNDETERMINED; char reqs[16] = "";
    if (!c) return;
    if (!lookup(c, r->package, &e, &in_index, &rv, reqs, sizeof reqs)) {
        pkg_close(c);
        rx_fail(m, RX_RC_ERROR, "NOTFOUND %s is neither in the catalogue nor installed", r->package);
        return;
    }
    if (e.installed) {
        /* As the window's Upgrade button: from the updates model. */
        pkg_updates u; pkg_err ue; memset(&ue, 0, sizeof ue);
        if (pkg_updates_get(c, index_path, 1, &u, &ue) == PKG_OK) {
            const pkg_update_row *ur = pkg_updates_find(&u, e.id);
            up = ur && ur->state == PKG_UPD_AVAILABLE;
            pkg_updates_free(&u);
        } else up = pkg_can_upgrade(&e, why, sizeof why);
        back = pkg_can_rollback(c, e.id, why, sizeof why);
    }
    pkg_close(c);
    f = rx_format_entry(&e, up, back, pkg_compat_word(rv), reqs, r->field, rxout, sizeof rxout);
    if (f == -1) rx_fail(m, RX_RC_FAIL, "BADARGS unknown field \"%s\"; fields: %s", r->field, rx_entry_fields());
    else rx_ok(m, rxout);
}

static void rx_requirements(rexx_msg *m, const rx_req *r)
{
    pkg_ctx *c = rx_open(m); pkg_entry e; pkg_err err; pkg_status st; int in_index; size_t n;
    if (!c) return;
    if (!lookup(c, r->package, &e, &in_index, NULL, NULL, 0) || !in_index) {
        pkg_close(c);
        rx_fail(m, RX_RC_ERROR, "NOTFOUND %s is not in the catalogue", r->package);
        return;
    }
    memset(&err, 0, sizeof err);
    rxout[0] = 0;
    st = pkg_requirements(c, index_path, r->package, rxout, sizeof rxout, &err);
    pkg_close(c);
    if (st != PKG_OK) { char t[600]; err_text(t, sizeof t, &err); rx_fail(m, RX_RC_ERROR, "%s %s", rx_statusword(st), t); return; }
    n = strlen(rxout);
    while (n && rxout[n - 1] == '\n') rxout[--n] = 0;
    rx_ok(m, rxout);
}

static void rx_show(rexx_msg *m, const rx_req *r)
{
    pkg_ctx *c = rx_open(m); pkg_entry e; pkg_entry *sel; int in_index, known;
    if (!c) return;
    known = lookup(c, r->package, &e, &in_index, NULL, NULL, 0) && in_index;
    pkg_close(c);
    if (!known) { rx_fail(m, RX_RC_ERROR, "NOTFOUND %s is not in the catalogue", r->package); return; }
    /* Clear the search and the filters, or the row could be filtered out of
       the very list it is to be shown in. */
    SetAttrs(str_search, MUIA_NoNotify, TRUE, MUIA_String_Contents, (IPTR)"", TAG_DONE);
    last_term[0] = 0; cat_chosen[0] = 0;
    SetAttrs(cyc_cat,   MUIA_NoNotify, TRUE, MUIA_Cycle_Active, 0, TAG_DONE);
    SetAttrs(cyc_state, MUIA_NoNotify, TRUE, MUIA_Cycle_Active, 0, TAG_DONE);
    refill("", r->package, "arexx show");
    sel = selected();
    if (!sel || strcmp(sel->id, r->package)) { rx_fail(m, RX_RC_ERROR, "NOTFOUND %s could not be selected in the list", r->package); return; }
    DoMethod(lst, MUIM_List_Jump, MUIV_List_Jump_Active);
    SetAttrs(app, MUIA_Application_Iconified, FALSE, TAG_DONE);
    SetAttrs(win, MUIA_Window_Open, TRUE, TAG_DONE);
    DoMethod(win, MUIM_Window_ToFront);
    SetAttrs(win, MUIA_Window_Activate, TRUE, TAG_DONE);
    rx_ok(m, "");
}

static void rx_start(rexx_msg *m, const rx_req *r)
{
    job_kind kind = r->cmd == RXC_UPDATE ? JOB_UPDATE : r->cmd == RXC_INSTALL ? JOB_INSTALL :
                    r->cmd == RXC_REMOVE ? JOB_REMOVE : r->cmd == RXC_UPGRADE ? JOB_PREVIEW_UPGRADE :
                    r->cmd == RXC_RUN ? JOB_RUN : r->cmd == RXC_VERIFY ? JOB_VERIFY :
                    JOB_PREVIEW_ROLLBACK;
    rx_job *j;
    if (quit_pending || rexx_quit) { rx_fail(m, RX_RC_ERROR, "CLOSING PkgManager is closing"); return; }
    if (busy_now()) {
        rx_job *a = active;
        if (a) rx_fail(m, RX_RC_WARN, "BUSY job %ld (%s%s%s) is %s", a->id, a->op,
                       a->package[0] ? " " : "", a->package, rx_statename(a->state));
        else rx_fail(m, RX_RC_WARN, "BUSY another operation is running");
        return;
    }
    if (kind != JOB_UPDATE) {
        pkg_ctx *c = rx_open(m); pkg_entry e; int in_index, known;
        if (!c) return;
        known = lookup(c, r->package, &e, &in_index, NULL, NULL, 0);
        pkg_close(c);
        if (!known || (kind == JOB_INSTALL && !in_index)) {
            rx_fail(m, RX_RC_ERROR, "NOTFOUND %s %s", r->package,
                    kind == JOB_INSTALL ? "is not in the catalogue" : "is neither in the catalogue nor installed");
            return;
        }
    }
    /* The answer is the job's number, and it is allocated before the job
       exists: if that fails the script hears NOMEM and nothing was started. */
    snprintf(rxout, sizeof rxout, "%ld", next_job);
    if (!rexx_prepare(m, rxout)) { rx_fail(m, RX_RC_ERROR, "NOMEM no memory for the answer; nothing was started"); return; }
    j = begin(kind, r->package, "arexx");
    if (!j || j->state == RXJ_FAILED) { rx_fail(m, RX_RC_ERROR, "CANNOTSTART could not start the worker process"); return; }
    rx_ok(m, rxout);
}

static void rx_jobcmd(rexx_msg *m, const rx_req *r)
{
    rx_job *j = find_job(r->job);
    int i;
    if (!j) {
        if (r->job) rx_fail(m, RX_RC_ERROR, "NOJOB there is no job %ld", r->job);
        else rx_fail(m, RX_RC_ERROR, "NOJOB no job has been started");
        return;
    }
    if (rx_format_job(j, r->field, rxout, sizeof rxout) == -1) {
        rx_fail(m, RX_RC_FAIL, "BADARGS unknown field \"%s\"; fields: %s", r->field, rx_job_fields());
        return;
    }
    if (r->cmd == RXC_WAIT && !rx_final(j->state)) {
        for (i = 0; i < MAXWAITS; i++) if (!waits[i].m) break;
        if (i == MAXWAITS) { rx_fail(m, RX_RC_WARN, "BUSY %d scripts are already waiting", MAXWAITS); return; }
        waits[i].m = m; waits[i].job = j->id;
        snprintf(waits[i].field, sizeof waits[i].field, "%s", r->field);
        logline("arexx wait for job %ld (%s)", j->id, rx_statename(j->state));
        return;
    }
    rx_ok(m, rxout);
}

static void rx_cancel(rexx_msg *m, const rx_req *r)
{
    rx_job *j = find_job(r->job);
    if (!j) { rx_fail(m, RX_RC_ERROR, "NOJOB there is no job %ld", r->job); return; }
    if (rx_final(j->state)) { rx_fail(m, RX_RC_WARN, "FINISHED job %ld is already %s", j->id, rx_statename(j->state)); return; }
    if (j->state == RXJ_CONFIRM) {
        if (!rexx_prepare(m, "declined")) { rx_fail(m, RX_RC_ERROR, "NOMEM no memory for the answer; nothing was done"); return; }
        decline("declined (ARexx CANCEL)"); rx_ok(m, "declined"); return;
    }
    /* Only while the library's last event said it would still take it; the
       library decides, and JOB then says what came of it. */
    if (j != active || !in_flight || !j->can_cancel) {
        rx_fail(m, RX_RC_WARN, "NOTCANCELLABLE job %ld is in phase %s, which cannot be cancelled",
                j->id, j->phase[0] ? j->phase : "start");
        return;
    }
    if (!rexx_prepare(m, "requested")) { rx_fail(m, RX_RC_ERROR, "NOMEM no memory for the answer; nothing was done"); return; }
    cur.cancel = 1; j->cancel_asked = 1;
    status("cancelling...");
    logline("cancel asked by ARexx for job %ld in %s", j->id, j->phase);
    rx_ok(m, "requested");
}

/* OUTDATED [SHOW]: the updates model's counts and ids; with SHOW the window
   also comes forward with the Updates filter, which is what the ScreenBar
   widget's action asks for when PkgManager is already running. */
static void rx_outdated(rexx_msg *m, const rx_req *r)
{
    pkg_ctx *c; pkg_updates u; pkg_err e;
    if (r->show) {
        SetAttrs(str_search, MUIA_NoNotify, TRUE, MUIA_String_Contents, (IPTR)"", TAG_DONE);
        last_term[0] = 0; cat_chosen[0] = 0;
        SetAttrs(cyc_cat,   MUIA_NoNotify, TRUE, MUIA_Cycle_Active, 0, TAG_DONE);
        SetAttrs(cyc_state, MUIA_NoNotify, TRUE, MUIA_Cycle_Active, ST_UPDATES, TAG_DONE);
        refill("", NULL, "arexx outdated show");
        SetAttrs(app, MUIA_Application_Iconified, FALSE, TAG_DONE);
        SetAttrs(win, MUIA_Window_Open, TRUE, TAG_DONE);
        DoMethod(win, MUIM_Window_ToFront);
        SetAttrs(win, MUIA_Window_Activate, TRUE, TAG_DONE);
    }
    if (!(c = rx_open(m))) return;
    memset(&e, 0, sizeof e);
    if (pkg_updates_get(c, index_path, 1, &u, &e) != PKG_OK) {
        char t[600]; pkg_close(c); err_text(t, sizeof t, &e);
        rx_fail(m, RX_RC_ERROR, "%s %s", rx_statusword(e.status), t); return;
    }
    pkg_close(c);
    if (rx_format_updates(&u, rxout, sizeof rxout) != 0) rx_fail(m, RX_RC_ERROR, "TOOLONG the answer does not fit in one ARexx result");
    else rx_ok(m, rxout);
    pkg_updates_free(&u);
}

static void rexx_dispatch(rexx_msg *m, const char *line)
{
    rx_req r; char err[300];
    log_command(line);
    if (rx_parse(line, &r, err, sizeof err) != RX_RC_OK) { rx_fail(m, RX_RC_FAIL, "%s", err); return; }
    switch (r.cmd) {
    case RXC_VERSION:
        snprintf(rxout, sizeof rxout, "PkgManager %s ARexx %d sha256 %s", PM_VERSION, RX_INTERFACE, self_sha());
        rx_ok(m, rxout); break;
    case RXC_HELP:         rx_ok(m, rx_help()); break;
    case RXC_LIST:         rx_list(m, &r); break;
    case RXC_INFO:         rx_info(m, &r); break;
    case RXC_REQUIREMENTS: rx_requirements(m, &r); break;
    case RXC_SHOW:         rx_show(m, &r); break;
    case RXC_OUTDATED:     rx_outdated(m, &r); break;
    case RXC_UPDATE: case RXC_INSTALL: case RXC_REMOVE: case RXC_UPGRADE: case RXC_ROLLBACK:
    case RXC_RUN: case RXC_VERIFY:
        rx_start(m, &r); break;
    case RXC_JOB: case RXC_WAIT: rx_jobcmd(m, &r); break;
    case RXC_CANCEL:       rx_cancel(m, &r); break;
    case RXC_QUIT:
        /* The same as the close gadget, and as safe. */
        if (!rexx_prepare(m, "closing")) { rx_fail(m, RX_RC_ERROR, "NOMEM no memory for the answer; nothing was done"); break; }
        if (confirming) decline("declined (ARexx QUIT)");
        if (in_flight) close_requested(); else rexx_quit = 1;
        rx_ok(m, "closing");
        break;
    case RXC_LASTERROR:
        {
            char text[600];
            snprintf(text, sizeof text, "%s", rexx_lasterror(m));
            logline("arexx< rc=%d lasterror %s", rexx_reply(m, RX_RC_OK, text, REXX_KEEP), text);
        }
        break;
    default: rx_fail(m, RX_RC_FAIL, "BADCOMMAND"); break;
    }
}

/* Quiet lasts for the one command: a WAIT answered later, when its job
   ends, is always logged. */
static void rexx_command(rexx_msg *m, const char *line)
{
    rexx_dispatch(m, line);
    quiet = 0;
}

/* ------------------------------------------------------------------ main */

static int real_main(int argc, char **argv)
{
    ULONG sigs = 0, portsig, rxsig;
    IPTR ret;
    int i, quit = 0;
    const char *rexxlib = NULL;
    static char title[64];

    int after_update = 0;

    wb_start = argc == 0;
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--after-update")) after_update = 1;
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--log") && i + 1 < argc) {
            /* Started again after an update: the same log goes on. */
            snprintf(log_path, sizeof log_path, "%s", argv[++i]);
            logf = fopen(log_path, after_update ? "a" : "w");
        }
        else if (!strcmp(argv[i], "--after-update")) ;
        else if (!strcmp(argv[i], "--release-from") && i + 1 < argc) su_set_base(argv[++i]);  /* tests */
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) snprintf(root, sizeof root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--slow") && i + 1 < argc) job_set_slow(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--break-download-at") && i + 1 < argc)   /* tests */
            pkg_test_break_download(atol(argv[++i]), 1);
        else if (!strcmp(argv[i], "--corrupt-download-at") && i + 1 < argc) /* tests */
            pkg_test_break_download(atol(argv[++i]), 2);
        else if (!strcmp(argv[i], "--yes")) auto_yes = 1;        /* tests: no requester to click */
        else if (!strcmp(argv[i], "--demo") && i + 1 < argc) {
            snprintf(demo_path, sizeof demo_path, "%s", argv[++i]);
            snprintf(demo_arg, sizeof demo_arg, "%s", demo_path);
        }
        else if (!strcmp(argv[i], "--updates") || !strcasecmp(argv[i], "UPDATES")) start_updates = 1;
        else if (!strcmp(argv[i], "--rexxlib") && i + 1 < argc) rexxlib = argv[++i];  /* tests: a machine without ARexx */
        else if (!strcmp(argv[i], "--test-nomem") && i + 1 < argc) {                   /* tests: allocation failures */
            const char *w = argv[++i];
            if (!strcmp(w, "start")) job_test_nomem();
            else if (!strcmp(w, "result")) rexx_test_nomem();
            else if (!strcmp(w, "prepare")) rexx_test_nomem_prepare();
        }

    snprintf(index_path, sizeof index_path, "%s/db/index.json", root);
    {
        /* This program's file and drawer, as DOS names them: what a
           self-update replaces, and what this window announces while it
           runs so that apkg can tell it is in use. */
        char name[256] = "";
        if (!GetProgramName((STRPTR)name, sizeof name) || !name[0])
            snprintf(name, sizeof name, "%s", FindTask(NULL)->tc_Node.ln_Name ? FindTask(NULL)->tc_Node.ln_Name : "PkgManager");
        if (GetProgramDir() && NameFromLock(GetProgramDir(), (STRPTR)self_drawer, sizeof self_drawer)) {
            snprintf(sujob.program, sizeof sujob.program, "%s", self_drawer);
            AddPart((STRPTR)sujob.program, FilePart((STRPTR)name), sizeof sujob.program);
        }
        su_announce_start(self_drawer, announce_nonce, sizeof announce_nonce);
    }
    {
        /* The demo storefront is optional: without it the window has no
           pictures and no Demo store, and says nothing more about it. */
        char why[200];
        if (demo_load(demo_path, why, sizeof why) == 0) state_entries[ST_DEMO] = (STRPTR)"Demo store";
        else logline("no demo storefront: %s", why);
    }
    display_hook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(display_func);
    cat_entries[0] = (STRPTR)"All categories"; cat_entries[1] = NULL;

    port = CreateMsgPort();
    if (!port) { su_announce_stop(); return RETURN_FAIL; }
    portsig = 1UL << port->mp_SigBit;

    rexx_open(rexxlib);
    rxsig = rexx_sigmask();
    if (logf) logline("start PkgManager %s sha256 %s root %s: %s", PM_VERSION, self_sha(), root, rexx_state());
    snprintf(title, sizeof title, "%s", rexx_active() ? "PkgManager" : "PkgManager (no ARexx port)");

    app = ApplicationObject,
        MUIA_Application_Title,       (IPTR)"PkgManager",
        MUIA_Application_Author,      (IPTR)AROSPKG_AUTHOR,
        MUIA_Application_Version,     (IPTR)"$VER: PkgManager " PM_VERSION " (12.10.2026)",
        MUIA_Application_Description, (IPTR)"Install software from AROS Archives",
        MUIA_Application_Base,        (IPTR)"PKGMANAGER",
        SubWindow, (win = WindowObject,
            MUIA_Window_Title, (IPTR)title,
            MUIA_Window_ID,    MAKE_ID('P','K','G','M'),
            /* A fixed first placement: the list needs the width, and the
               acceptance run clicks by coordinates read off a screenshot. */
            MUIA_Window_LeftEdge, 100, MUIA_Window_TopEdge, 40,
            MUIA_Window_Width, 800,   MUIA_Window_Height, 600,
            WindowContents, (VGroup,
                Child, (HGroup,
                    Child, (str_search = StringObject,
                        MUIA_Frame, MUIV_Frame_String,
                        MUIA_CycleChain, 1,
                    End),
                    Child, (cyc_cat = CycleObject,
                        MUIA_Cycle_Entries, (IPTR)cat_entries,
                    End),
                    Child, (cyc_state = CycleObject,
                        MUIA_Cycle_Entries, (IPTR)state_entries,
                    End),
                    Child, (btn_update = SimpleButton("Update index")),
                    Child, (btn_about = SimpleButton("About")),
                End),
                Child, (txt_catalogue = TextObject,
                    MUIA_Text_Contents, (IPTR)"",
                End),
                Child, (lst = ListviewObject,
                    MUIA_Listview_List, (ListObject,
                        MUIA_Frame,           MUIV_Frame_InputList,
                        MUIA_List_Format,     (IPTR)"BAR,BAR,BAR,",
                        MUIA_List_Title,      TRUE,
                        MUIA_List_DisplayHook, (IPTR)&display_hook,
                    End),
                End),
                /* Details on the left, pictures on the right. The picture
                   panel has fixed places (demo.h) and is hidden for a
                   package without pictures, so the text takes the width;
                   at 800x600, the default size, everything fits a 1024x768
                   screen with the list keeping about nine rows. */
                Child, (HGroup,
                    Child, (VGroup,
                        Child, (HGroup,
                            Child, (pg_icon = PageGroup,
                                MUIA_FixWidth, DEMO_ICON_W, MUIA_FixHeight, DEMO_ICON_H,
                                Child, (box_icon = VGroup, MUIA_InnerLeft, 0, MUIA_InnerRight, 0,
                                    MUIA_InnerTop, 0, MUIA_InnerBottom, 0,
                                    Child, (pic_icon = DtpicObject, End), End),
                                Child, (RectangleObject, End),
                            End),
                            /* Three lines always: the row must not change
                               height with its content or with whether an
                               icon is shown (on mainline without one, it
                               kept the height of a single line). */
                            Child, (txt_head = TextObject,
                                MUIA_Text_Contents, (IPTR)"",
                                MUIA_Text_SetMin, FALSE,
                                MUIA_FixHeightTxt, (IPTR)"A\nA\nA",
                            End),
                        End),
                        /* Not of fixed height: it takes whatever the
                           picture panel beside it leaves, so the header
                           above stays at the top. */
                        Child, (txt_detail = FloattextObject,
                            MUIA_Frame, MUIV_Frame_ReadList,
                            MUIA_Floattext_Text, (IPTR)"",
                        End),
                    End),
                    Child, (grp_media = VGroup,
                        MUIA_ShowMe, FALSE,
                        MUIA_Frame, MUIV_Frame_Group,
                        MUIA_HorizWeight, 0,
                        Child, (pg_shot = PageGroup,
                            MUIA_FixWidth, DEMO_SHOT_W, MUIA_FixHeight, DEMO_SHOT_H,
                            Child, (box_shot = VGroup, MUIA_InnerLeft, 0, MUIA_InnerRight, 0,
                                MUIA_InnerTop, 0, MUIA_InnerBottom, 0,
                                Child, (pic_shot = DtpicObject, End), End),
                            Child, (txt_noshot = TextObject,
                                MUIA_Text_Contents, (IPTR)"",
                                MUIA_Text_PreParse, (IPTR)"\33c",
                            End),
                        End),
                        Child, (txt_caption = TextObject,
                            MUIA_Text_Contents, (IPTR)"",
                            MUIA_Text_SetMin, FALSE,
                        End),
                        Child, (grp_thumbs = HGroup,
                            Child, (box_thumb[0] = VGroup, MUIA_InnerLeft, 0, MUIA_InnerRight, 0,
                                MUIA_InnerTop, 0, MUIA_InnerBottom, 0,
                                Child, (pic_thumb[0] = DtpicObject, MUIA_InputMode, MUIV_InputMode_RelVerify, End), End),
                            Child, (box_thumb[1] = VGroup, MUIA_InnerLeft, 0, MUIA_InnerRight, 0,
                                MUIA_InnerTop, 0, MUIA_InnerBottom, 0,
                                Child, (pic_thumb[1] = DtpicObject, MUIA_InputMode, MUIV_InputMode_RelVerify, End), End),
                            Child, (box_thumb[2] = VGroup, MUIA_InnerLeft, 0, MUIA_InnerRight, 0,
                                MUIA_InnerTop, 0, MUIA_InnerBottom, 0,
                                Child, (pic_thumb[2] = DtpicObject, MUIA_InputMode, MUIV_InputMode_RelVerify, End), End),
                            Child, (box_thumb[3] = VGroup, MUIA_InnerLeft, 0, MUIA_InnerRight, 0,
                                MUIA_InnerTop, 0, MUIA_InnerBottom, 0,
                                Child, (pic_thumb[3] = DtpicObject, MUIA_InputMode, MUIV_InputMode_RelVerify, End), End),
                            Child, HSpace(0),
                        End),
                        Child, (txt_provenance = TextObject,
                            MUIA_Text_Contents, (IPTR)"",
                            MUIA_Text_SetMin, FALSE,
                            MUIA_Font, MUIV_Font_Tiny,
                        End),
                    End),
                End),
                Child, (HGroup,
                    Child, (btn_install  = SimpleButton("Install")),
                    Child, (btn_run      = SimpleButton("Run")),
                    Child, (btn_open     = SimpleButton("Open folder")),
                    Child, (btn_verify   = SimpleButton("Verify")),
                    Child, (btn_upgrade  = SimpleButton("Upgrade")),
                    Child, (btn_rollback = SimpleButton("Roll back")),
                    Child, (btn_remove   = SimpleButton("Remove")),
                    Child, (btn_buy      = SimpleButton("Buy...")),
                    Child, (btn_cancel   = SimpleButton("Cancel")),
                End),
                Child, (gauge = GaugeObject,
                    MUIA_Frame, MUIV_Frame_Gauge,
                    MUIA_Gauge_Horiz, TRUE,
                    MUIA_Gauge_Max, 100,
                    MUIA_Gauge_Current, 0,
                    MUIA_Gauge_InfoText, (IPTR)"",
                End),
                Child, (txt_status = TextObject,
                    MUIA_Frame, MUIV_Frame_Text,
                    MUIA_Text_Contents, (IPTR)"",
                End),
            End),
        End),
        SubWindow, (win_about = WindowObject,
            MUIA_Window_Title, (IPTR)"About PkgManager",
            MUIA_Window_ID, MAKE_ID('P','K','G','A'),
            WindowContents, (VGroup,
                Child, (TextObject,
                    MUIA_Text_Contents, (IPTR)("PkgManager " PM_VERSION "\n\n" AROSPKG_ABOUT),
                End),
                Child, (txt_release = TextObject,
                    MUIA_Frame, MUIV_Frame_Text,
                    /* Five lines: the height every later answer fits in. */
                    MUIA_Text_Contents, (IPTR)"Check for a newer version, then Update arospkg\n"
                                              "updates PkgManager and apkg together.\n\n\n",
                End),
                Child, (HGroup,
                    Child, (btn_check = SimpleButton("Check for a newer version")),
                    Child, (btn_selfupdate = SimpleButton("Update arospkg")),
                    Child, (btn_about_close = SimpleButton("Close")),
                End),
            End),
        End),
        SubWindow, (win_confirm = WindowObject,
            MUIA_Window_Title, (IPTR)"PkgManager: confirm",
            MUIA_Window_ID,    MAKE_ID('P','K','G','C'),
            /* Fixed, like the main window: the acceptance run presses
               Proceed by coordinates. */
            MUIA_Window_LeftEdge, 260, MUIA_Window_TopEdge, 180,
            MUIA_Window_Width, 440,
            WindowContents, (VGroup,
                Child, (txt_confirm = FloattextObject,
                    MUIA_Frame, MUIV_Frame_ReadList,
                    MUIA_FixHeightTxt, (IPTR)"\n\n\n\n\n\n\n\n\n\n\n",
                    MUIA_Floattext_Text, (IPTR)"",
                End),
                Child, (HGroup,
                    Child, (btn_proceed = SimpleButton("Proceed")),
                    Child, (btn_decline = SimpleButton("Cancel")),
                End),
            End),
        End),
    End;

    if (!app) { rexx_close("CLOSING PkgManager could not open its window"); DeleteMsgPort(port); su_announce_stop(); return RETURN_FAIL; }

    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             (IPTR)app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(str_search, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_SEARCH);
    DoMethod(cyc_cat,   MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, (IPTR)app, 2, MUIM_Application_ReturnID, ID_FILTER);
    DoMethod(cyc_state, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, (IPTR)app, 2, MUIM_Application_ReturnID, ID_FILTER);
    DoMethod(btn_update,  MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_UPDATE);
    DoMethod(btn_about, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_ABOUT);
    DoMethod(btn_about_close, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_ABOUT_CLOSE);
    DoMethod(btn_check, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_CHECK);
    DoMethod(btn_selfupdate, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_SELFUPDATE);
    DoMethod(win_about, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_ABOUT_CLOSE);
    DoMethod(btn_install, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_INSTALL);
    DoMethod(btn_remove,  MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_REMOVE);
    DoMethod(btn_open,    MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_OPEN);
    DoMethod(btn_run,     MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_RUN);
    DoMethod(btn_verify,  MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_VERIFY);
    DoMethod(btn_buy,     MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_BUY);
    for (i = 0; i < DEMO_MAXSHOTS; i++)
        DoMethod(pic_thumb[i], MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_THUMB0 + i);
    DoMethod(btn_cancel,  MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_CANCEL);
    DoMethod(btn_upgrade, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_UPGRADE);
    DoMethod(btn_rollback,MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_ROLLBACK);
    DoMethod(btn_proceed, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_PROCEED);
    DoMethod(btn_decline, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_DECLINE);
    DoMethod(win_confirm, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_DECLINE);
    DoMethod(lst, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_SELECT);

    SetAttrs(btn_install,  MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_remove,   MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_cancel,   MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_upgrade,  MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_rollback, MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_open,     MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_run,      MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_verify,   MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_selfupdate, MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(btn_buy,      MUIA_Disabled, TRUE, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(win, MUIA_Window_Open, TRUE, TAG_DONE);
    if (start_updates) SetAttrs(cyc_state, MUIA_NoNotify, TRUE, MUIA_Cycle_Active, ST_UPDATES, TAG_DONE);
    refill("", NULL, start_updates ? "start updates" : "start");
    su_at_start();

    while (!quit) {
        ret = DoMethod(app, MUIM_Application_NewInput, (IPTR)&sigs);
        if (ret == MUIV_Application_ReturnID_Quit) {
            if (confirming) { decline("declined (window closed)"); break; }
            if (in_flight) close_requested(); else break;
        }
        switch (ret) {
        case ID_ABOUT:
            SetAttrs(win_about, MUIA_Window_Open, TRUE, MUIA_Window_Activate, TRUE, TAG_DONE);
            logline("about opened");
            break;
        case ID_ABOUT_CLOSE:
            SetAttrs(win_about, MUIA_Window_Open, FALSE, TAG_DONE);
            logline("about closed");
            break;
        case ID_SEARCH: search_acknowledged(); break;
        case ID_FILTER: {
            IPTR a = 0, st = 0;
            GetAttr(MUIA_Cycle_Active, cyc_cat, &a);
            GetAttr(MUIA_Cycle_Active, cyc_state, &st);
            snprintf(cat_chosen, sizeof cat_chosen, "%s", a ? cat_names[a - 1] : "");
            logline("filter category=%s state=%s", cat_chosen[0] ? cat_chosen : "all", state_entries[st]);
            SetAttrs(btn_buy, MUIA_ShowMe, st == ST_DEMO, TAG_DONE);
            refill_keep("filter");
        } break;
        case ID_SELECT:  show_detail(); break;
        case ID_UPDATE:  logline("update pressed"); begin(JOB_UPDATE, NULL, "window"); break;
        case ID_INSTALL: { pkg_entry *e = selected(); if (e) { logline("install pressed for %s", e->id); begin(JOB_INSTALL, e->id, "window"); } } break;
        case ID_REMOVE:  { pkg_entry *e = selected(); if (e) { logline("remove pressed for %s", e->id); begin(JOB_REMOVE, e->id, "window"); } } break;
        case ID_OPEN:    { pkg_entry *e = selected(); if (e) open_folder(e->id); } break;
        case ID_RUN:     { pkg_entry *e = selected(); if (e) { logline("run pressed for %s", e->id); begin(JOB_RUN, e->id, "window"); } } break;
        case ID_VERIFY:  { pkg_entry *e = selected(); if (e) { logline("verify pressed for %s", e->id); begin(JOB_VERIFY, e->id, "window"); } } break;
        case ID_UPGRADE: { pkg_entry *e = selected(); if (e) { logline("upgrade pressed for %s", e->id); begin(JOB_PREVIEW_UPGRADE, e->id, "window"); } } break;
        case ID_ROLLBACK:{ pkg_entry *e = selected(); if (e) { logline("rollback pressed for %s", e->id); begin(JOB_PREVIEW_ROLLBACK, e->id, "window"); } } break;
        case ID_CHECK:   logline("check for a newer version pressed"); begin(JOB_CHECK_RELEASE, NULL, "window"); break;
        case ID_SELFUPDATE: logline("update arospkg pressed"); su_begin(); break;
        case ID_CANCEL:  if (in_flight) { cur.cancel = 1; if (active) active->cancel_asked = 1; status("cancelling..."); logline("cancel pressed for %s", cur.id); } break;
        case ID_PROCEED: proceed("confirmed"); break;
        case ID_DECLINE: if (buy_showing) buy_close(); else decline("declined"); break;
        case ID_BUY:     { pkg_entry *e = selected(); demo_offer *o = demo_offer_of(e);
                           if (o && demo_is_offer(e)) buy_preview(o); } break;
        case ID_THUMB0: case ID_THUMB1: case ID_THUMB2: case ID_THUMB3:
            show_shot((int)(ret - ID_THUMB0)); break;
        default: break;
        }
        if (sigs) {
            sigs = Wait(sigs | portsig | rxsig | SIGBREAKF_CTRL_C);
            if (sigs & SIGBREAKF_CTRL_C) {
                if (confirming) { decline("declined (break)"); break; }
                if (in_flight) close_requested(); else break;
            }
            if (sigs & portsig) {
                pm_msg *m;
                while ((m = (pm_msg *)GetMsg(port))) { if (on_message(m)) quit = 1; FreeVec(m); }
            }
            if (sigs & rxsig) rexx_poll(rexx_command);
        }
        if (rexx_quit && !busy_now()) break;
        if (su_handed_off) break;
    }

    /* The worker's last message says the operation ended; whether the task
       has left the system is a separate fact, measured here before the code
       it ran is unloaded. */
    logline("worker task gone %ld ms after done", job_wait_gone(3000, still_waiting));
    for (i = 0; i < MAXWAITS; i++)
        if (waits[i].m) { rexx_reply(waits[i].m, RX_RC_ERROR, NULL, "CLOSING PkgManager has quit"); waits[i].m = NULL; }
    rexx_close("CLOSING PkgManager has quit");
    MUI_DisposeObject(app);
    pkg_entries_free(&rows);
    pkg_updates_free(&upd);
    DeleteMsgPort(port);
    logline("quit");
    /* Last: an update waiting for this window starts once this is gone. */
    su_announce_stop();
    if (logf) fclose(logf);
    return RETURN_OK;
}

int main(int argc, char **argv)
{
    return pkg_run_with_stack(real_main, argc, argv);
}
