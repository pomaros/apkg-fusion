/* Run the program's real main on a stack big enough for TLS and the archive
 * readers. A Shell on AROS One gives a program 40960 bytes by default, and
 * `apkg install` overran that on its first use by someone else (OpenLoco,
 * 2026-09-25: "Stack extends out of range" before any output, the lock left
 * held). Every test until then had run from a Shell with a larger stack, so
 * the suites never saw it. The ABIv11 runtime has no `__stack` mechanism, so
 * the swap is explicit: exec's NewStackSwap runs the function on memory of
 * our own and comes back when it returns. */
#include "pkg.h"
#include <proto/exec.h>
#include <exec/tasks.h>
#include <exec/memory.h>

#define PKG_STACK_WANT (256 * 1024)

static IPTR trampoline(IPTR fn, IPTR argc, IPTR argv)
{
    return (IPTR)((pkg_main_fn)fn)((int)argc, (char **)argv);
}

int pkg_run_with_stack(pkg_main_fn fn, int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    IPTR have = (IPTR)me->tc_SPUpper - (IPTR)me->tc_SPLower;
    struct StackSwapStruct sss;
    struct StackSwapArgs ssa;
    APTR stk;
    IPTR r;

    if (have >= PKG_STACK_WANT) return fn(argc, argv);
    stk = AllocMem(PKG_STACK_WANT, MEMF_ANY);
    if (!stk) return fn(argc, argv);   /* better a possible overrun than no run at all */
    sss.stk_Lower   = stk;
    sss.stk_Upper   = (APTR)((IPTR)stk + PKG_STACK_WANT);
    sss.stk_Pointer = sss.stk_Upper;
    ssa.Args[0] = (IPTR)fn; ssa.Args[1] = (IPTR)argc; ssa.Args[2] = (IPTR)argv;
    r = NewStackSwap(&sss, (APTR)trampoline, &ssa);
    FreeMem(stk, PKG_STACK_WANT);
    return (int)r;
}
