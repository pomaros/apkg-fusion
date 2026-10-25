/* PkgManager's ARexx port. Its own port, not Zune's: Zune's dispatcher
 * answers an unknown command with RC 0, matches command names by prefix,
 * calls the command after ReadArgs has refused its arguments, and keeps one
 * command's RESULT for the next -- all four read in
 * workbench/libs/muimaster/classes/application.c. The port is served from
 * the window's own Wait(), so a command runs between two Zune events and
 * never on another task.
 *
 * Nothing here sets a variable in the calling script. SetRexxVar was the
 * first way RC2 was filled, and on AROS One 1.3 it brings RexxMast down
 * ("tlsf_freevec", privilege violation) after some thirty calls made from
 * inside a REXX PROCEDURE -- the way anyone wraps a command. Measured in
 * docs/spikes/arexx/. The text of a failure is kept here instead, for the
 * script that caused it, and LASTERROR hands it over. */
#include "rexx.h"
#include "rexxcmd.h"
#include <proto/exec.h>
#include <proto/rexxsyslib.h>
#include <rexx/storage.h>
#include <rexx/errors.h>
#include <stdio.h>
#include <string.h>

struct RxsLib *RexxSysBase;
static struct MsgPort *port;
static char state[120] = "ARexx port not opened";

/* The last failure of each client. A client is the port its messages come
 * back to together with the task that owns that port: Regina gives every
 * running script one of each, so two scripts running at once never read
 * each other's error. A success from that client clears its entry; the
 * least recently used entry makes room for a new client. What this cannot
 * rule out, and docs/arexx.md says so: a script whose port and task land at
 * the addresses a finished script had, asking LASTERROR before it has
 * failed at all, reads that script's leftover. */
#define MAXCLIENTS 32
static struct { struct MsgPort *port; struct Task *task; unsigned long used; char text[600]; } lasterr[MAXCLIENTS];
static unsigned long lasterr_clock;
static int test_nomem_result;

void rexx_test_nomem(void) { test_nomem_result = 1; }

static int client_slot(rexx_msg *m, int create)
{
    struct MsgPort *p = m->rm_Node.mn_ReplyPort;
    struct Task *t = p ? p->mp_SigTask : NULL;
    int i, old = 0;
    for (i = 0; i < MAXCLIENTS; i++)
        if (lasterr[i].port == p && lasterr[i].task == t && lasterr[i].used) {
            lasterr[i].used = ++lasterr_clock;
            return i;
        }
    if (!create) return -1;
    for (i = 1; i < MAXCLIENTS; i++) if (lasterr[i].used < lasterr[old].used) old = i;
    lasterr[old].port = p; lasterr[old].task = t; lasterr[old].used = ++lasterr_clock;
    lasterr[old].text[0] = 0;
    return old;
}

/* An answer allocated before the command acts; see rexx.h. One at a time:
 * commands run one after another on this task. */
static rexx_msg *prep_msg;
static UBYTE *prep_arg;
static int test_nomem_prepare;
void rexx_test_nomem_prepare(void) { test_nomem_prepare = 1; }

int rexx_prepare(rexx_msg *m, const char *result)
{
    size_t n = strlen(result);
    prep_msg = m; prep_arg = NULL;
    if (!(m->rm_Action & RXFF_RESULT) || !n) return 1;      /* nothing to allocate */
    if (!test_nomem_prepare) prep_arg = CreateArgstring((CONST_STRPTR)result, n > RX_MAXRESULT ? RX_MAXRESULT : n);
    if (!prep_arg) { prep_msg = NULL; return 0; }
    return 1;
}

const char *rexx_lasterror(rexx_msg *m)
{
    int i = client_slot(m, 0);
    return i < 0 ? "" : lasterr[i].text;
}

void rexx_open(const char *libname)
{
    RexxSysBase = (struct RxsLib *)OpenLibrary((CONST_STRPTR)(libname ? libname : "rexxsyslib.library"), 0);
    if (!RexxSysBase) {
        snprintf(state, sizeof state, "no ARexx port: %s cannot be opened",
                 libname ? libname : "rexxsyslib.library");
        return;
    }
    port = CreateMsgPort();
    if (!port) {
        snprintf(state, sizeof state, "no ARexx port: out of memory");
        CloseLibrary((struct Library *)RexxSysBase); RexxSysBase = NULL;
        return;
    }
    port->mp_Node.ln_Name = (char *)RX_PORTNAME;
    port->mp_Node.ln_Pri = 0;
    /* One port of that name at a time. A second PkgManager keeps the window
       and goes without a port, so a script always reaches the first one --
       the only one it can find by name. */
    Forbid();
    if (FindPort((CONST_STRPTR)RX_PORTNAME)) {
        Permit();
        port->mp_Node.ln_Name = NULL;
        DeleteMsgPort(port); port = NULL;
        CloseLibrary((struct Library *)RexxSysBase); RexxSysBase = NULL;
        snprintf(state, sizeof state, "no ARexx port: %s is already in use", RX_PORTNAME);
        return;
    }
    AddPort(port);
    Permit();
    snprintf(state, sizeof state, "ARexx port %s", RX_PORTNAME);
}

int rexx_active(void) { return port != NULL; }
const char *rexx_state(void) { return state; }
ULONG rexx_sigmask(void) { return port ? 1UL << port->mp_SigBit : 0; }

int rexx_reply(rexx_msg *m, int rc, const char *result, const char *error)
{
    int i;
    m->rm_Result2 = 0;
    if (m == prep_msg) {
        /* Prepared: it goes out as it is on success, and is thrown away if
           the command failed after all. */
        if (rc == 0) m->rm_Result2 = (IPTR)prep_arg;
        else if (prep_arg) DeleteArgstring(prep_arg);
        prep_msg = NULL; prep_arg = NULL;
        result = NULL;
    }
    if (rc == 0 && (m->rm_Action & RXFF_RESULT) && result) {
        size_t n = strlen(result);
        if (n > RX_MAXRESULT) n = RX_MAXRESULT;
        /* --test-nomem result: long answers fail as they would with no
           memory, short ones -- LASTERROR's among them -- still go out. */
        if (!(test_nomem_result && n >= 64))
            m->rm_Result2 = (IPTR)CreateArgstring((CONST_STRPTR)result, n);
        /* RC 0 without the RESULT the script asked for would read as an
           empty answer; say it failed instead. (No argstring at all is how
           an empty answer is sent, so that case is not a failure.) */
        if (!m->rm_Result2 && n) { rc = RX_RC_ERROR; error = "NOMEM no memory for the answer"; }
    }
    m->rm_Result1 = rc;
    if (rc == 0) {
        if (error != REXX_KEEP && (i = client_slot(m, 0)) >= 0) lasterr[i].text[0] = 0;
    } else {
        i = client_slot(m, 1);
        snprintf(lasterr[i].text, sizeof lasterr[i].text, "%s", error ? error : "");
    }
    ReplyMsg(&m->rm_Node);
    return rc;
}

/* A few commands per turn of the window's loop, not the whole queue: the
 * rest waits behind the window's own input and the worker's messages. The
 * port's signal is raised again for them, so they are not forgotten. */
#define PER_TURN 4

void rexx_poll(void (*cmd)(rexx_msg *, const char *line))
{
    struct RexxMsg *m;
    int n = 0;
    if (!port) return;
    while (n++ < PER_TURN && (m = (struct RexxMsg *)GetMsg(port))) {
        if (!IsRexxMsg(m)) { ReplyMsg(&m->rm_Node); continue; }
        if ((m->rm_Action & RXCODEMASK) != RXCOMM || !m->rm_Args[0]) {
            rexx_reply(m, RX_RC_FAIL, NULL, "BADCOMMAND not a command message");
            continue;
        }
        cmd(m, (const char *)m->rm_Args[0]);
    }
    if (!IsListEmpty(&port->mp_MsgList)) SetSignal(1UL << port->mp_SigBit, 1UL << port->mp_SigBit);
}

void rexx_close(const char *error)
{
    struct RexxMsg *m;
    if (!port) return;
    Forbid();
    RemPort(port);
    Permit();
    while ((m = (struct RexxMsg *)GetMsg(port))) {
        if (IsRexxMsg(m)) rexx_reply(m, RX_RC_ERROR, NULL, error);
        else ReplyMsg(&m->rm_Node);
    }
    port->mp_Node.ln_Name = NULL;
    DeleteMsgPort(port); port = NULL;
    CloseLibrary((struct Library *)RexxSysBase); RexxSysBase = NULL;
    snprintf(state, sizeof state, "ARexx port closed");
}
