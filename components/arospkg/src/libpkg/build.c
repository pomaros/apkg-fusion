/* Build identity and provenance.
 *
 * PKG_BUILD_ID is the SHA-256 of the sources, set by src/build.sh. It is
 * reproducible and cheap, but it identifies the SOURCE, not the program: it
 * says nothing about which toolchain compiled it, with which flags, against
 * which SDK, or what got linked in. So the toolchain and SDK are recorded
 * beside it, and pkg_self_sha256() hashes the running file, which is the only
 * identifier that covers all of it.
 */
#include "pkg.h"
#include "sha256.h"
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#ifndef PKG_BUILD_ID
#define PKG_BUILD_ID "unknown"
#endif
#ifndef PKG_TOOLCHAIN
#define PKG_TOOLCHAIN "unknown"
#endif
#ifndef PKG_SDK
#define PKG_SDK "unknown"
#endif
#ifndef PKG_CC
#define PKG_CC "unknown"
#endif
#ifndef PKG_ABI
#define PKG_ABI "unknown"
#endif

/* A tag the host can find in the built file with `strings`, so packaging can
 * refuse to ship a mainline build as the ABIv11 release. The bare "v11" that
 * pkg_abi() returns is not findable: the linker merges it with neighbouring
 * literals, so a search for it matches nothing or matches the wrong thing. */
const char pkg_abi_tag[] = "arospkg-abi=" PKG_ABI;

const char *pkg_build_id(void)  { return PKG_BUILD_ID; }
const char *pkg_toolchain(void) { return PKG_TOOLCHAIN; }
const char *pkg_sdk(void)       { return PKG_SDK; }
const char *pkg_cc(void)        { return PKG_CC; }
const char *pkg_abi(void)       { return PKG_ABI; }

/* The CPU is the compiler's to say, not a build flag's: there is no way to
   build this file for one CPU and label it another. */
const char *pkg_arch(void)
{
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#elif defined(__i386__)
    return "i386";
#else
    return "";
#endif
}

/* GetProgramName() returns the command AS INVOKED. Typed with a path
 * ("RAM:pkg") that is enough to reopen the file; installed in C: and run as
 * "pkg" it is a bare name, and hashing it fails -- so every run report of an
 * INSTALLED pkg said binary "unknown", which is the one field the evidence
 * rule depends on. Found by the v0.1 acceptance run, which is the first test
 * that copied pkg to C: the way the README tells a user to.
 *
 * PROGDIR: is the directory the running program was loaded from, for both
 * Shell and Workbench, so it resolves the bare name. Tried second, because a
 * name that already works should not be second-guessed. */
int pkg_self_sha256(char out[65])
{
    char name[512], viaprogdir[512];
    const char *base;
    strcpy(out, "unknown");
    if (!GetProgramName((STRPTR)name, sizeof name - 1)) return -1;
    if (!name[0]) return -1;
    if (sha256_file(name, out) == 0) return 0;

    base = strrchr(name, '/');
    if (!base) base = strrchr(name, ':');
    base = base ? base + 1 : name;
    snprintf(viaprogdir, sizeof viaprogdir, "PROGDIR:%s", base);
    if (sha256_file(viaprogdir, out) == 0) return 0;
    strcpy(out, "unknown");
    return -1;
}

static const char *abi_override_s = NULL;
static int         abi_show_all_s = 0;

void        pkg_set_abi_override(const char *a) { abi_override_s = a; }
const char *pkg_abi_override(void)              { return abi_override_s; }
void        pkg_set_abi_show_all(int v)         { abi_show_all_s = v; }
int         pkg_abi_show_all(void)              { return abi_show_all_s; }

static const char *verify_name_s = NULL;
void        pkg_set_verify_name(const char *n) { verify_name_s = n; }
const char *pkg_verify_name(void)              { return verify_name_s; }
