/* PkgUpdates -- a ScreenBar item showing how many installed packages have
 * updates, and opening PkgManager on them. A demonstration provider for the
 * experimental ScreenBar executable-provider SDK (protocol 1), which this
 * program uses as it is; nothing here extends it.
 *
 * The count is the same model `apkg outdated` and PkgManager use
 * (pkg_updates_scan in libpkg), read directly rather than by parsing apkg's
 * printout. Two rhythms, kept apart on purpose:
 *   - publishing: once a second, because the host drops an item after five
 *     seconds without an update (its lease); it only re-sends a string;
 *   - recounting: when the catalogue or the registry has changed (their
 *     dates, checked every ten seconds), and otherwise every --recheck
 *     minutes. Never a download: the catalogue is what apkg update or
 *     PkgManager last fetched, and its age is part of what is shown.
 * Neither arospkg nor PkgManager needs this program or ScreenBar. It does
 * not install itself anywhere; start it by hand or from your own startup.
 */
#include "../libpkg/pkg.h"
#include "screenbar.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/rexxsyslib.h>
#include <rexx/storage.h>
#include <rexx/errors.h>
#include <dos/dostags.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char version[] __attribute__((used)) = "$VER: PkgUpdates 0.5.0 (12.10.2026)";
#define PKGMANAGER_PORT "PKGMANAGER"   /* src/pkgmanager/rexxcmd.h, RX_PORTNAME */

struct RxsLib *RexxSysBase;
static FILE *logf;
static void logline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#include <stdarg.h>
static void logline(const char *fmt, ...)
{
    va_list ap;
    if (!logf) return;
    {
        time_t t = time(NULL); struct tm *g = gmtime(&t); char w[24] = "";
        if (g) strftime(w, sizeof w, "%H:%M:%S", g);
        fprintf(logf, "%s ", w);
    }
    va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap);
    fputc('\n', logf); fflush(logf);
}

/* A date that changes whenever the file or drawer is rewritten: the
   catalogue on apkg update, the registry drawer on install, upgrade,
   rollback and remove. */
static long stamp(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    struct FileInfoBlock *fib;
    long s = -1;
    if (!l) return -1;
    if ((fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL))) {
        if (Examine(l, fib))
            s = fib->fib_Date.ds_Days * 86400L + fib->fib_Date.ds_Minute * 60L + fib->fib_Date.ds_Tick / TICKS_PER_SECOND;
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(l);
    return s;
}

/* What the item says, within the SDK's 63 printable ASCII bytes. A missing
   catalogue is said as such: "0" would read as "nothing to update". */
static void describe(const pkg_updates *u, int failed, char *out, size_t n)
{
    if (failed) { snprintf(out, n, "cannot check"); return; }
    if (u->catalogue == PKG_CAT_MISSING) { snprintf(out, n, "no catalogue"); return; }
    if (u->catalogue == PKG_CAT_UNREADABLE) { snprintf(out, n, "catalogue unreadable"); return; }
    if (!u->installed) snprintf(out, n, "none installed");
    else if (!u->newer) snprintf(out, n, "up to date");
    else if (!u->ready) snprintf(out, n, "%d blocked", u->newer);
    else if (u->blocked) snprintf(out, n, "%d available (%d blocked)", u->newer, u->blocked);
    else snprintf(out, n, "%d available", u->newer);
    if (u->catalogue_stale) {
        size_t k = strlen(out);
        snprintf(out + k, n - k, " (old catalogue)");
    }
}

/* AmigaDOS quoting for a path inside "...": '"' and '*' get a '*'. */
static void quoted(char *out, size_t n, const char *s)
{
    size_t k = 0;
    if (n < 3) { if (n) out[0] = 0; return; }
    out[k++] = '"';
    for (; *s && k + 3 < n; s++) {
        if (*s == '"' || *s == '*') out[k++] = '*';
        out[k++] = *s;
    }
    out[k++] = '"'; out[k] = 0;
}

/* The ARexx request in flight, if any. The item's loop must keep publishing
   while PkgManager answers: the host drops an item after five seconds
   without an update, and PkgManager answers only when its window loop gets
   to the message, which a long operation or a requester can delay. So the
   request is sent and its answer collected by the loop, never waited for. */
static struct { struct MsgPort *port; struct RexxMsg *rm; time_t sent; } ask;

static void ask_free(void)
{
    if (ask.rm) {
        if (ask.rm->rm_Result1 == 0 && ask.rm->rm_Result2) DeleteArgstring((UBYTE *)ask.rm->rm_Result2);
        DeleteArgstring((UBYTE *)ask.rm->rm_Args[0]);
        DeleteRexxMsg(ask.rm);
    }
    if (ask.port) DeleteMsgPort(ask.port);
    ask.port = NULL; ask.rm = NULL;
}

/* Called every turn of the loop: takes the answer if it has come. */
static void ask_poll(void)
{
    if (!ask.port || !GetMsg(ask.port)) return;
    logline("action: the running PkgManager answered OUTDATED SHOW, rc=%ld", (long)ask.rm->rm_Result1);
    ask_free();
}

/* On exit. The message belongs to PkgManager until it is replied, so it
   cannot be freed before that. Wait a while; if no answer comes, leave the
   message and its port allocated, the port set to ignore the late reply, so
   the answer lands in memory that is still valid instead of signalling a
   task that has gone. A few hundred bytes, only if PkgManager is stuck. */
static void ask_finish(void)
{
    int i;
    if (!ask.port) return;
    for (i = 0; i < 100 && !GetMsg(ask.port); i++) Delay(5);
    if (i < 100) { logline("action: answer collected at exit"); ask_free(); return; }
    Forbid();
    if (!GetMsg(ask.port)) {
        ask.port->mp_Flags = PA_IGNORE;
        Permit();
        logline("action: no answer from PkgManager in 10 s; its request is left allocated");
        ask.port = NULL; ask.rm = NULL;
        return;
    }
    Permit();
    ask_free();
}

/* The one action. A running PkgManager is asked over its ARexx port to show
   its Updates filter (OUTDATED SHOW), so a second window is not opened;
   otherwise PkgManager is started with --updates, detached. */
static void open_pkgmanager(const char *pkgmanager, const char *root, int custom_root)
{
    struct MsgPort *pm = NULL;
    if (ask.port) {
        logline("action: ignored, PkgManager has not answered the last one (%lds)", (long)(time(NULL) - ask.sent));
        return;
    }
    if (RexxSysBase) {
        struct MsgPort *reply = CreateMsgPort();
        struct RexxMsg *rm = reply ? CreateRexxMsg(reply, NULL, NULL) : NULL;
        if (rm && (rm->rm_Args[0] = (IPTR)CreateArgstring((STRPTR)"OUTDATED SHOW", 13))) {
            rm->rm_Action = RXCOMM | RXFF_RESULT;
            Forbid();
            pm = FindPort((CONST_STRPTR)PKGMANAGER_PORT);
            if (pm) PutMsg(pm, &rm->rm_Node);
            Permit();
            if (pm) {
                ask.port = reply; ask.rm = rm; ask.sent = time(NULL);
                logline("action: asked the running PkgManager (OUTDATED SHOW)");
                return;
            }
            DeleteArgstring((UBYTE *)rm->rm_Args[0]);
        }
        if (rm) DeleteRexxMsg(rm);
        if (reply) DeleteMsgPort(reply);
    }
    {
        char cmd[3 * PKG_MAXPATH], qp[PKG_MAXPATH + 8], qr[PKG_MAXPATH + 8];
        BPTR in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE), out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
        LONG rc;
        quoted(qp, sizeof qp, pkgmanager);
        quoted(qr, sizeof qr, root);
        if (custom_root) snprintf(cmd, sizeof cmd, "%s --updates --root %s", qp, qr);
        else snprintf(cmd, sizeof cmd, "%s --updates", qp);
        rc = SystemTags((CONST_STRPTR)cmd, SYS_Input, (IPTR)in, SYS_Output, (IPTR)out,
                        SYS_Asynch, TRUE, NP_StackSize, 65536, TAG_DONE);
        logline("action: started %s (rc %ld)", cmd, (long)rc);
        /* With SYS_Asynch the handles belong to the new process on success. */
        if (rc == -1) { if (in) Close(in); if (out) Close(out); }
    }
}

static void usage(void)
{
    printf("Usage: PkgUpdates [--root DIR] [--label TEXT] [--recheck MINUTES]\n"
           "                  [--pkgmanager PATH] [--seconds N] [--log FILE]\n"
           "A ScreenBar item counting updates for installed packages. Ctrl-C ends it.\n");
}

static int real_main(int argc, char **argv)
{
    const char *root = "SYS:Packages", *label = "Updates", *pkgmanager = "PkgManager";
    char index[PKG_MAXPATH], installed[PKG_MAXPATH], text[SB_TEXT_SIZE] = "checking";
    long recheck = 15 * 60, seconds = 0, last_index = -2, last_reg = -2;
    int custom_root = 0, i, rc = 0, failed = 0, have = 0;
    time_t start = time(NULL), counted = 0, looked = 0, retry_at = 0;
    struct SbClient sb;
    pkg_updates u;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) { root = argv[++i]; custom_root = 1; }
        else if (!strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "--recheck") && i + 1 < argc) recheck = atol(argv[++i]) * 60;
        else if (!strcmp(argv[i], "--pkgmanager") && i + 1 < argc) pkgmanager = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atol(argv[++i]);   /* tests */
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) { if (!(logf = fopen(argv[++i], "w"))) return 10; }
        else { usage(); return 5; }
    }
    if (recheck < 60) recheck = 60;
    snprintf(index, sizeof index, "%s/db/index.json", root);
    snprintf(installed, sizeof installed, "%s/db/installed", root);
    memset(&u, 0, sizeof u);
    RexxSysBase = (struct RxsLib *)OpenLibrary((CONST_STRPTR)"rexxsyslib.library", 0);
    if (!sb_client_open(&sb)) { if (RexxSysBase) CloseLibrary((struct Library *)RexxSysBase); return 20; }
    logline("start root=%s recheck=%lds rexx=%s", root, recheck, RexxSysBase ? "yes" : "no");

    for (;;) {
        time_t now = time(NULL);
        unsigned action = 0;
        int st;
        if (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) { logline("Ctrl-C"); break; }
        if (seconds > 0 && now - start >= seconds) { logline("time limit"); break; }

        /* Recount when something changed, or when it is time to. */
        if (!have || now - looked >= 10) {
            long si = stamp(index), sr = stamp(installed);
            looked = now;
            if (!have || si != last_index || sr != last_reg || now - counted >= recheck) {
                pkg_err e; memset(&e, 0, sizeof e);
                pkg_updates_free(&u);
                failed = pkg_updates_scan(root, index, 1, &u, &e) != PKG_OK;
                describe(&u, failed, text, sizeof text);
                logline("count: catalogue=%s when=%s installed=%d newer=%d ready=%d blocked=%d -> \"%s\"%s%s",
                        pkg_cat_word(u.catalogue), u.catalogue_when, u.installed, u.newer, u.ready, u.blocked,
                        text, failed ? " error: " : "", failed ? e.summary : "");
                last_index = si; last_reg = sr; counted = now; have = 1;
            }
        }

        /* Publish. After the host went away or restarted, register again at
           a modest pace rather than every second. */
        if (sb.token) st = sb_client_update(&sb, label, text, &action);
        else if (now >= retry_at) {
            st = sb_client_register(&sb, label, text, "Open PkgManager");
            if (st == SB_OK) logline("registered token=%u", (unsigned)sb.token);
            else retry_at = now + 5;
        } else st = SB_ABSENT;
        ask_poll();
        if (st == SB_OK && action == SB_ACTION_PRIMARY) open_pkgmanager(pkgmanager, root, custom_root);
        if (st == SB_STALE || st == SB_STOPPED) { logline("host restarted or dropped the item (%d); registering again", st); retry_at = now; }
        if (st == SB_INVALID || st == SB_VERSION) { logline("host refused the item (%d)", st); rc = 10; break; }
        Delay(50);    /* one second: well inside the five-second lease */
    }
    sb_client_close(&sb);       /* unregisters and frees the reply port */
    ask_finish();
    pkg_updates_free(&u);
    if (RexxSysBase) CloseLibrary((struct Library *)RexxSysBase);
    logline("clean exit rc=%d", rc);
    if (logf) fclose(logf);
    return rc;
}

/* The model's requirement check keeps a few kilobytes on the stack; started
   from an icon this program would have the default 4 KB. The same swap as
   apkg and PkgManager. */
int main(int argc, char **argv)
{
    return pkg_run_with_stack(real_main, argc, argv);
}
