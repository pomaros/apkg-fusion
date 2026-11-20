/* The two util.c helpers zip.c needs, in POSIX, so zip.c itself can be tested
 * on the host. Nothing else of util.c is reproduced: the rest is AROS DOS. */
#include "../src/libpkg/util.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int u_join(char *out, size_t n, const char *a, const char *b)
{
    size_t la = strlen(a);
    int w;
    if (la && (a[la-1] == '/' || a[la-1] == ':')) w = snprintf(out, n, "%s%s", a, b);
    else w = snprintf(out, n, "%s/%s", a, b);
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}
int u_mkdir(const char *path) { mkdir(path, 0755); return 0; }
