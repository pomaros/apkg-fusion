/* requires_system: the three outcomes, and the parse failures that must not
 * be mistaken for "no requirements".
 *
 * Runs on the host. The point of req_set_probe() is precisely this: the real
 * verdicts depend on which machine you are standing on, and a test that can
 * only run on one particular guest is a test that does not get run. The probe
 * here is a lookup table, so every branch of req_check() is reachable in one
 * second on any machine, and the AROS runs then check the one thing this
 * cannot -- that the real probe answers correctly about a real library.
 */
#include "../src/libpkg/req.h"
#include "../src/libpkg/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad = 0;

static void ok(int cond, const char *label)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", label);
    if (!cond) bad++;
}

/* ------------------------------------------------------- the staged probe */

/* Stands in for a machine. "present.library" is there at version 3,
 * "old.library" is there but too old, "absent.library" is not. Anything of
 * another type falls through to the real probe's own answer, which is
 * undetermined -- so that path is the library's, not the stub's. */
static void stub(req_ent *r)
{
    if (strcmp(r->type, "library") != 0) {
        r->state = REQ_UNDETERMINED;
        snprintf(r->why, sizeof r->why, "type %s not checkable", r->type);
        return;
    }
    if (strcmp(r->id, "present.library") == 0) {
        r->found_version = 3;
        if (r->min_version <= 3) {
            r->state = REQ_SATISFIED;
            snprintf(r->why, sizeof r->why, "opens, version 3");
        } else {
            r->state = REQ_MISSING;
            snprintf(r->why, sizeof r->why,
                     "version 3, need %ld", r->min_version);
        }
        return;
    }
    r->state = REQ_MISSING;
    snprintf(r->why, sizeof r->why, "not on this machine");
}

/* ------------------------------------------------------------- the driver */

/* Parse one index-shaped object and run the check over it, the same way
 * pkg_install does. */
static pkg_status run(const char *json, req_ent *out, int *n, pkg_err *e,
                      char *why, size_t whyn)
{
    js_tok *t = malloc(sizeof(js_tok) * 512);
    int ntok = js_parse(json, strlen(json), t, 512);
    pkg_status st;

    memset(e, 0, sizeof *e);
    why[0] = 0;
    if (ntok <= 0) { free(t); printf("FAIL test json does not parse\n"); bad++; return PKG_E_IO; }
    if (req_parse(json, t, ntok, 0, out, MAXREQ, n, why, whyn) != 0) {
        free(t);
        return PKG_E_REQUIRES;   /* pkg_install turns this into the same status */
    }
    st = req_check("testpkg", out, *n, e);
    free(t);
    return st;
}

int main(void)
{
    req_ent r[MAXREQ];
    pkg_err e;
    char why[200];
    int n;
    pkg_status st;

    req_set_probe(stub);

    /* 1. No field at all. Absent is empty; it must not be an error, because
     *    every package in the index today has no requirements. */
    st = run("{ \"id\": \"testpkg\" }", r, &n, &e, why, sizeof why);
    ok(st == PKG_OK && n == 0, "absent requires_system installs");

    /* 2. An empty array is the same thing said explicitly. */
    st = run("{ \"requires_system\": [] }", r, &n, &e, why, sizeof why);
    ok(st == PKG_OK && n == 0, "empty requires_system installs");

    /* 3. SATISFIED. */
    st = run("{ \"requires_system\": [ "
             "{ \"type\": \"library\", \"id\": \"present.library\" } ] }",
             r, &n, &e, why, sizeof why);
    ok(st == PKG_OK && n == 1 && r[0].state == REQ_SATISFIED,
       "a library that is present is satisfied");
    ok(r[0].found_version == 3, "the version found is recorded, not just the verdict");

    /* 4. MISSING, and the refusal must name the thing. A refusal that does not
     *    say what is wrong sends the user to guess. */
    st = run("{ \"requires_system\": [ "
             "{ \"type\": \"library\", \"id\": \"absent.library\" } ] }",
             r, &n, &e, why, sizeof why);
    ok(st == PKG_E_REQUIRES && r[0].state == REQ_MISSING,
       "a library that is absent refuses");
    ok(strstr(e.detail, "absent.library") != NULL,
       "the refusal names the missing library");
    /* The wording matters as much as the status: this is the machine's lack,
     * and the message must not read as "the index is incomplete". */
    ok(strstr(e.summary, "system") != NULL,
       "the refusal says the requirement is the system's");

    /* 5. MISSING by version. Present is not the same as present-and-new-enough,
     *    and a check that only looks at the name would pass this. */
    st = run("{ \"requires_system\": [ "
             "{ \"type\": \"library\", \"id\": \"present.library\", "
             "  \"min_version\": 9 } ] }",
             r, &n, &e, why, sizeof why);
    ok(st == PKG_E_REQUIRES && r[0].min_version == 9,
       "a library too old to meet min_version refuses");
    ok(r[0].found_version == 3,
       "the version actually found is still recorded on a refusal");

    /* 6. UNDETERMINED proceeds. This is the outcome that must NOT be rounded
     *    to either neighbour: rounding to satisfied hides it, rounding to
     *    missing refuses every package with a requirement we cannot check. */
    st = run("{ \"requires_system\": [ "
             "{ \"type\": \"datatype\", \"id\": \"png\" } ] }",
             r, &n, &e, why, sizeof why);
    ok(st == PKG_OK && r[0].state == REQ_UNDETERMINED,
       "an unknown requirement type proceeds as undetermined");
    ok(r[0].state != REQ_SATISFIED,
       "undetermined is not recorded as satisfied");

    /* 7. Missing beats undetermined. A package with one of each must refuse:
     *    the doubt does not excuse the certainty. */
    st = run("{ \"requires_system\": [ "
             "{ \"type\": \"datatype\", \"id\": \"png\" }, "
             "{ \"type\": \"library\", \"id\": \"absent.library\" } ] }",
             r, &n, &e, why, sizeof why);
    ok(st == PKG_E_REQUIRES, "one missing requirement refuses despite an undetermined one");

    /* 8. Several, all satisfied -- the shape sdllopan has. */
    st = run("{ \"requires_system\": [ "
             "{ \"type\": \"library\", \"id\": \"present.library\" }, "
             "{ \"type\": \"library\", \"id\": \"present.library\", \"min_version\": 2 } ] }",
             r, &n, &e, why, sizeof why);
    ok(st == PKG_OK && n == 2, "two satisfied requirements install");

    /* 9. An entry with no id is REFUSED, not skipped. Skipping it would make a
     *    malformed requirement indistinguishable from no requirement, which is
     *    the exact confusion this field exists to end. */
    st = run("{ \"requires_system\": [ { \"type\": \"library\" } ] }",
             r, &n, &e, why, sizeof why);
    ok(st == PKG_E_REQUIRES && strstr(why, "id") != NULL,
       "a requirement with no id refuses and says so");

    st = run("{ \"requires_system\": [ { \"id\": \"present.library\" } ] }",
             r, &n, &e, why, sizeof why);
    ok(st == PKG_E_REQUIRES && strstr(why, "type") != NULL,
       "a requirement with no type refuses and says so");

    /* 10. The registry text. An undetermined verdict that is not written down
     *     is an install nobody can account for afterwards. */
    st = run("{ \"requires_system\": [ "
             "{ \"type\": \"datatype\", \"id\": \"png\" } ] }",
             r, &n, &e, why, sizeof why);
    {
        char buf[2048];
        js_out o;
        js_out_init(&o, buf, sizeof buf);
        js_raw(&o, "{ ");
        req_emit(&o, r, n);
        js_raw(&o, " }\n");
        ok(!o.fail, "the registry fragment fits");
        ok(strstr(buf, "undetermined") != NULL,
           "the registry records the undetermined verdict");
        ok(strstr(buf, "\"png\"") != NULL, "the registry records what it was about");
        {   /* It has to be readable by the same parser that reads it back. */
            js_tok t2[128];
            int nt = js_parse(buf, strlen(buf), t2, 128);
            ok(nt > 0, "the registry fragment is valid JSON");
        }
    }

    printf(bad ? "\nFAIL %d check(s)\n" : "\nPASS all checks\n", bad);
    return bad ? 1 : 0;
}
