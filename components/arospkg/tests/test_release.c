/* Reading a release's SHA256SUMS (src/libpkg/release.c) on the host. */
#include "../src/libpkg/release.h"
#include <stdio.h>
#include <string.h>

static int bad;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); bad++; } else printf("ok   %s\n", m); } while (0)
#define H "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

int main(void)
{
    const char *sums = H "  arospkg-0.4.1.x86_64-aros-v11.zip\n" H "  arospkg-0.4.1.i386-aros-v0.zip\n";
    const char *two = H "  arospkg-0.4.1.x86_64-aros-v11.zip\n" H "  arospkg-0.4.2.x86_64-aros-v11.zip\n";
    pkg_release r;
    CHECK(pkg_release_pick(sums, "x86_64-aros-v11", &r) == 1 && !strcmp(r.version, "0.4.1")
          && !strcmp(r.archive, "arospkg-0.4.1.x86_64-aros-v11.zip"), "picks this target");
    CHECK(pkg_release_pick(sums, "aarch64-aros-raspi", &r) == 0 && !strcmp(r.other_version, "0.4.1"),
          "no archive for the Pi; the release's version is still known");
    CHECK(pkg_release_pick(two, "x86_64-aros-v11", &r) == 2, "two archives for one target are counted");
    CHECK(pkg_release_pick(sums, "x86_64-aros-v1", &r) == 0, "v1 is not v11");
    CHECK(pkg_release_pick("garbage\n", "x86_64-aros-v11", &r) == 0, "garbage");
    CHECK(pkg_release_vercmp("0.4.0", "0.4.1") < 0 && pkg_release_vercmp("0.4.10", "0.4.9") > 0
          && pkg_release_vercmp("0.4.1", "0.4.1") == 0 && pkg_release_vercmp("0.4.1", "0.4.1-rc1") > 0,
          "version order");
    printf(bad ? "\nFAILED %d\n" : "\nall release checks pass\n", bad);
    return bad != 0;
}
