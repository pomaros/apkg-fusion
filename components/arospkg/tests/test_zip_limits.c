/* Extraction limits and what reaches the disk: a member over 0.5.0's 64 MiB
 * limit extracts with constant memory, the 128 MiB member and 256 MiB total
 * limits still refuse, and an archive whose directory lies about a member
 * (CRC, size, stored sizes) is refused rather than written as declared. */
#include "../src/libpkg/zip.h"
#include "../src/libpkg/limits.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/resource.h>

static int bad = 0;
static void ok(int c, const char *l) { printf("%-4s %s\n", c ? "ok" : "FAIL", l); if (!c) bad++; }

static int run(const char *dir, const char *zip, char why[160])
{
    char a[512], d[512], cmd[600];
    snprintf(a, sizeof a, "%s/%s", dir, zip);
    snprintf(d, sizeof d, "%s/out-%s", dir, zip);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'; mkdir -p '%s'", d, d);
    if (system(cmd) != 0) return -2;
    return zip_extract(a, d, "App", PKG_MAX_MEMBER, PKG_MAX_TOTAL, NULL, NULL, why);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp/arospkg-zip-limits";
    char why[160], p[512];
    struct stat st;
    struct rusage ru;
    long before;

    getrusage(RUSAGE_SELF, &ru); before = ru.ru_maxrss;
    ok(run(dir, "big.zip", why) == 0, "an 80 MiB member extracts (refused by 0.5.0's 64 MiB limit)");
    snprintf(p, sizeof p, "%s/out-big.zip/App", dir);
    ok(stat(p, &st) == 0 && st.st_size == 80L * 1024 * 1024, "and has exactly its declared size");
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    ok((ru.ru_maxrss - before) / 1024 < 16 * 1024, "with constant memory: the peak grew by less than 16 MiB");
#else
    ok(ru.ru_maxrss - before < 16 * 1024, "with constant memory: the peak grew by less than 16 MiB");
#endif
    ok(run(dir, "over.zip", why) != 0 && strstr(why, "size limit"), "a 129 MiB member is over the 128 MiB limit");
    ok(run(dir, "total.zip", why) != 0 && strstr(why, "total size limit"), "300 MiB in all is over the 256 MiB limit");
    ok(run(dir, "stored.zip", why) == 0, "a stored member extracts");
    ok(run(dir, "badcrc.zip", why) != 0 && strstr(why, "CRC"), "a member whose CRC-32 does not match is refused");
    ok(run(dir, "smaller.zip", why) != 0 && strstr(why, "larger than declared"),
       "a member that expands to more than its declared size is refused (it would bypass the limits)");
    ok(run(dir, "larger.zip", why) != 0 && strstr(why, "differs"),
       "a member that expands to less than its declared size is refused");
    ok(run(dir, "storedbad.zip", why) != 0 && strstr(why, "stored member with a bad size"),
       "a stored member whose two sizes differ is refused before anything is written");
    snprintf(p, sizeof p, "%s/out-storedbad.zip/data", dir);
    ok(stat(p, &st) != 0, "and nothing of it was written");

    printf(bad ? "\nFAIL %d\n" : "\nPASS all checks\n", bad);
    return bad != 0;
}
