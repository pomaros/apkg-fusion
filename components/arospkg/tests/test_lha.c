/* The LHA reader. Fixtures are built beside this test (see run-host-tests.sh):
 * a stored archive, a compressed one, and two that must be refused.
 *
 * The strongest check on the decompressor is not here but in
 * tests/lha-corpus.sh, which extracts every real LHA we hold -- the AROS
 * Archives uploads and our own Micropolis release -- and compares every file
 * with the system `lha`. This file covers the rules that corpus cannot: the
 * paths a hostile or careless archive can carry.
 */
#include "../src/libpkg/arc.h"
#include "../src/libpkg/lha.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int bad = 0;
static void ok(int c, const char *l) { printf("%-4s %s\n", c ? "ok" : "FAIL", l); if (!c) bad++; }

static long filesize(const char *p)
{ struct stat st; return stat(p, &st) == 0 ? (long)st.st_size : -1; }

static int contains(const char *path, const char *want)
{
    char buf[4096]; FILE *f = fopen(path, "rb"); size_t n;
    if (!f) return 0;
    n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    return strstr(buf, want) != NULL;
}

int main(void)
{
    char why[160];

    /* the format is identified by content, not by the name it was given */
    ok(arc_kind("/tmp/arospkg-lha-plain.lha") && !strcmp(arc_kind("/tmp/arospkg-lha-plain.lha"), "lha"),
       "an LHA is recognised as one");
    ok(arc_kind("/tmp/arospkg-test.zip") && !strcmp(arc_kind("/tmp/arospkg-test.zip"), "zip"),
       "a ZIP is still recognised as one");
    ok(arc_kind("/tmp/arospkg-lha-notanarchive") == NULL,
       "something that is neither is refused rather than guessed at");
    ok(arc_extract("/tmp/arospkg-lha-notanarchive", "/tmp/arospkg-lha-out", "",
                   1L<<20, 1L<<24, 0, 0, why) != 0 && strstr(why, "not a ZIP or LHA") != NULL,
       "and it says so, instead of failing deep inside a reader");

    /* extraction, through the same door pkg uses */
    system("rm -rf /tmp/arospkg-lha-out && mkdir -p /tmp/arospkg-lha-out");
    ok(arc_extract("/tmp/arospkg-lha-plain.lha", "/tmp/arospkg-lha-out", "",
                   1L<<20, 1L<<24, 0, 0, why) == 0, "a stored (-lh0-) archive extracts");
    ok(contains("/tmp/arospkg-lha-out/Drawer/hello.txt", "hello from lha"),
       "its contents are what went in");
    ok(filesize("/tmp/arospkg-lha-out/Drawer.info") == 64, "the drawer icon beside it came out too");

    system("rm -rf /tmp/arospkg-lha-out && mkdir -p /tmp/arospkg-lha-out");
    ok(arc_extract("/tmp/arospkg-lha-packed.lha", "/tmp/arospkg-lha-out", "",
                   4L<<20, 1L<<24, 0, 0, why) == 0, "a compressed (-lh5-) archive extracts");
    ok(filesize("/tmp/arospkg-lha-out/big.txt") == 306000,
       "and a member large enough to span Huffman blocks comes out whole");

    /* strip, as install uses it for a package's own drawer */
    system("rm -rf /tmp/arospkg-lha-out && mkdir -p /tmp/arospkg-lha-out");
    ok(arc_extract("/tmp/arospkg-lha-plain.lha", "/tmp/arospkg-lha-out", "Drawer",
                   1L<<20, 1L<<24, 0, 0, why) == 0 &&
       contains("/tmp/arospkg-lha-out/hello.txt", "hello from lha"),
       "naming a subdirectory strips it, as install does for a drawer");

    /* an empty directory member: the archive means the drawer to exist */
    system("rm -rf /tmp/arospkg-lha-out && mkdir -p /tmp/arospkg-lha-out");
    ok(arc_extract("/tmp/arospkg-lha-emptydir.lha", "/tmp/arospkg-lha-out", "",
                   1L<<20, 1L<<24, 0, 0, why) == 0 &&
       system("test -d /tmp/arospkg-lha-out/Drawer/objects") == 0,
       "an -lhd- member with nothing in it is created as a directory");
    system("rm -rf /tmp/arospkg-lha-out && mkdir -p /tmp/arospkg-lha-out");
    ok(arc_extract("/tmp/arospkg-lha-emptydir.lha", "/tmp/arospkg-lha-out", "Drawer",
                   1L<<20, 1L<<24, 0, 0, why) == 0 &&
       system("test -d /tmp/arospkg-lha-out/objects") == 0 &&
       system("test -e /tmp/arospkg-lha-out/Drawer") != 0,
       "and under a stripped subdirectory it lands where the files do");

    /* one member, which is how the drawer icon is taken */
    remove("/tmp/arospkg-lha-icon");
    ok(arc_extract_member("/tmp/arospkg-lha-plain.lha", "Drawer.info",
                          "/tmp/arospkg-lha-icon", 1L<<20, why) == 0 &&
       filesize("/tmp/arospkg-lha-icon") == 64, "a single member extracts on its own");
    ok(arc_extract_member("/tmp/arospkg-lha-plain.lha", "Nope.info",
                          "/tmp/arospkg-lha-icon2", 1L<<20, why) != 0 &&
       strncmp(why, "no member", 9) == 0, "a member that is not there says so");

    /* the rules that matter: what a member may be called */
    ok(arc_extract("/tmp/arospkg-lha-absolute.lha", "/tmp/arospkg-lha-out", "",
                   1L<<20, 1L<<24, 0, 0, why) != 0 && strstr(why, "unsafe") != NULL,
       "an AROS absolute path (ram:thing) is refused -- a real archive does this");
    ok(filesize("/tmp/arospkg-lha-out/ram:evil.txt") < 0,
       "and nothing was written when it was refused");
    ok(arc_extract("/tmp/arospkg-lha-parent.lha", "/tmp/arospkg-lha-out", "",
                   1L<<20, 1L<<24, 0, 0, why) != 0 && strstr(why, "unsafe") != NULL,
       "a path escaping with .. is refused");

    /* size limits, the same as the ZIP reader's */
    ok(arc_extract("/tmp/arospkg-lha-packed.lha", "/tmp/arospkg-lha-out", "",
                   1000L, 1L<<24, 0, 0, why) != 0 && strstr(why, "size limit") != NULL,
       "a member over the per-file limit is refused");

    printf(bad ? "\nFAIL %d\n" : "\nPASS all checks\n", bad);
    return bad != 0;
}
