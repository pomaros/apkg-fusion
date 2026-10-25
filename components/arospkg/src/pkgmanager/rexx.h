/* The ARexx port as a message source for the window's loop. main.c decides
 * what a command does; this file only receives, replies and cleans up. */
#ifndef PKGMANAGER_REXX_H
#define PKGMANAGER_REXX_H

#include <exec/types.h>

typedef struct RexxMsg rexx_msg;

/* Open rexxsyslib.library (or `libname`, for the test that has none) and add
 * the public port, unless one of that name already exists. Never fails the
 * program: without ARexx the window is the whole of PkgManager, as before.
 * rexx_state() says which case this is, in the user's words. */
void        rexx_open(const char *libname);
int         rexx_active(void);
const char *rexx_state(void);
ULONG       rexx_sigmask(void);

/* Hand every waiting command to `cmd`, which must reply to it -- now or
 * later, but exactly once. Messages that are not ARexx commands are
 * answered here. */
void rexx_poll(void (*cmd)(rexx_msg *, const char *line));

/* rc 0: `result` becomes RESULT (when the script asked for one), and the
 * client's last error is cleared -- unless `error` is REXX_KEEP, which is
 * how LASTERROR itself answers. Otherwise RC is `rc` and `error` ("CODE
 * text") is kept for LASTERROR from the same client. A result that cannot
 * be allocated is sent as RC 10, NOMEM, and that is what is returned. */
#define REXX_KEEP ((const char *)1)
int  rexx_reply(rexx_msg *, int rc, const char *result, const char *error);  /* the RC sent */
const char *rexx_lasterror(rexx_msg *);
/* Testing only: answers of 64 bytes or more cannot be allocated. */
void rexx_test_nomem(void);

/* For a command that changes something: allocate its answer before it acts,
 * so that running out of memory for the answer is a refusal with nothing
 * done, not a failure reported after the fact. 0 = no memory; the caller
 * answers NOMEM and does nothing. Otherwise the next rexx_reply for `m`
 * sends this answer on success (its `result` is then ignored) and frees it
 * on failure. */
int  rexx_prepare(rexx_msg *, const char *result);
void rexx_test_nomem_prepare(void);   /* testing only: every rexx_prepare fails */

/* Take the port off the public list first, then answer what is still queued
 * with `error`, so no command is left without a reply. */
void rexx_close(const char *error);

#endif
