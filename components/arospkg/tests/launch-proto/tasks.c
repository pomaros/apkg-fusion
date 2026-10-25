/* Probe: which task names does a walk of TaskReady/TaskWait see, and what
 * does FindTask(name) answer? wbrun counted 38 tasks named "Folio" before
 * Folio was started (pool v11-2, 2026-10-12), so the walk is not what it
 * looks like.   tasks <name> >RESULTS:file */
#include <proto/exec.h>
#include <exec/execbase.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    struct Node *n;
    static char names[200][40];
    int k = 0, i, match = 0;
    const char *want = argc > 1 ? argv[1] : "Folio";
    Forbid();
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ && k < 200; n = n->ln_Succ)
        snprintf(names[k++], 40, "R %s", n->ln_Name ? n->ln_Name : "(null)");
    for (n = SysBase->TaskWait.lh_Head; n->ln_Succ && k < 200; n = n->ln_Succ)
        snprintf(names[k++], 40, "W %s", n->ln_Name ? n->ln_Name : "(null)");
    Permit();
    for (i = 0; i < k; i++) {
        printf("%s\n", names[i]);
        if (strcmp(names[i] + 2, want) == 0) match++;
    }
    printf("total %d, named %s: %d, FindTask: %p\n", k, want, match, (void *)FindTask((CONST_STRPTR)want));
    return 0;
}
