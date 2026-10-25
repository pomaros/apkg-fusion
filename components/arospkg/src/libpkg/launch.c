/* Starting an installed application: `apkg run`, PkgManager's Run button and
 * ARexx RUN. The description is the drawer's application.json (app.c); this
 * file checks it against the disk and starts the program.
 *
 * Workbench mode goes through workbench.library's OpenWorkbenchObjectA with
 * the program's full path. That is what Wanderer does for a double-click:
 * the library reads the program's icon for the stack (never below the
 * desktop's default) and TOOLPRI, starts the program with a WBStartup
 * message carrying the icon's tool types, and makes the program's drawer its
 * current directory (workbench/libs/workbench/openworkbenchobjecta.c,
 * handler.c, the same in ABIv11 and mainline). No Shell command is built, so
 * spaces in paths need no quoting. Two traps in that path decide what is
 * checked first:
 *
 *   - Without an icon, or with a CLI tool type, the library starts the
 *     program as a CLI command with the default stack: a program that needs
 *     8 MiB then crashes. So workbench mode requires a program icon.
 *   - The library's handler loads the program on its own process and drops a
 *     failed LoadSeg silently ("FIXME: report error somehow?"), while the
 *     call has already returned TRUE. So the CPU of the file is checked
 *     before the call, and success is claimed only when a new process of
 *     the program's name has been seen.
 *
 * Shell mode runs `"<program>" <args>` through SystemTags in the declared
 * working directory, with the program icon's stack when it has one; the icon
 * stays the one place a stack is written down. */
#include "internal.h"
#include "app.h"
#include "json.h"
#include "util.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <proto/workbench.h>
#include <exec/execbase.h>
#include <dos/dostags.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long a Workbench start may take to show a process. Folio's 40 MB
   binary took several seconds to load under QEMU TCG; a program that has
   not appeared by then is reported as not seen, never as started. */
#define RUN_CONFIRM_TICKS (20 * 50)

const char *pkg_run_word(pkg_run_state s)
{
    switch (s) {
    case PKG_RUN_READY:          return "ready";
    case PKG_RUN_NO_DESCRIPTION: return "no-description";
    case PKG_RUN_INVALID:        return "invalid";
    case PKG_RUN_MISSING:        return "missing";
    case PKG_RUN_INCOMPATIBLE:   return "incompatible";
    case PKG_RUN_REQUIRES:       return "requires";
    case PKG_RUN_NOT_INSTALLED:  return "not-installed";
    }
    return "?";
}

static pkg_status set_state(pkg_app_info *a, pkg_run_state s, const char *why)
{
    a->state = s;
    snprintf(a->why, sizeof a->why, "%s", why);
    return PKG_OK;
}

/* The canonical name of a path, through a lock, so a link that leads
   elsewhere shows up as elsewhere. 0 on success. */
static int real_name(const char *path, char *out, size_t n, int *isdir)
{
    struct FileInfoBlock *fib;
    BPTR l = Lock((CONST_STRPTR)path, ACCESS_READ);
    int ok = -1;
    if (!l) return -1;
    if (NameFromLock(l, (STRPTR)out, (LONG)n)) ok = 0;
    if (isdir && (fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL)) != NULL) {
        *isdir = Examine(l, fib) ? (fib->fib_DirEntryType > 0) : -1;
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(l);
    return ok;
}

/* `inner` is `outer` or below it. Names compare without case, as AROS
   file systems do. */
static int inside(const char *outer, const char *inner)
{
    size_t k = strlen(outer);
    if (strncasecmp(outer, inner, k) != 0) return 0;
    if (inner[k] == 0) return 1;
    if (outer[k - 1] == ':' || outer[k - 1] == '/') return 1;
    return inner[k] == '/';
}

static long protection_of(const char *path)
{
    struct FileInfoBlock *fib;
    long prot = -1;
    BPTR l = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (!l) return -1;
    if ((fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL)) != NULL) {
        if (Examine(l, fib)) prot = fib->fib_Protection;
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(l);
    return prot;
}

pkg_status pkg_app_info_get(pkg_ctx *c, const char *what, pkg_app_info *a, pkg_err *e)
{
    char path[PKG_MAXPATH], why[900], w[200], realdrawer[PKG_MAXPATH], realp[PKG_MAXPATH];
    char *txt;
    size_t len;
    app_desc d;
    req_ent reqs[MAXREQ];
    int nreq = 0, isdir = 0, i, by_path;
    long prot;
    BPTR fh;
    unsigned char head[64];
    LONG got;
    const char *bin;

    memset(a, 0, sizeof *a);
    snprintf(a->requirements, sizeof a->requirements, "none");
    by_path = strpbrk(what, ":/") != NULL;
    if (by_path) {
        snprintf(a->drawer, sizeof a->drawer, "%s", what);
        /* Until the description names it: the drawer's own name. */
        snprintf(a->name, sizeof a->name, "%s", (const char *)FilePart((STRPTR)what));
    } else {
        char regpath[PKG_MAXPATH], dir[PKG_MAXID] = "";
        js_tok *t;
        int ntok;
        if (!c) return pkg_fail(e, PKG_E_IO, "no package root is open", "", what);
        snprintf(a->id, sizeof a->id, "%s", what);
        snprintf(a->name, sizeof a->name, "%s", what);
        registry_path(c, what, regpath, sizeof regpath);
        if (!(txt = u_read_all(regpath, &len)))
            return set_state(a, PKG_RUN_NOT_INSTALLED, "It is not installed.");
        ntok = js_parse_alloc(txt, len, &t, PKG_JSON_MAXTOK);
        if (ntok > 0) {
            js_str(txt, t, js_member(txt, t, ntok, 0, "dir"), dir, sizeof dir);
            js_str(txt, t, js_member(txt, t, ntok, 0, "version"), a->version, sizeof a->version);
            a->revision = js_long(txt, t, js_member(txt, t, ntok, 0, "revision"), 0);
        }
        free(t); free(txt);
        u_join(a->drawer, sizeof a->drawer, c->root, dir[0] ? dir : what);
    }
    if (!u_isdir(a->drawer))
        return set_state(a, PKG_RUN_NOT_INSTALLED, by_path
                         ? "That drawer is not there."
                         : "The drawer recorded for it is not there; it was moved or deleted outside apkg.");

    u_join(path, sizeof path, a->drawer, PKG_APP_FILE);
    if (!u_exists(path))
        return set_state(a, PKG_RUN_NO_DESCRIPTION,
                         "It does not say how to start it (no application.json). Open folder shows its files.");
    if (u_size(path) > PKG_APP_MAXSIZE)
        return set_state(a, PKG_RUN_INVALID, "Its application.json is larger than 64 KiB.");
    if (!(txt = u_read_all(path, &len)))
        return set_state(a, PKG_RUN_INVALID, "Its application.json cannot be read.");
    if (app_parse(txt, len, &d, reqs, MAXREQ, &nreq, why, sizeof why) != 0) {
        free(txt);
        return set_state(a, PKG_RUN_INVALID, why);
    }
    free(txt);
    a->launch = d.launch;
    snprintf(a->name, sizeof a->name, "%s", d.name);
    if (by_path) {
        snprintf(a->id, sizeof a->id, "%s", d.id);
        snprintf(a->version, sizeof a->version, "%s", d.version);
        a->revision = d.revision;
    } else if (strcasecmp(d.id, a->id) != 0) {
        snprintf(why, sizeof why, "Its application.json describes \"%s\", not \"%s\".", d.id, a->id);
        return set_state(a, PKG_RUN_INVALID, why);
    }

    /* The target before the files: a description for another machine is
       the clearest reason there is. */
    if (pkg_compat_of(d.arch, d.abi, pkg_arch(), pkg_abi(), w, sizeof w) == PKG_COMPAT_INCOMPATIBLE) {
        snprintf(why, sizeof why, "It is %s.", w);
        return set_state(a, PKG_RUN_INCOMPATIBLE, why);
    }

    u_join(a->program, sizeof a->program, a->drawer, d.launch.executable);
    /* "." is the drawer itself. AmigaDOS has no "." component -- a path
       ending in "/." names a file called "." -- so it is not joined. */
    if (strcmp(d.launch.workdir, ".") == 0) snprintf(a->workdir, sizeof a->workdir, "%s", a->drawer);
    else u_join(a->workdir, sizeof a->workdir, a->drawer, d.launch.workdir);
    if (real_name(a->drawer, realdrawer, sizeof realdrawer, NULL) != 0)
        return set_state(a, PKG_RUN_MISSING, "The drawer cannot be read.");
    if (real_name(a->program, realp, sizeof realp, &isdir) != 0) {
        snprintf(why, sizeof why, "Its program %s is missing.", d.launch.executable);
        return set_state(a, PKG_RUN_MISSING, why);
    }
    if (isdir) {
        snprintf(why, sizeof why, "%s is a drawer, not a program.", d.launch.executable);
        return set_state(a, PKG_RUN_INVALID, why);
    }
    if (!inside(realdrawer, realp)) {
        snprintf(why, sizeof why, "%s leads outside the drawer (to %s).", d.launch.executable, realp);
        return set_state(a, PKG_RUN_INVALID, why);
    }
    if (real_name(a->workdir, realp, sizeof realp, &isdir) != 0 || isdir != 1) {
        snprintf(why, sizeof why, "Its working directory %s is not a drawer.", d.launch.workdir);
        return set_state(a, PKG_RUN_MISSING, why);
    }
    if (!inside(realdrawer, realp)) {
        snprintf(why, sizeof why, "Its working directory leads outside the drawer (to %s).", realp);
        return set_state(a, PKG_RUN_INVALID, why);
    }

    /* AROS keeps the "executable" bit inverted: set means not executable. */
    prot = protection_of(a->program);
    if (prot >= 0 && (prot & FIBF_EXECUTE)) {
        snprintf(why, sizeof why, "%s is not marked executable (protection bit e).", d.launch.executable);
        return set_state(a, PKG_RUN_INVALID, why);
    }
    got = 0;
    if ((fh = Open((CONST_STRPTR)a->program, MODE_OLDFILE)) != BNULL) {
        got = Read(fh, head, sizeof head);
        Close(fh);
    }
    bin = app_binary_arch(head, got > 0 ? (size_t)got : 0);
    if (!*bin) {
        /* A Shell script is a legitimate Shell program; AROS runs a file
           with the s bit through the Shell. Anything else is not a program
           this machine loads. */
        if (!(d.launch.mode == PKG_LAUNCH_SHELL && prot >= 0 && (prot & FIBF_SCRIPT))) {
            snprintf(why, sizeof why, "%s is not a program this system can load.", d.launch.executable);
            return set_state(a, PKG_RUN_INVALID, why);
        }
    } else if (strcmp(bin, pkg_arch()) != 0) {
        snprintf(why, sizeof why, "Its program file is built for %s; this machine is %s.", bin, pkg_arch());
        return set_state(a, PKG_RUN_INCOMPATIBLE, why);
    }

    {
        struct Library *IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 44);
        struct DiskObject *dob = NULL;
        int type = -1;
        if (IconBase) {
            char iconpath[PKG_MAXPATH];
            snprintf(iconpath, sizeof iconpath, "%s.info", a->program);
            /* GetDiskObject, not GetDiskObjectNew: a default icon would hide
               the missing one, and its stack is not the program's. */
            if (u_exists(iconpath) && (dob = GetDiskObject((STRPTR)a->program)) != NULL) {
                type = dob->do_Type;
                if (type == WBTOOL) a->stack = dob->do_StackSize;
                /* With a CLI tool type, workbench.library starts the program
                   as a Shell command with the default stack, not the
                   icon's (openworkbenchobjecta.c, HandleTool). */
                if (type == WBTOOL && dob->do_ToolTypes && FindToolType(dob->do_ToolTypes, (STRPTR)"CLI"))
                    type = -2;
                FreeDiskObject(dob);
            }
            CloseLibrary(IconBase);
        }
        if (d.launch.mode == PKG_LAUNCH_WORKBENCH) {
            if (type < 0) {
                snprintf(why, sizeof why, "A Workbench start needs the program's icon, %s.info, "
                         "and it is missing.", d.launch.executable);
                return set_state(a, PKG_RUN_MISSING, why);
            }
            if (type == -2) {
                snprintf(why, sizeof why, "%s.info has the tool type CLI, so Workbench would start it "
                         "as a Shell command with the default stack; use shell mode, or remove CLI.",
                         d.launch.executable);
                return set_state(a, PKG_RUN_INVALID, why);
            }
            if (type != WBTOOL) {
                snprintf(why, sizeof why, "%s.info is not a program icon (type %d), so Workbench "
                         "would not start it as one.", d.launch.executable, type);
                return set_state(a, PKG_RUN_INVALID, why);
            }
        }
    }

    if (nreq > 0) {
        int missing = 0, undet = 0;
        char list[200] = "";
        req_probe_all(reqs, nreq);
        for (i = 0; i < nreq; i++) {
            if (reqs[i].state == REQ_MISSING) {
                size_t k = strlen(list);
                snprintf(list + k, sizeof list - k, "%s%s", missing ? ", " : "", reqs[i].id);
                missing++;
            } else if (reqs[i].state == REQ_UNDETERMINED) undet++;
        }
        snprintf(a->requirements, sizeof a->requirements, "%s",
                 missing ? "missing" : undet ? "undetermined" : "satisfied");
        if (missing) {
            snprintf(why, sizeof why, "This machine does not have %s, which it needs.", list);
            return set_state(a, PKG_RUN_REQUIRES, why);
        }
    }
    snprintf(why, sizeof why, "%s",
             d.launch.mode == PKG_LAUNCH_WORKBENCH ? "Starts from its icon, as a double-click would."
                                                   : "Starts as a Shell command.");
    return set_state(a, PKG_RUN_READY, why);
}

/* Tasks with this name, ready or waiting. Processes started by Workbench
   are named after the program file. Under Disable, not Forbid: exec moves
   tasks between these lists from interrupts (rom/exec/signal.c). On an SMP
   exec a task running on another core is in a private list this cannot
   see, so the caller also asks FindTask, which looks there. */
static int tasks_named(const char *name)
{
    struct Node *n;
    int k = 0;
    Disable();
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (n->ln_Name && strcmp(n->ln_Name, name) == 0) k++;
    for (n = SysBase->TaskWait.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (n->ln_Name && strcmp(n->ln_Name, name) == 0) k++;
    Enable();
    return k;
}

/* One Shell argument, quoted the way ReadArgs reads it back: '*' is the
   escape character, so '"' becomes '*"' and '*' becomes '**'. Control
   characters are refused rather than escaped: a newline would end the
   command and start another. */
static int quote_arg(char *out, size_t n, const char *arg)
{
    size_t k = 0;
    const char *p;
    if (n < 3) return -1;
    out[k++] = '"';
    for (p = arg; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x20 || ch == 0x7f) return -1;
        if (ch == '"' || ch == '*') { if (k + 2 >= n) return -1; out[k++] = '*'; }
        if (k + 2 >= n) return -1;
        out[k++] = (char)ch;
    }
    out[k++] = '"';
    out[k] = 0;
    return 0;
}

pkg_status pkg_run(pkg_ctx *c, const char *what, const char *const *args, int nargs,
                   int flags, pkg_run_result *r, pkg_err *e)
{
    pkg_app_info a;
    pkg_status st;
    int i;

    memset(r, 0, sizeof *r);
    st = pkg_app_info_get(c, what, &a, e);
    if (st != PKG_OK) return st;
    r->mode = a.launch.mode;
    switch (a.state) {
    case PKG_RUN_READY: break;
    case PKG_RUN_NOT_INSTALLED:
        return pkg_fail(e, PKG_E_NOT_FOUND, "not installed", a.why, what);
    case PKG_RUN_REQUIRES:
        return pkg_fail(e, PKG_E_REQUIRES, "cannot start it on this machine", a.why, a.drawer);
    case PKG_RUN_INCOMPATIBLE:
        return pkg_fail(e, PKG_E_CONFLICT, "built for another system", a.why, a.drawer);
    case PKG_RUN_NO_DESCRIPTION:
        return pkg_fail(e, PKG_E_LAUNCH, "it does not say how to start it", a.why, a.drawer);
    default:
        return pkg_fail(e, PKG_E_LAUNCH, "it cannot be started as described", a.why, a.drawer);
    }

    if (a.launch.mode == PKG_LAUNCH_WORKBENCH) {
        struct Library *WorkbenchBase;
        struct TagItem *tags;
        BPTR *locks;
        const char *base = (const char *)FilePart((STRPTR)a.program);
        int before, after = 0, ticks = 0, nl = 0;
        BOOL ok;

        tags = (struct TagItem *)calloc((size_t)nargs * 2 + 1, sizeof *tags);
        locks = (BPTR *)calloc((size_t)nargs + 1, sizeof *locks);
        if (!tags || !locks) { free(tags); free(locks); return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", what); }
        /* A document is passed as Workbench passes a double-clicked project:
           the lock of its drawer and its name. */
        for (i = 0; i < nargs; i++) {
            BPTR l = Lock((CONST_STRPTR)args[i], ACCESS_READ);
            if (!l) {
                for (; nl > 0; nl--) UnLock(locks[nl - 1]);
                free(tags); free(locks);
                return pkg_fail(e, PKG_E_NOT_FOUND, "cannot find the file to open", "", args[i]);
            }
            locks[nl] = ParentDir(l);
            UnLock(l);
            if (!locks[nl]) {
                for (; nl > 0; nl--) UnLock(locks[nl - 1]);
                free(tags); free(locks);
                return pkg_fail(e, PKG_E_IO, "cannot pass a whole volume as a file to open", "", args[i]);
            }
            tags[2 * i].ti_Tag = WBOPENA_ArgLock;  tags[2 * i].ti_Data = (IPTR)locks[nl];
            tags[2 * i + 1].ti_Tag = WBOPENA_ArgName;
            tags[2 * i + 1].ti_Data = (IPTR)FilePart((STRPTR)args[i]);
            nl++;
        }
        tags[2 * nargs].ti_Tag = TAG_DONE;

        WorkbenchBase = OpenLibrary((CONST_STRPTR)"workbench.library", 44);
        if (!WorkbenchBase) {
            for (; nl > 0; nl--) UnLock(locks[nl - 1]);
            free(tags); free(locks);
            return pkg_fail(e, PKG_E_LAUNCH, "there is no desktop to start it with",
                            "workbench.library 44 is not available.", a.program);
        }
        before = tasks_named(base);
        ok = OpenWorkbenchObjectA((STRPTR)a.program, tags);
        CloseLibrary(WorkbenchBase);
        for (; nl > 0; nl--) UnLock(locks[nl - 1]);
        free(tags); free(locks);
        if (!ok)
            return pkg_fail(e, PKG_E_LAUNCH, "Workbench refused to start it", "", a.program);
        for (ticks = 0; ticks < RUN_CONFIRM_TICKS; ticks += 10) {
            if ((after = tasks_named(base)) > before) break;
            if (before == 0 && FindTask((CONST_STRPTR)base)) { after = 1; break; }
            Delay(10);
        }
        if (after <= before) {
            snprintf(r->how, sizeof r->how, "Workbench took the request, but no %s process was seen "
                     "within %d seconds.", base, RUN_CONFIRM_TICKS / 50);
            return pkg_fail(e, PKG_E_LAUNCH, "not seen running",
                            "It may have failed to load, or it ended at once. Try its icon in the drawer; "
                            "Open folder shows it.", a.program);
        }
        r->confirmed = 1;
        if (a.stack > 0)
            snprintf(r->how, sizeof r->how, "Started %s from its icon (stack %ld bytes from %s.info).",
                     a.name, a.stack, base);
        else
            snprintf(r->how, sizeof r->how, "Started %s from its icon.", a.name);
        return PKG_OK;
    }

    /* Shell mode. */
    {
        size_t cap = PKG_MAXPATH + 8;
        char *cmd, q[PKG_MAXPATH + 8];
        BPTR wd, old, con = BNULL;
        LONG rc;
        struct TagItem tags[8];
        int k = 0;

        for (i = 0; i < nargs; i++) cap += strlen(args[i]) * 2 + 4;
        if (!(cmd = (char *)malloc(cap))) return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", what);
        if (quote_arg(cmd, cap, a.program) != 0) {
            free(cmd);
            return pkg_fail(e, PKG_E_LAUNCH, "the program's path cannot be passed to the Shell", "", a.program);
        }
        for (i = 0; i < nargs; i++) {
            if (quote_arg(q, sizeof q, args[i]) != 0) {
                free(cmd);
                return pkg_fail(e, PKG_E_LAUNCH, "an argument cannot be passed to the Shell",
                                "It has a control character or is too long.", args[i]);
            }
            strcat(cmd, " ");
            strcat(cmd, q);
        }
        if (!(wd = Lock((CONST_STRPTR)a.workdir, ACCESS_READ))) {
            free(cmd);
            return pkg_fail(e, PKG_E_LAUNCH, "cannot enter its working directory", "", a.workdir);
        }
        if (a.stack > 0) { tags[k].ti_Tag = NP_StackSize; tags[k++].ti_Data = (IPTR)a.stack; }
        if (flags & PKG_RUN_WAIT) {
            tags[k].ti_Tag = SYS_Input;  tags[k++].ti_Data = (IPTR)Input();
            tags[k].ti_Tag = SYS_Output; tags[k++].ti_Data = (IPTR)Output();
        } else {
            char spec[160];
            snprintf(spec, sizeof spec, "CON:40/60/640/260/%.60s/AUTO/CLOSE/WAIT", a.name);
            if (!(con = Open((CONST_STRPTR)spec, MODE_OLDFILE))) {
                UnLock(wd); free(cmd);
                return pkg_fail(e, PKG_E_LAUNCH, "cannot open a console window for it", "", spec);
            }
            tags[k].ti_Tag = SYS_Input;  tags[k++].ti_Data = (IPTR)con;
            tags[k].ti_Tag = SYS_Output; tags[k++].ti_Data = (IPTR)BNULL;
            tags[k].ti_Tag = SYS_Asynch; tags[k++].ti_Data = TRUE;
        }
        tags[k].ti_Tag = TAG_DONE;
        /* The new process inherits the caller's current directory, so the
           caller moves there for the duration of the call. */
        /* The program shares our output handle. On mainline before
           da9bbd01b5 (the pool's v1 base among them) its stdcio.library
           writes back the handle's flags as they were when it opened, and
           a handle not in write mode then loses whatever is buffered at the
           next Flush: with apkg's output redirected to a file, the
           program's lines and apkg's own report after it came out empty
           (~/Work/AROS/CLAUDE.md). Flushing ours and writing nothing first
           puts the handle in write mode for both, before and after. */
        if (flags & PKG_RUN_WAIT) { fflush(stdout); FPuts(Output(), (CONST_STRPTR)""); Flush(Output()); }
        old = CurrentDir(wd);
        rc = SystemTagList((CONST_STRPTR)cmd, tags);
        CurrentDir(old);
        if (flags & PKG_RUN_WAIT) FPuts(Output(), (CONST_STRPTR)"");
        UnLock(wd);
        free(cmd);
        if (rc == -1) {
            if (con) Close(con);
            return pkg_fail(e, PKG_E_LAUNCH, "the Shell could not start it", "", a.program);
        }
        if (flags & PKG_RUN_WAIT) {
            r->waited = 1;
            r->rc = rc;
            snprintf(r->how, sizeof r->how, "%s ended with return code %ld.", a.name, (long)rc);
        } else {
            snprintf(r->how, sizeof r->how, "Started %s in a Shell window of its own.", a.name);
        }
        return PKG_OK;
    }
}
