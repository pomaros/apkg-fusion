/* requires_system -- what the MACHINE must already provide.
 *
 * Separate from `depends`, and the distinction is the whole point. "No package
 * in the index provides X" is our problem and we can fix it by publishing a
 * package. "This machine does not have X" is not ours and no amount of
 * downloading changes it. The two conditions therefore produce different
 * messages and live in different fields; conflating them is how a manager ends
 * up telling a user to wait for a package that will never exist.
 *
 * Three outcomes, not two:
 *
 *   satisfied      the requirement is met here, now. Proceeds silently.
 *   missing        it is not met. Refuses BEFORE the download -- installing
 *                  would succeed and produce something that cannot start.
 *   undetermined   we could not decide. Proceeds, says so prominently, and is
 *                  written into the registry entry so the record of the
 *                  install carries the doubt rather than dropping it.
 *
 * Undetermined is not a rounding error on the way to satisfied. Most things a
 * package might require on this platform are not decidable from outside the
 * program -- a datatype, a screenmode, a TCP stack that is installed but not
 * running -- and a manager that refuses whenever it cannot be sure is one
 * nobody runs. So the honest answer is recorded, not resolved by assumption.
 */
#include "internal.h"
#include "json.h"
#include "req.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#ifdef __AROS__
#  include <proto/exec.h>
#  include <exec/execbase.h>
#  include <exec/libraries.h>
#endif

/* ------------------------------------------------------------------- parse */

int req_parse(const char *js, js_tok *t, int ntok, int el,
              req_ent *out, int max, int *n, char *why, size_t whyn)
{
    int arr = js_member(js, t, ntok, el, "requires_system");
    int i;

    *n = 0;
    if (arr < 0) return 0;            /* absent is empty, not an error */

    for (i = 0; ; i++) {
        int o = js_elem(t, ntok, arr, i);
        req_ent *r;
        if (o < 0) break;
        if (*n >= max) {
            snprintf(why, whyn, "more than %d system requirements", max);
            return -1;
        }
        r = &out[*n];
        memset(r, 0, sizeof *r);
        r->state = REQ_UNDETERMINED;
        r->found_version = -1;
        js_str(js, t, js_member(js, t, ntok, o, "type"), r->type, sizeof r->type);
        js_str(js, t, js_member(js, t, ntok, o, "id"),   r->id,   sizeof r->id);
        r->min_version = js_long(js, t, js_member(js, t, ntok, o, "min_version"), 0);
        /* An entry we cannot even read is refused rather than skipped: a
         * requirement silently dropped is indistinguishable from a package
         * with no requirements, which is the failure this field exists to
         * prevent. */
        if (!r->id[0] || !r->type[0]) {
            snprintf(why, whyn,
                     "requires_system entry %d has no %s", i,
                     r->id[0] ? "type" : "id");
            return -1;
        }
        (*n)++;
    }
    return 0;
}

/* ------------------------------------------------------------------- probe */

/* The real probe, on AROS. Cheapest and least invasive test first.
 *
 * 1. The resident library list. If it is already open, its version is right
 *    there in the node and nothing has to be loaded to read it.
 *
 * 2. OpenLibrary(). This does run the library's init code, which is a real
 *    side effect and worth being deliberate about: it is exactly what the
 *    program we are about to install will do a moment later, and it is the
 *    only test that answers the question actually asked -- not "is there a
 *    file with this name" but "can this be opened at this version here".
 *
 * 3. A file in LIBS: when neither worked. That distinguishes "nothing by this
 *    name on the machine" from "it is there and will not open", which are
 *    different things to tell a user even though both refuse.
 *
 * What this cannot distinguish: OpenLibrary() failing for want of memory looks
 * the same as failing for absence. Rare, and it would be dishonest to report
 * absence with confidence -- so a machine short of memory can get a wrong
 * "not installed" here. Noted rather than handled.
 */
#ifdef __AROS__
static void probe_library(req_ent *r)
{
    struct Library *l;
    /* r->id is 64 bytes; sized so the message below cannot be truncated. */
    char path[80];

    Forbid();
    l = (struct Library *)FindName(&SysBase->LibList, (STRPTR)r->id);
    if (l) r->found_version = l->lib_Version;
    Permit();
    if (l && r->found_version >= r->min_version) {
        r->state = REQ_SATISFIED;
        snprintf(r->why, sizeof r->why, "already open, version %ld",
                 r->found_version);
        return;
    }

    l = OpenLibrary((STRPTR)r->id, (ULONG)r->min_version);
    if (l) {
        r->found_version = l->lib_Version;
        CloseLibrary(l);
        r->state = REQ_SATISFIED;
        snprintf(r->why, sizeof r->why, "opens, version %ld", r->found_version);
        return;
    }

    snprintf(path, sizeof path, "LIBS:%s", r->id);
    r->state = REQ_MISSING;
    if (u_exists(path))
        snprintf(r->why, sizeof r->why,
                 "%s exists but would not open%s", path,
                 r->min_version ? " at the required version" : "");
    else if (r->min_version)
        snprintf(r->why, sizeof r->why,
                 "not available at version %ld or later", r->min_version);
    else
        snprintf(r->why, sizeof r->why, "not on this machine");
}
#endif

static void probe_default(req_ent *r)
{
    if (strcmp(r->type, "library") == 0) {
#ifdef __AROS__
        probe_library(r);
#else
        /* The host build exists to run the unit tests, which supply their own
         * probe. It must not claim a verdict it has no way to reach. */
        r->state = REQ_UNDETERMINED;
        snprintf(r->why, sizeof r->why, "this build cannot probe a library");
#endif
        return;
    }
    /* Everything else. Section 3 lists devices, datatypes and screenmodes as
     * future requirement types; until one is implemented, saying so is the
     * correct answer and pretending otherwise is not. */
    r->state = REQ_UNDETERMINED;
    snprintf(r->why, sizeof r->why,
             "requirement type \"%s\" is not one this client can check",
             r->type);
}

static req_probe_fn probe = probe_default;

void req_set_probe(req_probe_fn f) { probe = f ? f : probe_default; }

const char *req_statename(req_state s)
{
    switch (s) {
    case REQ_SATISFIED:    return "satisfied";
    case REQ_MISSING:      return "missing";
    case REQ_UNDETERMINED: return "undetermined";
    }
    return "?";
}

/* ------------------------------------------------------------------- check */

void req_probe_all(req_ent *r, int n)
{
    int i;
    for (i = 0; i < n; i++) probe(&r[i]);
}

pkg_status req_check(const char *pkg_id, req_ent *r, int n, pkg_err *e)
{
    int i, missing = 0, undet = 0;
    char det[320];
    size_t k = 0;

    req_probe_all(r, n);

    for (i = 0; i < n; i++) {
        if (r[i].state == REQ_MISSING)      missing++;
        else if (r[i].state == REQ_UNDETERMINED) undet++;
    }

    if (missing) {
        det[0] = 0;
        for (i = 0; i < n && k + 80 < sizeof det; i++) {
            if (r[i].state != REQ_MISSING) continue;
            k += (size_t)snprintf(det + k, sizeof det - k, "%s%s (%s)",
                                  k ? "; " : "", r[i].id, r[i].why);
        }
        /* Said in the user's terms: this is a property of the machine, so
         * there is nothing to install and nothing to retry. The wording
         * deliberately does not suggest waiting for a package. */
        return pkg_fail(e, PKG_E_REQUIRES,
                        "this system does not provide what the package needs",
                        det, pkg_id);
    }

    if (undet) {
        printf("WARNING: %d of this package's %d system requirement(s) could not be\n"
               "         checked. It will be installed, and this is recorded with it.\n",
               undet, n);
        for (i = 0; i < n; i++)
            if (r[i].state == REQ_UNDETERMINED)
                printf("         %s %s: %s\n", r[i].type, r[i].id, r[i].why);
    }
    return PKG_OK;
}

void req_emit(js_out *o, req_ent *r, int n)
{
    int i;
    js_key(o, "requires_system"); js_raw(o, " [");
    for (i = 0; i < n; i++) {
        js_raw(o, i ? ",\n    { " : "\n    { ");
        js_key(o, "type");  js_vstr(o, r[i].type); js_raw(o, ", ");
        js_key(o, "id");    js_vstr(o, r[i].id);   js_raw(o, ", ");
        js_key(o, "min_version"); js_vlong(o, r[i].min_version); js_raw(o, ", ");
        js_key(o, "state"); js_vstr(o, req_statename(r[i].state)); js_raw(o, ", ");
        js_key(o, "found_version"); js_vlong(o, r[i].found_version); js_raw(o, ", ");
        js_key(o, "checked"); js_vstr(o, r[i].why);
        js_raw(o, " }");
    }
    js_raw(o, n ? "\n  ]" : " ]");
}
