/* The pure half of release.h: reading SHA256SUMS and ordering versions.
 * Moved here from apkg's self-update so PkgManager reads the same release
 * the same way. */
#include "release.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int pkg_release_vercmp(const char *a, const char *b)
{
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.' && *b == '.') { a++; b++; continue; }
        if (*a == *b) return 0;                 /* both ended */
        if (!*a) return 1;                      /* a is the release, b a pre-release */
        if (!*b) return -1;
        return strcmp(a, b) < 0 ? -1 : 1;
    }
    return 0;
}

int pkg_release_pick(const char *sums, const char *target, pkg_release *out)
{
    char suffix[48];
    const char *line, *b;
    size_t sl;
    memset(out, 0, sizeof *out);
    snprintf(out->target, sizeof out->target, "%s", target ? target : "");
    if (!target || !*target || !sums) return 0;
    snprintf(suffix, sizeof suffix, ".%s.zip", target);
    sl = strlen(suffix);
    for (line = sums; line && *line; line = b ? b + 1 : NULL) {
        char sha[80], fn[200];
        size_t fl;
        b = strchr(line, '\n');
        if (sscanf(line, "%79s %199s", sha, fn) != 2 || strlen(sha) != 64 || strncmp(fn, "arospkg-", 8)) continue;
        fl = strlen(fn);
        if (fl > 8 + sl && !strcmp(fn + fl - sl, suffix) && fl - sl - 8 < sizeof out->version) {
            out->found++;
            snprintf(out->sha256, sizeof out->sha256, "%s", sha);
            snprintf(out->archive, sizeof out->archive, "%s", fn);
            snprintf(out->version, sizeof out->version, "%.*s", (int)(fl - sl - 8), fn + 8);
        } else if (!out->other_version[0]) {
            /* arospkg-0.4.1.i386-aros-v0.zip: the version ends at the
               first dot that is not followed by a digit. */
            const char *v = fn + 8, *p = v;
            while (*p && !(*p == '.' && !(p[1] >= '0' && p[1] <= '9'))) p++;
            if (*p && p > v && (size_t)(p - v) < sizeof out->other_version)
                snprintf(out->other_version, sizeof out->other_version, "%.*s", (int)(p - v), v);
        }
    }
    return out->found;
}
