/* The two util.c helpers zip.c needs, in POSIX, so zip.c itself can be tested
 * on the host. Nothing else of util.c is reproduced: the rest is AROS DOS. */
#include "../src/libpkg/util.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

void u_join(char *out, size_t n, const char *a, const char *b)
{
    size_t la = strlen(a);
    if (la && (a[la-1] == '/' || a[la-1] == ':')) snprintf(out, n, "%s%s", a, b);
    else snprintf(out, n, "%s/%s", a, b);
}
int u_mkdir(const char *path) { mkdir(path, 0755); return 0; }
