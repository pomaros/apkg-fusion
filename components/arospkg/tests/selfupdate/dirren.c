/* dirren -- what AROS lets a running program do to its own drawer.
 *
 *   dirren            run it from the drawer under test, e.g. SYS:dr/d/dirren
 *
 * A self-update of a managed arospkg swaps the package drawer by renaming
 * it, and the apkg doing that may have been started from that very drawer:
 * its PROGDIR: is a lock on it. This says, on the volume it is run from,
 * whether the drawer can be renamed while that lock is held, whether
 * PROGDIR: follows it, whether the program's own file can be renamed in
 * the moved drawer. Every step is undone before it exits.
 */
#include <proto/dos.h>
#include <dos/dos.h>
#include <stdio.h>
#include <string.h>

static void say(const char *what, LONG ok)
{
    printf("%-44s %s", what, ok ? "yes" : "NO");
    if (!ok) printf(" (IoErr %ld)", (long)IoErr());
    printf("\n");
}

int main(void)
{
    char dir[512], moved[520], name[256], self[600], selfmoved[620], tmp[640], now[512];
    BPTR pd = GetProgramDir();
    LONG ok;

    if (!pd || !NameFromLock(pd, (STRPTR)dir, sizeof dir) || !GetProgramName((STRPTR)name, sizeof name)) {
        printf("cannot name PROGDIR:\n");
        return 20;
    }
    snprintf(moved, sizeof moved, "%s-moved", dir);
    snprintf(self, sizeof self, "%s/%s", dir, (char *)FilePart((STRPTR)name));
    snprintf(selfmoved, sizeof selfmoved, "%s/%s", moved, (char *)FilePart((STRPTR)name));
    snprintf(tmp, sizeof tmp, "%s.old", selfmoved);
    printf("PROGDIR: %s\n", dir);

    ok = Rename((CONST_STRPTR)dir, (CONST_STRPTR)moved);
    say("rename the drawer while PROGDIR: holds it", ok);
    if (!ok) return 10;
    say("PROGDIR: still names it", NameFromLock(pd, (STRPTR)now, sizeof now));
    printf("PROGDIR: now %s\n", now);
    ok = Rename((CONST_STRPTR)selfmoved, (CONST_STRPTR)tmp);
    say("rename own file inside the moved drawer", ok);
    if (ok) say("  and back", Rename((CONST_STRPTR)tmp, (CONST_STRPTR)selfmoved));
    say("rename the drawer back", Rename((CONST_STRPTR)moved, (CONST_STRPTR)dir));
    return 0;
}
