/* The upgrade planner, every rule in upgrade.h, on the host. */
#include "../src/libpkg/upgrade.h"
#include <stdio.h>
#include <string.h>

static int bad = 0;
static void ok(int c, const char *l) { printf("%-4s %s\n", c ? "ok" : "FAIL", l); if (!c) bad++; }

static inv_ent E(const char *rel, const char *sha)
{
    inv_ent e; memset(&e, 0, sizeof e);
    snprintf(e.rel, sizeof e.rel, "%s", rel);
    snprintf(e.sha, sizeof e.sha, "%s", sha);
    e.size = 1;
    return e;
}

static up_act act_of(up_step *s, int n, const char *rel)
{
    int i;
    for (i = 0; i < n; i++) if (strcmp(s[i].rel, rel) == 0) return s[i].act;
    return (up_act)-1;
}

int main(void)
{
    /* old revision: Prog(a) Same(s) Gone(g) Cfg(c)
       new revision: Prog(b) Same(s) New(n)  Cfg(c)
       live:  Prog untouched, Same edited by the user, Gone untouched,
              Cfg edited by the user, Extra unknown */
    inv_ent old[] = { E("Prog","a"), E("Same","s"), E("Gone","g"), E("Cfg","c") };
    inv_ent nw[]  = { E("Prog","b"), E("Same","s"), E("New","n"),  E("Cfg","c") };
    inv_ent live[] = { E("Prog","a"), E("Same","s2"), E("Gone","g"), E("Cfg","c2"), E("Extra","x") };
    up_step steps[32]; int n, conflicts = -1;
    char why[240];

    n = upgrade_plan(old, 4, nw, 4, live, 5, steps, 32, &conflicts);
    ok(n == 6 && conflicts == 0, "six steps, no conflict for the clean case");
    ok(act_of(steps, n, "Prog") == UP_REPLACE,     "changed upstream, untouched here: replace");
    ok(act_of(steps, n, "Same") == UP_UNCHANGED,   "same upstream, edited here: the user's copy stays");
    ok(act_of(steps, n, "Cfg")  == UP_UNCHANGED,   "a config the package did not change stays as edited");
    ok(act_of(steps, n, "New")  == UP_ADD,         "new upstream, absent here: add");
    ok(act_of(steps, n, "Gone") == UP_REMOVE,      "dropped upstream, untouched here: remove");
    ok(act_of(steps, n, "Extra") == UP_KEEP_UNKNOWN, "a file we never installed is kept");
    ok(strcmp(steps[0].rel, "Cfg") == 0 && strcmp(steps[n-1].rel, "Same") == 0,
       "steps are sorted by path");

    /* the user edited a file the new revision changes: refuse */
    {
        inv_ent live2[] = { E("Prog","user"), E("Same","s"), E("Gone","g"), E("Cfg","c") };
        n = upgrade_plan(old, 4, nw, 4, live2, 4, steps, 32, &conflicts);
        ok(conflicts == 1 && act_of(steps, n, "Prog") == UP_CONFLICT,
           "changed upstream AND edited here: conflict");
    }
    /* the user already has the new content: nothing to do */
    {
        inv_ent live3[] = { E("Prog","b"), E("Same","s"), E("Gone","g"), E("Cfg","c") };
        n = upgrade_plan(old, 4, nw, 4, live3, 4, steps, 32, &conflicts);
        ok(conflicts == 0 && act_of(steps, n, "Prog") == UP_UNCHANGED,
           "live already equals the new content: unchanged");
    }
    /* an unknown file where the new revision wants one: conflict */
    {
        inv_ent live4[] = { E("Prog","a"), E("Same","s"), E("Gone","g"), E("Cfg","c"), E("New","theirs") };
        n = upgrade_plan(old, 4, nw, 4, live4, 5, steps, 32, &conflicts);
        ok(conflicts == 1 && act_of(steps, n, "New") == UP_CONFLICT,
           "something already at a path the new revision adds: conflict");
    }
    /* dropped upstream but edited here: kept */
    {
        inv_ent live5[] = { E("Prog","a"), E("Same","s"), E("Gone","g-edited"), E("Cfg","c") };
        n = upgrade_plan(old, 4, nw, 4, live5, 4, steps, 32, &conflicts);
        ok(conflicts == 0 && act_of(steps, n, "Gone") == UP_KEEP_USER,
           "dropped upstream, edited here: kept for the user");
    }
    /* the user deleted a package file: the upgrade delivers it complete */
    {
        inv_ent live6[] = { E("Same","s"), E("Gone","g"), E("Cfg","c") };
        n = upgrade_plan(old, 4, nw, 4, live6, 3, steps, 32, &conflicts);
        ok(conflicts == 0 && act_of(steps, n, "Prog") == UP_ADD,
           "a deleted package file comes back with the new revision");
    }
    /* rollback is the same planner with the roles swapped */
    {
        inv_ent after[] = { E("Prog","b"), E("Same","s2"), E("New","n"), E("Cfg","c2"), E("Extra","x") };
        n = upgrade_plan(nw, 4, old, 4, after, 5, steps, 32, &conflicts);
        ok(conflicts == 0 &&
           act_of(steps, n, "Prog") == UP_REPLACE && act_of(steps, n, "New") == UP_REMOVE &&
           act_of(steps, n, "Gone") == UP_ADD && act_of(steps, n, "Same") == UP_UNCHANGED &&
           act_of(steps, n, "Extra") == UP_KEEP_UNKNOWN,
           "rollback: new roles swapped; the user's edits survive the way back too");
        {
            inv_ent after2[] = { E("Prog","b"), E("Same","s2"), E("New","n-edited"), E("Cfg","c2") };
            n = upgrade_plan(nw, 4, old, 4, after2, 4, steps, 32, &conflicts);
            ok(conflicts == 0 && act_of(steps, n, "New") == UP_KEEP_USER,
               "rollback keeps a file the user edited after the upgrade");
        }
    }
    ok(upgrade_plan(old, 4, nw, 4, live, 5, steps, 3, &conflicts) == -1,
       "too small an output is refused, not truncated");

    ok(upgrade_allowed("2.9", 1, "2.9", 2, why, sizeof why) == 0, "same version, higher revision: allowed");
    ok(upgrade_allowed("2.9", 2, "2.9", 2, why, sizeof why) != 0 && strstr(why, "not newer"),
       "same revision: refused as not newer");
    /* Across versions: ordered only when both are dotted numbers. */
    ok(version_compare("0.1.0", "0.2.0") == -1, "0.1.0 < 0.2.0");
    ok(version_compare("1.9", "1.10") == -1, "1.9 < 1.10: numbers, not text");
    ok(version_compare("1.10", "1.9") == 1, "1.10 > 1.9");
    ok(version_compare("2", "2.0.1") == -1, "a missing component counts as 0");
    ok(version_compare("1.0", "1.0.0") == 0, "1.0 and 1.0.0 are equal as numbers");
    ok(version_compare("1.02", "1.2") == 0, "leading zeros do not change the number");
    ok(version_compare("2026.09", "2026.10") == -1, "dates as numbers");
    ok(version_compare("1.3-", "1.4") == 2, "a trailing '-' is not a number");
    ok(version_compare("v1.3", "1.4") == 2, "a 'v' prefix is not a number");
    ok(version_compare("1.3b", "1.4") == 2, "letters are not ordered");
    ok(version_compare("1..3", "1.4") == 2 && version_compare("1.3.", "1.4") == 2 &&
       version_compare(".1", "1") == 2, "empty components are not numbers");
    ok(version_compare("", "1") == 2 && version_compare(NULL, "1") == 2, "empty or missing version");
    ok(version_compare("1234567890", "1") == 2, "a component over 9 digits is refused, not overflowed");
    ok(version_compare("1.2.3.4.5.6.7.8", "1.2.3.4.5.6.7.9") == -1 &&
       version_compare("1.2.3.4.5.6.7.8.9", "2") == 2, "at most 8 components");

    /* Pre-release and rebuild tags, as the catalogue's own ports use them. */
    ok(version_compare("0.9.0-test2-r2", "0.9.0-test4") == -1, "test2-r2 < test4 (PdfCraft test 2 rebuild 2 -> test 4)");
    ok(version_compare("0.9.0-test9", "0.9.0-test10") == -1, "test9 < test10: numbers, not text");
    ok(version_compare("0.9.0-test10", "0.9.0-test9") == 1, "test10 > test9");
    ok(version_compare("0.9.0-test4", "0.9.0") == -1, "a test release comes before the final release");
    ok(version_compare("0.9.0", "0.9.0-test4") == 1, "the final release is newer than its test");
    ok(version_compare("0.9.0-test4", "0.9.1-test1") == -1, "the numbers decide first");
    ok(version_compare("0.8.9", "0.9.0-test1") == -1, "an older final release < a newer test");
    ok(version_compare("0.7.0-preview2", "0.7.0-preview2-r2") == -1, "a rebuild is newer than what it rebuilds");
    ok(version_compare("0.7.0-preview2-r2", "0.7.0-preview3") == -1, "the next preview is newer than a rebuild");
    ok(version_compare("0.1.0-rc1", "0.1.0-rc3") == -1, "rc1 < rc3");
    ok(version_compare("0.1.0-rc3", "0.1.0") == -1, "an rc comes before the release");
    ok(version_compare("1.0", "1.0-r2") == -1 && version_compare("1.0-r2", "1.0-r10") == -1, "rebuilds of a release");
    ok(version_compare("0.9.0-test4", "0.9.0-test4") == 0, "the same version");
    ok(version_compare("0.7.0-preview1", "0.7.0-test1") == 2, "different pre-release kinds are not ordered");
    ok(version_compare("0.1.0-preview5", "0.1.0-rc1") == 2, "preview and rc are not ordered either");
    ok(version_compare("0.1.0-preview5", "0.2.0-rc1") == -1, "different kinds, different numbers: the numbers decide");
    ok(version_compare("1.0-test", "1.0") == 2 && version_compare("1.0-r", "1.0") == 2, "a tag needs its number");
    ok(version_compare("1.0-r2-test1", "1.0") == 2, "the rebuild tag comes last");
    ok(version_compare("1.0-test1-test2", "1.0") == 2, "one pre-release tag");
    ok(version_compare("1.6.0-dev", "1.6.0") == 2 && version_compare("0.3.1-1", "0.3.1") == 2 &&
       version_compare("2.1-fix-1", "2.1") == 2 && version_compare("9.13beta", "9.13") == 2 &&
       version_compare("v0.9.0b1", "0.9.0") == 2 && version_compare("0.9r2", "0.9") == 2,
       "other tags the catalogue contains stay unordered");
    ok(version_compare("1.0-TEST1", "1.0") == 2, "tags are lower case");
    ok(version_compare("1.0-test1234567890", "1.0") == 2, "a tag number over 9 digits is refused");
    ok(upgrade_allowed("0.9.0-test2-r2", 1, "0.9.0-test4", 1, why, sizeof why) == 0, "0.9.0-test2-r2 -> 0.9.0-test4 allowed");
    ok(upgrade_allowed("0.9.0-test4", 1, "0.9.0-test2-r2", 1, why, sizeof why) != 0 && strstr(why, "older version"),
       "0.9.0-test4 -> 0.9.0-test2-r2 is a downgrade");
    ok(upgrade_allowed("0.9.0-test4", 1, "0.9.0", 1, why, sizeof why) == 0, "test -> final allowed");
    ok(upgrade_allowed("0.9.0", 1, "0.9.0-test4", 1, why, sizeof why) != 0 && strstr(why, "older version"),
       "final -> its test is a downgrade");
    ok(upgrade_allowed("0.7.0-preview1", 1, "0.7.0-test1", 1, why, sizeof why) != 0 && strstr(why, "refused rather than guessed"),
       "preview -> test of the same numbers is refused, not guessed");

    ok(upgrade_allowed("0.1.0", 1, "0.2.0", 1, why, sizeof why) == 0, "newer version: allowed");
    ok(upgrade_allowed("0.1.0", 5, "0.2.0", 1, why, sizeof why) == 0,
       "newer version with a lower revision number: allowed, revisions count per version");
    ok(upgrade_allowed("1.9", 1, "1.10", 1, why, sizeof why) == 0, "1.9 -> 1.10 allowed");
    ok(upgrade_allowed("2.10", 1, "2.9", 5, why, sizeof why) != 0 && strstr(why, "older version"),
       "2.10 -> 2.9 is a downgrade and refused, whatever the revision");
    ok(upgrade_allowed("0.2.0", 1, "0.1.0", 9, why, sizeof why) != 0 && strstr(why, "older version"),
       "no silent downgrade");
    ok(upgrade_allowed("1.0", 1, "1.0.0", 2, why, sizeof why) != 0 && strstr(why, "written differently"),
       "equal numbers written differently: refused");
    ok(upgrade_allowed("1.3-", 1, "1.4", 1, why, sizeof why) != 0 && strstr(why, "numbers and dots"),
       "an unordered format: refused with the reason");
    ok(upgrade_allowed("v1.3", 1, "v1.4", 1, why, sizeof why) != 0 && strstr(why, "refused rather than guessed"),
       "both unordered: refused, not guessed");
    ok(upgrade_allowed("1.3-", 1, "1.3-", 2, why, sizeof why) == 0,
       "an unordered format still gets new revisions of the same version");
    {
        char small[240];
        upgrade_allowed("12345678.12345678.1", 1, "beta-2026-10-08-x", 1, small, sizeof small);
        ok(strlen(small) < sizeof small - 1, "the longest reason fits the 240-byte buffers callers use");
    }

    printf(bad ? "\nFAIL %d\n" : "\nPASS all checks\n", bad);
    return bad != 0;
}
