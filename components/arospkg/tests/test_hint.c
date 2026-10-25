#include "../src/pkg/hint.h"
#include <stdio.h>
#include <string.h>

static int fails;
static void eq(const char *root, const char *want)
{
    char got[256];
    open_hint(got, sizeof got, root, "gmore");
    if (strcmp(got, want)) { printf("FAIL %s: got [%s] want [%s]\n", root ? root : "(default)", got, want); fails++; }
}

int main(void)
{
    eq(NULL, "apkg open gmore");
    eq("SYS:Packages", "apkg open gmore");
    eq("sys:packages", "apkg open gmore");
    eq("SYS:UxA", "apkg --root \"SYS:UxA\" open gmore");
    eq("SYS:Ux Test/Root", "apkg --root \"SYS:Ux Test/Root\" open gmore");
    eq("Work:a\"b*c", "apkg --root \"Work:a*\"b**c\" open gmore");
    {
        char got[256];
        cmd_hint(got, sizeof got, "Work:My Apps", "outdated");
        if (strcmp(got, "apkg --root \"Work:My Apps\" outdated")) { printf("FAIL cmd_hint: [%s]\n", got); fails++; }
        cmd_hint(got, sizeof got, NULL, "upgrade --all");
        if (strcmp(got, "apkg upgrade --all")) { printf("FAIL cmd_hint default: [%s]\n", got); fails++; }
    }
    printf(fails ? "test_hint: %d failed\n" : "test_hint: ok\n", fails);
    return fails != 0;
}
