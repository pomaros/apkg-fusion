/* Regression test for the drawer-icon extraction, using a real archive shaped
 * like the five that were refused on AROS One: a drawer with a member over the
 * icon's 1 MB limit, and the icon beside the drawer. */
#include "../src/libpkg/zip.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int bad = 0;
static void ok(int c, const char *l) { printf("%-4s %s\n", c ? "ok" : "FAIL", l); if (!c) bad++; }

int main(int argc, char **argv)
{
    char why[160];
    struct stat st;
    const char *zip = argc > 1 ? argv[1] : "/tmp/arospkg-test.zip";
    remove("/tmp/arospkg-icon.info");

    ok(zip_extract_member(zip, "Game.info", "/tmp/arospkg-icon.info",
                          1L * 1024 * 1024, why) == 0,
       "the icon extracts although another member is over its size limit");
    ok(stat("/tmp/arospkg-icon.info", &st) == 0 && st.st_size == 64,
       "the extracted icon is the 64-byte member");

    ok(zip_extract_member(zip, "Nope.info", "/tmp/arospkg-nope.info",
                          1L * 1024 * 1024, why) != 0 && strncmp(why, "no member", 9) == 0,
       "a missing icon fails, and says it is missing");

    ok(zip_extract_member(zip, "Game/Big", "/tmp/arospkg-big",
                          1L * 1024 * 1024, why) != 0 && strstr(why, "size limit") != NULL,
       "the limit still applies to the member actually extracted");

    ok(zip_extract_member("/tmp/arospkg-unsafe.zip", "Game.info", "/tmp/arospkg-x",
                          1L * 1024 * 1024, why) != 0 && strstr(why, "unsafe") != NULL,
       "an unsafe path ANYWHERE still rejects the archive, whichever member is asked for");

    printf(bad ? "\nFAIL %d\n" : "\nPASS all checks\n", bad);
    return bad != 0;
}
