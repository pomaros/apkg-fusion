/* A member whose path under the destination does not fit in a path buffer is
 * refused, for ZIP and LHA alike, instead of being written under a cut-off
 * name (the path used to be joined with an unchecked snprintf). The same
 * archives extract normally into a short destination. */
#include "../src/libpkg/arc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static int bad = 0;
static void ok(int c, const char *l) { printf("%-4s %s\n", c ? "ok" : "FAIL", l); if (!c) bad++; }

/* Entries in dir whose name starts with 'n' (a cut-off long name would be one) */
static int count_n(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int n = 0;
    if (!d) return 0;
    while ((de = readdir(d)))
        if (de->d_name[0] == 'n') n++;
    closedir(d);
    return n;
}

int main(void)
{
    char why[160], longdest[400], drawer[460], name[700];
    struct stat st;
    const char *archives[] = { "/tmp/arospkg-zip-longname.zip", "/tmp/arospkg-lha-longname.lha" };
    int i;

    char d200[201], e90[91], n200[201];
    memset(d200, 'd', 200); d200[200] = 0;
    memset(e90, 'e', 90); e90[90] = 0;
    memset(n200, 'n', 200); n200[200] = 0;
    /* /tmp/arospkg-lp/<200 d>/<90 e>: 307 bytes; + "/Drawer/" + 200 = 515 */
    snprintf(longdest, sizeof longdest, "/tmp/arospkg-lp/%s/%s", d200, e90);
    snprintf(drawer, sizeof drawer, "%s/Drawer", longdest);

    for (i = 0; i < 2; i++) {
        char label[200];
        system("rm -rf /tmp/arospkg-lp /tmp/arospkg-lp-short");
        mkdir("/tmp/arospkg-lp", 0755);
        {
            char mid[300];
            snprintf(mid, sizeof mid, "%.*s", (int)(strrchr(longdest, '/') - longdest), longdest);
            mkdir(mid, 0755);
        }
        mkdir(longdest, 0755);

        snprintf(label, sizeof label, "%s: a member path over the limit is refused", archives[i]);
        ok(arc_extract(archives[i], longdest, "", 1L<<20, 1L<<24, 0, 0, why) != 0
           && strstr(why, "path too long") != NULL, label);
        snprintf(label, sizeof label, "%s: and no file was written under a cut-off name", archives[i]);
        ok(count_n(drawer) == 0, label);

        mkdir("/tmp/arospkg-lp-short", 0755);
        snprintf(label, sizeof label, "%s: the same archive extracts into a short destination", archives[i]);
        snprintf(name, sizeof name, "/tmp/arospkg-lp-short/Drawer/%s", n200);
        ok(arc_extract(archives[i], "/tmp/arospkg-lp-short", "", 1L<<20, 1L<<24, 0, 0, why) == 0
           && stat(name, &st) == 0 && st.st_size == 10, label);
    }
    system("rm -rf /tmp/arospkg-lp /tmp/arospkg-lp-short");

    printf(bad ? "\nFAIL %d\n" : "\nPASS all checks\n", bad);
    return bad != 0;
}
