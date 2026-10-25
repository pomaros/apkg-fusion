/* The drawer icon's rules. Each case is a way an icon can be lost or wrongly
 * deleted; the AROS runs then check the probe-free parts on a real disk. */
#include "../src/libpkg/icon.h"
#include <stdio.h>

static int bad = 0;
static void ok(int c, const char *l) { printf("%-4s %s\n", c ? "ok" : "FAIL", l); if (!c) bad++; }

#define OURS  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define OTHER "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

int main(void)
{
    /* install */
    ok(icon_manage_on_install(1, 0) == 1, "no icon there: install ours and manage it");
    ok(icon_manage_on_install(1, 1) == 0, "an icon already there is left alone");
    ok(icon_manage_on_install(0, 0) == 0, "a package without an icon manages none");

    /* remove */
    ok(icon_on_remove(1, 1, OURS, OURS)   == ICON_DELETE,       "remove: ours, unchanged -> delete");
    ok(icon_on_remove(1, 1, OTHER, OURS)  == ICON_KEEP_CHANGED, "remove: ours, changed (snapshotted) -> keep");
    ok(icon_on_remove(1, 0, "", OURS)     == ICON_NOTHING,      "remove: already gone -> nothing");
    ok(icon_on_remove(0, 1, OURS, OURS)   == ICON_NOTHING,
       "remove: pre-existing, identical bytes -> still not ours, nothing");
    ok(icon_on_remove(1, 1, "", OURS)     == ICON_KEEP_CHANGED,
       "remove: unreadable -> kept, never deleted on a failed hash");

    /* rollback of an uncommitted install */
    ok(icon_on_rollback(1, 1, 1, OURS, OURS)  == ICON_NOTHING,
       "rollback: staged copy still present -> publish never happened -> nothing");
    ok(icon_on_rollback(1, 0, 1, OURS, OURS)  == ICON_DELETE,  "rollback: published and ours -> delete");
    ok(icon_on_rollback(1, 0, 1, OTHER, OURS) == ICON_UNKNOWN, "rollback: published, not our bytes -> stop");
    ok(icon_on_rollback(1, 0, 0, "", OURS)    == ICON_NOTHING, "rollback: absent -> nothing");
    ok(icon_on_rollback(0, 0, 1, OURS, OURS)  == ICON_NOTHING, "rollback: never managed -> nothing");

    printf(bad ? "\nFAIL %d\n" : "\nPASS all checks\n", bad);
    return bad != 0;
}
