/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The name a file's previous contents take while it is replaced without an
 * atomic replace primitive. Deterministic, so every reader and every later
 * writer finds it after an interruption, and short: FFS names are at most 30
 * characters and the file's own name may already be that long. */
#ifndef PKG_FS_ASIDE_H
#define PKG_FS_ASIDE_H

#include <stdio.h>
#include <string.h>

/* "<dir>/.pkgold" and the FNV-1a hash of the file's name. 0, or -1 when the
 * result does not fit. */
static int pkg_fs_aside_name(const char *path, char *out, size_t size)
{
    const char *base = path, *p;
    unsigned long h = 2166136261ul;
    int n;
    for (p = path; *p; p++)
        if (*p == '/' || *p == ':')
            base = p + 1;
    for (p = base; *p; p++)
        h = ((h ^ (unsigned char)*p) * 16777619ul) & 0xFFFFFFFFul;
    n = snprintf(out, size, "%.*s.pkgold%08lx", (int)(base - path), path, h);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

#endif
