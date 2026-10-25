/* Prototype for `apkg run`, workbench mode: does OpenWorkbenchObjectA start a
 * program the way a double-click on its icon does (icon stack, tool types,
 * the drawer as current directory), with a document passed as a Workbench
 * argument, from a Shell with the default 40960-byte stack? And can the
 * caller tell whether a process really appeared? workbench.library's
 * handler loads the program asynchronously and reports a failed LoadSeg to
 * nobody (handler.c, "FIXME: report error somehow?"), so the only evidence
 * is the process itself.
 *
 *   wbrun <program> [document] >RESULTS:file
 *
 * Prints the icon's stack and tool types, the number of tasks named like
 * the program before and after the call, and how long the process took to
 * appear. Ends by itself after at most 20 s. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <proto/workbench.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <exec/execbase.h>
#include <stdio.h>
#include <string.h>

struct Library *WorkbenchBase, *IconBase;

static int count_named(const char *name)
{
    struct Node *n;
    int k = 0;
    Forbid();
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (n->ln_Name && strcmp(n->ln_Name, name) == 0) k++;
    for (n = SysBase->TaskWait.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (n->ln_Name && strcmp(n->ln_Name, name) == 0) k++;
    Permit();
    return k;
}

int main(int argc, char **argv)
{
    struct DiskObject *dob;
    const char *prog, *base;
    BPTR doclock = BNULL;
    struct TagItem tags[3];
    int before, after = 0, ticks;
    BOOL ok;

    if (argc < 2) { printf("usage: wbrun <program> [document]\n"); return 10; }
    prog = argv[1];
    base = (const char *)FilePart((STRPTR)prog);
    WorkbenchBase = OpenLibrary("workbench.library", 44);
    IconBase = OpenLibrary("icon.library", 44);
    if (!WorkbenchBase || !IconBase) { printf("libraries missing\n"); return 20; }

    printf("shell stack: %lu\n", (unsigned long)((struct Process *)FindTask(NULL))->pr_StackSize);
    dob = GetDiskObject((STRPTR)prog);
    if (!dob) printf("icon: none (%ld)\n", (long)IoErr());
    else {
        int i;
        printf("icon: type %d stack %ld\n", dob->do_Type, (long)dob->do_StackSize);
        for (i = 0; dob->do_ToolTypes && dob->do_ToolTypes[i]; i++)
            printf("tooltype: %s\n", dob->do_ToolTypes[i]);
        FreeDiskObject(dob);
    }

    tags[0].ti_Tag = TAG_DONE;
    if (argc > 2) {
        BPTR l = Lock(argv[2], ACCESS_READ);
        if (!l) { printf("document: cannot lock (%ld)\n", (long)IoErr()); return 10; }
        doclock = ParentDir(l);
        tags[0].ti_Tag = WBOPENA_ArgLock; tags[0].ti_Data = (IPTR)doclock;
        tags[1].ti_Tag = WBOPENA_ArgName; tags[1].ti_Data = (IPTR)FilePart(argv[2]);
        tags[2].ti_Tag = TAG_DONE;
        UnLock(l);
    }
    before = count_named(base);
    ok = OpenWorkbenchObjectA((STRPTR)prog, tags);
    printf("OpenWorkbenchObjectA: %s; tasks named %s before: %d\n", ok ? "TRUE" : "FALSE", base, before);
    for (ticks = 0; ok && ticks < 20 * 50; ticks += 10) {
        Delay(10);
        if ((after = count_named(base)) > before) break;
    }
    printf("tasks named %s after: %d; waited %d ticks\n", base, after, ticks);
    if (doclock) UnLock(doclock);
    CloseLibrary(IconBase);
    CloseLibrary(WorkbenchBase);
    return after > before ? 0 : 5;
}
