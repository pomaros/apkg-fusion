/* PkgManager's ARexx commands: the parts that are plain C -- reading a
 * command line against the command table, naming errors, and formatting
 * replies from libpkg's rows and the window's job records. No Amiga API
 * here, so the host tests run all of it. The port itself is rexx.c; what a
 * command does is main.c's. The contract is docs/arexx.md. */
#ifndef PKGMANAGER_REXXCMD_H
#define PKGMANAGER_REXXCMD_H

#include "../libpkg/pkg.h"
#include <stddef.h>

#define RX_INTERFACE 1          /* bumped when a reply changes meaning */
#define RX_PORTNAME  "PKGMANAGER"
#define RX_MAXRESULT 65535      /* an argstring's length is 16 bits */

/* ARexx severities: RC as the script sees it. */
enum { RX_RC_OK = 0, RX_RC_WARN = 5, RX_RC_ERROR = 10, RX_RC_FAIL = 20 };

typedef enum {
    RXC_NONE = 0, RXC_VERSION, RXC_HELP, RXC_LIST, RXC_INFO, RXC_REQUIREMENTS,
    RXC_SHOW, RXC_UPDATE, RXC_INSTALL, RXC_REMOVE, RXC_UPGRADE, RXC_ROLLBACK,
    RXC_JOB, RXC_WAIT, RXC_CANCEL, RXC_QUIT, RXC_LASTERROR, RXC_OUTDATED,
    RXC_RUN, RXC_VERIFY
} rx_cmd;

typedef struct {
    rx_cmd cmd;
    char   package[PKG_MAXID];
    char   field[24];           /* INFO, JOB, WAIT: one field instead of all */
    char   match[128];          /* LIST MATCH */
    int    installed;           /* LIST INSTALLED */
    int    show;                /* OUTDATED SHOW */
    long   job;                 /* JOB, WAIT, CANCEL; 0 = the latest job */
} rx_req;

/* Parse one command line. Returns 0, or RX_RC_FAIL with `err` set to
 * "BADCOMMAND ..." or "BADARGS ..." -- the text LASTERROR then returns. */
int rx_parse(const char *line, rx_req *out, char *err, size_t errn);

/* "INSTALL", for the command table in HELP and in messages. */
const char *rx_cmdname(rx_cmd);
/* Every command name, space separated: HELP's result. */
const char *rx_help(void);

/* libpkg's status as the word LASTERROR and JOB's `error` carry: LOCKED, NOTFOUND,
 * VERIFY ... Stable; "OK" for PKG_OK. */
const char *rx_statusword(pkg_status);

/* One job as the window records it, whoever started it. */
typedef enum {
    RXJ_PREVIEW = 0,   /* working out an upgrade's or rollback's plan   */
    RXJ_CONFIRM,       /* plan shown; the window waits for the user     */
    RXJ_RUNNING,
    RXJ_DONE, RXJ_FAILED, RXJ_CANCELLED, RXJ_DECLINED   /* final */
} rx_jobstate;

typedef struct {
    long          id;
    char          op[12];       /* update install remove upgrade rollback */
    char          package[PKG_MAXID];
    char          origin[8];    /* window arexx */
    rx_jobstate   state;
    char          phase[16];
    unsigned long done, total;
    int           can_cancel;
    int           cancel_asked;
    char          error[16];    /* a status word, NOTALLOWED or CONFLICT; "" */
    char          message[480]; /* libpkg's words, or the plan when confirming */
} rx_job;

const char *rx_statename(rx_jobstate);
int rx_final(rx_jobstate);

/* Replies. `field` NULL or "" formats every field, one "name value" line
 * each; otherwise only that field's value. Return 0, or -1 when `field` is
 * not one of the command's fields (the caller answers BADARGS). */
int rx_format_entry(const pkg_entry *, int can_upgrade, int can_rollback,
                    const char *compatibility, const char *requirements,
                    const char *field, char *out, size_t n);
int rx_format_job(const rx_job *, const char *field, char *out, size_t n);

/* OUTDATED's answer from the shared updates model: one "name value" line
 * each for catalogue, updated, stale, installed, newer, ready, blocked, and
 * "packages" with the ids that have something newer. -1 if it does not fit. */
int rx_format_updates(const pkg_updates *, char *out, size_t n);

/* The fields in order, space separated, for the BADARGS message. */
const char *rx_entry_fields(void);
const char *rx_job_fields(void);

#endif
