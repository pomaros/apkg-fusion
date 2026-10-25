#ifndef PKG_REQ_H
#define PKG_REQ_H
#include "pkg.h"
#include "json.h"
#include <stddef.h>

/* System requirements: see req.c for why there are three outcomes. */

#define MAXREQ 16

typedef enum { REQ_SATISFIED = 0, REQ_MISSING, REQ_UNDETERMINED } req_state;

typedef struct {
    char type[24];        /* "library"; other types are undetermined for now */
    char id[64];          /* "SDL.library"                                   */
    long min_version;     /* 0 = any                                         */
    req_state state;
    long found_version;   /* -1 = never read                                 */
    char why[200];        /* how the verdict was reached, in user terms      */
} req_ent;

/* Read requires_system out of index element `el`. Returns 0, or -1 with a
 * reason in `why` -- an unreadable entry is refused, never skipped. */
int  req_parse(const char *js, js_tok *, int ntok, int el,
               req_ent *out, int max, int *n, char *why, size_t whyn);

/* Probe every entry on this machine and record the state and why; no
 * verdict. A front end shows the outcomes; the install decides on them. */
void req_probe_all(req_ent *, int n);

/* Probe every entry and decide. PKG_E_REQUIRES if anything is missing;
 * PKG_OK with a warning printed if anything is undetermined. */
pkg_status req_check(const char *pkg_id, req_ent *, int n, pkg_err *);

/* Write the outcomes into a registry entry, so the record of the install
 * carries any doubt rather than dropping it. */
void req_emit(js_out *, req_ent *, int n);

const char *req_statename(req_state);

/* Tests only: replace the probe. The three outcomes are otherwise reachable
 * only on particular machines, and a test that can only run on one of them is
 * a test that does not run. */
typedef void (*req_probe_fn)(req_ent *);
void req_set_probe(req_probe_fn);

#endif
