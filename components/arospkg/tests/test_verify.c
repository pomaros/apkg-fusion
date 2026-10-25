/* verify's classification and the revision label, on the host. */
#include "../src/libpkg/verify.h"
#include <stdio.h>
#include <string.h>

static int bad = 0;
static void ok(int c, const char *l) { printf("%-4s %s\n", c ? "ok" : "FAIL", l); if (!c) bad++; }

static inv_ent E(const char *rel, const char *sha)
{
    inv_ent e; memset(&e, 0, sizeof e);
    snprintf(e.rel, sizeof e.rel, "%s", rel); snprintf(e.sha, sizeof e.sha, "%s", sha); e.size = 1;
    return e;
}
static vf_state st(vf_item *v, int n, const char *rel)
{ int i; for (i = 0; i < n; i++) if (!strcmp(v[i].rel, rel)) return v[i].state; return (vf_state)-1; }

int main(void)
{
    inv_ent reg[]  = { E("Prog","a"), E("Same","s"), E("Gone","g"), E("Cfg","c") };
    inv_ent live[] = { E("Prog","a"), E("Same","s2"), E("Cfg","c"), E("Extra","x") };
    vf_item v[16]; int n, o, c, m, u; char lbl[80];

    n = verify_classify(reg, 4, live, 4, v, 16, &o, &c, &m, &u);
    ok(n == 5 && o == 2 && c == 1 && m == 1 && u == 1, "counts: 2 ok, 1 changed, 1 missing, 1 unknown");
    ok(st(v, n, "Prog") == VF_OK,      "same content: ok");
    ok(st(v, n, "Same") == VF_CHANGED, "different content: changed locally");
    ok(st(v, n, "Gone") == VF_MISSING, "in the registry, not on disk: missing");
    ok(st(v, n, "Extra") == VF_UNKNOWN,"on disk, not in the registry: not the package's");
    ok(!strcmp(v[0].rel, "Cfg") && !strcmp(v[n-1].rel, "Same"), "sorted by path");
    ok(verify_classify(reg, 4, live, 4, v, 3, &o, &c, &m, &u) == -1, "too small an output is refused");
    n = verify_classify(reg, 4, reg, 4, v, 16, &o, &c, &m, &u);
    ok(o == 4 && c + m + u == 0, "a pristine drawer is all ok");
    n = verify_classify(reg, 4, NULL, 0, v, 16, &o, &c, &m, &u);
    ok(m == 4 && o + c + u == 0, "an empty drawer is all missing");

    rev_label(lbl, sizeof lbl, "2.2", 0); ok(!strcmp(lbl, "2.2"),       "revision not stated: the version alone, never aros0");
    rev_label(lbl, sizeof lbl, "2.2", 2); ok(!strcmp(lbl, "2.2-aros2"), "revision 2: 2.2-aros2");

    printf(bad ? "\nFAIL %d\n" : "\nPASS all checks\n", bad);
    return bad != 0;
}
