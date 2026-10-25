#include "hint.h"
#include <stdio.h>
#include <strings.h>

/* The command a user can copy. Without --root it would act on SYS:Packages,
   so a custom root is passed on, quoted for the Shell: inside quotes
   AmigaDOS escapes '"' and '*' with a leading '*'. */
void cmd_hint(char *out, size_t n, const char *root, const char *args)
{
    size_t k;
    const char *p;
    if (!root || !strcasecmp(root, "SYS:Packages")) {
        snprintf(out, n, "apkg %s", args);
        return;
    }
    k = (size_t)snprintf(out, n, "apkg --root \"");
    for (p = root; *p && k + 3 < n; p++) {
        if (*p == '"' || *p == '*') out[k++] = '*';
        out[k++] = *p;
    }
    snprintf(out + k, n - k, "\" %s", args);
}

void open_hint(char *out, size_t n, const char *root, const char *id)
{
    char args[96];
    snprintf(args, sizeof args, "open %s", id);
    cmd_hint(out, n, root, args);
}
