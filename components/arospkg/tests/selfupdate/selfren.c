/* Probe: what AROS allows a running program to do with its own file.
 *
 *   selfren <tag>
 *
 * Finds its file through PROGDIR: and its name, then: renames it to
 * <name>.old, copies <name>.old to <name>.new, renames <name>.new to
 * <name>, protects it +e, deletes <name>.old, and prints each step's
 * result and IoErr(). Run it from SYS:, RAM: and C: to see whether the
 * running copy holds a lock on its file anywhere. */
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

static void step(const char *what, LONG ok)
{
    printf("%-28s %s (IoErr %ld)\n", what, ok ? "ok" : "FAILED", ok ? 0L : (long)IoErr());
}

int main(int argc, char **argv)
{
    char name[256], dir[256], self[512], old[520], nw[520];
    BPTR in, out;
    char buf[4096];
    LONG n, ok = 1;
    const char *base;
    if (!GetProgramName((STRPTR)name, sizeof name)) return 20;
    base = FilePart((STRPTR)name);
    if (!NameFromLock(GetProgramDir(), (STRPTR)dir, sizeof dir)) return 20;
    snprintf(self, sizeof self, "%s", dir);
    AddPart((STRPTR)self, (STRPTR)base, sizeof self);
    snprintf(old, sizeof old, "%s.old", self);
    snprintf(nw, sizeof nw, "%s.new", self);
    printf("tag %s, running file %s\n", argc > 1 ? argv[1] : "-", self);
    DeleteFile((STRPTR)old); DeleteFile((STRPTR)nw);
    step("rename self -> .old", Rename((STRPTR)self, (STRPTR)old));
    in = Open((STRPTR)old, MODE_OLDFILE); out = Open((STRPTR)nw, MODE_NEWFILE);
    step("open .old and .new", in && out);
    while (in && out && (n = Read(in, buf, sizeof buf)) > 0) if (Write(out, buf, n) != n) ok = 0;
    if (in) Close(in);
    if (out) Close(out);
    step("copy .old -> .new", ok);
    step("rename .new -> self", Rename((STRPTR)nw, (STRPTR)self));
    step("protect self +e", SetProtection((STRPTR)self, 0));
    step("delete .old", DeleteFile((STRPTR)old));
    return 0;
}
