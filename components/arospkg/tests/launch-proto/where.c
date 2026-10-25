/* A test program for `apkg run`: says how it was started. From a Shell it
 * prints its arguments; from Workbench, the WBStartup arguments and the
 * icon's tool types. Both: its current directory, PROGDIR: and its stack.
 * With a file named in WHERE_LOG it appends the same there, because a
 * Workbench start has no console of its own.   where [args...] */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <stdio.h>
#include <string.h>

extern struct WBStartup *WBenchMsg;
struct Library *IconBase;

static void report(FILE *f, int argc, char **argv)
{
    char cd[256] = "?", pd[256] = "?";
    struct Process *me = (struct Process *)FindTask(NULL);
    BPTR l;
    int i;
    NameFromLock(me->pr_CurrentDir, (STRPTR)cd, sizeof cd);
    if ((l = Lock((CONST_STRPTR)"PROGDIR:", ACCESS_READ))) { NameFromLock(l, (STRPTR)pd, sizeof pd); UnLock(l); }
    fprintf(f, "where: started from %s\n", WBenchMsg ? "Workbench" : "a Shell");
    fprintf(f, "where: current directory %s\n", cd);
    fprintf(f, "where: PROGDIR: %s\n", pd);
    fprintf(f, "where: stack %lu\n", (unsigned long)me->pr_StackSize);
    if (WBenchMsg) {
        for (i = 1; i < WBenchMsg->sm_NumArgs; i++) {
            char dir[256] = "?";
            NameFromLock(WBenchMsg->sm_ArgList[i].wa_Lock, (STRPTR)dir, sizeof dir);
            fprintf(f, "where: wbarg %d: %s / %s\n", i, dir, WBenchMsg->sm_ArgList[i].wa_Name);
        }
        if ((IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 0))) {
            struct DiskObject *d;
            BPTR old = CurrentDir(WBenchMsg->sm_ArgList[0].wa_Lock);
            if ((d = GetDiskObject((STRPTR)WBenchMsg->sm_ArgList[0].wa_Name))) {
                for (i = 0; d->do_ToolTypes && d->do_ToolTypes[i]; i++)
                    fprintf(f, "where: tooltype %s\n", d->do_ToolTypes[i]);
                FreeDiskObject(d);
            }
            CurrentDir(old);
            CloseLibrary(IconBase);
        }
    } else {
        for (i = 1; i < argc; i++) fprintf(f, "where: arg %d: [%s]\n", i, argv[i]);
    }
}

int main(int argc, char **argv)
{
    char log[256];
    report(stdout, argc, argv);
    if (GetVar((CONST_STRPTR)"WHERE_LOG", (STRPTR)log, sizeof log, GVF_GLOBAL_ONLY) > 0) {
        FILE *f = fopen(log, "a");
        if (f) { report(f, argc, argv); fclose(f); }
    }
    /* Long enough for a confirmation poll to see the process. */
    Delay(3 * 50);
    return argc > 1 && !strcmp(argv[1], "fail") ? 7 : 0;
}
