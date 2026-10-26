/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak <https://tomaszstaniak.com>
 * Copyright (c) 2026 John Knipper
 * Version comparison adapted from arospkg upgrade.c. */
#include <string.h>
/* Up to 8 components of up to 9 digits each: every value fits a long, and
   anything longer is not a version number anyone compares by hand either. */
#define VMAX_PARTS 8

/* The pre-release kinds the catalogue uses. A version carries at most one,
   and two versions with different kinds for the same numbers are not
   ordered: whether a preview comes before a test is the project's choice. */
static const char *const PRE_KIND[] = { "test", "preview", "rc" };
#define NPRE ((int)(sizeof PRE_KIND / sizeof PRE_KIND[0]))

typedef struct {
    long part[VMAX_PARTS];
    int  nparts;
    int  pre;        /* -1: a final release; otherwise an index into PRE_KIND */
    long pre_n;
    long rebuild;    /* "-rN": a rebuild of the same version; 0 when absent */
} version_t;

/* 1-9 decimal digits at *v; advances *v. -1 when there are none or too many. */
static long number(const char **v)
{
    long x = 0; int digits = 0;
    while (**v >= '0' && **v <= '9') {
        if (++digits > 9) return -1;
        x = x * 10 + (*(*v)++ - '0');
    }
    return digits ? x : -1;
}

/* version := numbers ['-' kind N] ['-r' N], numbers := N ('.' N)*, kind one
   of PRE_KIND: "0.9.0", "0.9.0-test4", "0.7.0-preview2-r2", "1.0-r3". */
static int version_parse(const char *v, version_t *out)
{
    long x;
    int k;
    if (!v || !*v) return -1;
    memset(out, 0, sizeof *out);
    out->pre = -1;
    for (;;) {
        if ((x = number(&v)) < 0 || out->nparts == VMAX_PARTS) return -1;
        out->part[out->nparts++] = x;
        if (*v != '.') break;
        v++;
    }
    if (*v == '-') {
        for (k = 0; k < NPRE; k++) {
            size_t n = strlen(PRE_KIND[k]);
            if (strncmp(v + 1, PRE_KIND[k], n) == 0 && v[1 + n] >= '0' && v[1 + n] <= '9') {
                v += 1 + n;
                if ((out->pre_n = number(&v)) < 0) return -1;
                out->pre = k;
                break;
            }
        }
    }
    if (v[0] == '-' && v[1] == 'r' && v[2] >= '0' && v[2] <= '9') {
        v += 2;
        if ((out->rebuild = number(&v)) < 0) return -1;
    }
    return *v ? -1 : 0;
}

int ak_version_compare(const char *a, const char *b)
{
    version_t va, vb;
    int i;
    if (version_parse(a, &va) != 0 || version_parse(b, &vb) != 0) return 2;
    for (i = 0; i < va.nparts || i < vb.nparts; i++) {
        long x = i < va.nparts ? va.part[i] : 0, y = i < vb.nparts ? vb.part[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    /* The same numbers: a pre-release comes before the release itself. */
    if (va.pre != vb.pre) {
        if (va.pre < 0) return 1;
        if (vb.pre < 0) return -1;
        return 2;
    }
    if (va.pre >= 0 && va.pre_n != vb.pre_n) return va.pre_n < vb.pre_n ? -1 : 1;
    if (va.rebuild != vb.rebuild) return va.rebuild < vb.rebuild ? -1 : 1;
    return 0;
}
