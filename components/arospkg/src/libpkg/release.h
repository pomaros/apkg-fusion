/* The latest stable arospkg release for this build: what `apkg self-update`
 * installs, and what PkgManager's "Check for a newer version" reports. The
 * release is GitHub's releases/latest (prereleases never), and in it
 * SHA256SUMS, one line per archive, "<sha256>  arospkg-<ver>.<target>.zip". */
#ifndef PKG_RELEASE_H
#define PKG_RELEASE_H
#include "pkg.h"
#include <stddef.h>

#define PKG_RELEASE_LATEST "https://github.com/tomaszstaniak/arospkg/releases/latest/download/"
#define PKG_RELEASE_PAGE   "https://github.com/tomaszstaniak/arospkg/releases/latest"

typedef struct {
    char target[40];       /* this build's archive target; "" when it has none */
    int  found;            /* archives for the target in SHA256SUMS: 1 is the only good answer */
    char version[40];      /* the release's version for the target ("" when not found) */
    char other_version[40];/* the version the release has for other targets */
    char archive[160];     /* arospkg-<version>.<target>.zip */
    char sha256[65];
    int  cmp;              /* current against version: -1 older, 0 same, 1 newer (when found == 1) */
} pkg_release;

/* Pure. Reads the SHA256SUMS text for `target`; fills out and returns
   out->found. */
int pkg_release_pick(const char *sums, const char *target, pkg_release *out);
/* 1.2.3[-x]: numbers first; a release outranks its own pre-releases. Pure. */
int pkg_release_vercmp(const char *a, const char *b);

/* This build's archive target ("x86_64-aros-v11", "aarch64-aros-raspi"), or
   NULL when no release is ever built for it. */
const char *pkg_release_target(void);

/* Fetch the latest release's SHA256SUMS (from `base`, or the default when
   NULL; `sums_file` reads a local copy instead, for tests), pick this
   build's archive and compare it with `current`. Network only through
   net.c; nothing is written but a temporary file in T: or RAM:. */
pkg_status pkg_release_check(const char *base, const char *sums_file, const char *current,
                             pkg_release *out, pkg_err *e);
#endif
