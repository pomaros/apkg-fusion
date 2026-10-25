/* The upgrade planner. Pure: inventories in, steps out. See upgrade.h. */
#include "upgrade.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *upgrade_actname(up_act a)
{
    switch (a) {
    case UP_ADD:          return "add";
    case UP_REPLACE:      return "replace";
    case UP_REMOVE:       return "remove";
    case UP_UNCHANGED:    return "unchanged";
    case UP_KEEP_USER:    return "keep (changed by the user)";
    case UP_KEEP_UNKNOWN: return "keep (not ours)";
    case UP_CONFLICT:     return "CONFLICT";
    }
    return "?";
}

static const inv_ent *find(const inv_ent *v, int n, const char *rel)
{
    int i;
    for (i = 0; i < n; i++) if (strcmp(v[i].rel, rel) == 0) return &v[i];
    return NULL;
}

static int cmp_step(const void *a, const void *b)
{ return strcmp(((const up_step *)a)->rel, ((const up_step *)b)->rel); }

static int same(const inv_ent *a, const inv_ent *b)
{ return a && b && strcmp(a->sha, b->sha) == 0; }

int upgrade_plan(const inv_ent *old, int nold,
                 const inv_ent *new_, int nnew,
                 const inv_ent *live, int nlive,
                 up_step *out, int max, int *conflicts)
{
    int n = 0, i, pass;
    *conflicts = 0;

    /* Three passes over the union, each path decided once: new first (adds,
       replacements, conflicts), then old-only (removals, kept user files),
       then live-only (unknown files). */
    for (pass = 0; pass < 3; pass++) {
        const inv_ent *src = pass == 0 ? new_ : pass == 1 ? old : live;
        int cnt = pass == 0 ? nnew : pass == 1 ? nold : nlive;
        for (i = 0; i < cnt; i++) {
            const char *rel = src[i].rel;
            const inv_ent *o = find(old, nold, rel), *nw = find(new_, nnew, rel),
                          *l = find(live, nlive, rel);
            up_act act;
            if (pass == 1 && nw) continue;          /* decided in pass 0 */
            if (pass == 2 && (nw || o)) continue;   /* decided earlier */
            if (nw && o) {
                if (same(nw, o))           act = l ? UP_UNCHANGED : UP_ADD;
                else if (!l)               act = UP_ADD;
                else if (same(l, o))       act = UP_REPLACE;
                else if (same(l, nw))      act = UP_UNCHANGED;
                else                       act = UP_CONFLICT;
            } else if (nw) {
                if (!l)                    act = UP_ADD;
                else if (same(l, nw))      act = UP_UNCHANGED;
                else                       act = UP_CONFLICT;
            } else if (o) {
                if (!l)                    continue;          /* already gone */
                else if (same(l, o))       act = UP_REMOVE;
                else                       act = UP_KEEP_USER;
            } else {
                act = UP_KEEP_UNKNOWN;
            }
            if (n >= max) return -1;
            snprintf(out[n].rel, sizeof out[n].rel, "%s", rel);
            out[n].act = act;
            if (act == UP_CONFLICT) (*conflicts)++;
            n++;
        }
    }
    if (n > 1) qsort(out, (size_t)n, sizeof *out, cmp_step);
    return n;
}

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

int version_compare(const char *a, const char *b)
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

int upgrade_allowed(const char *from_version, long from_rev,
                    const char *to_version, long to_rev,
                    char *why, size_t n)
{
    if (strcmp(from_version, to_version) == 0) {
        if (to_rev <= from_rev) {
            snprintf(why, n, "installed is %s revision %ld and the index offers "
                     "revision %ld, which is not newer", from_version, from_rev, to_rev);
            return -1;
        }
        return 0;
    }
    switch (version_compare(from_version, to_version)) {
    case -1:
        return 0;
    case 1:
        snprintf(why, n, "installed is version %s, newer than the %s the index offers: "
                 "apkg does not go back to an older version", from_version, to_version);
        return -1;
    case 0:
        snprintf(why, n, "installed is version %s and the index offers %s, the same number "
                 "written differently: refused rather than guessed", from_version, to_version);
        return -1;
    default:
        snprintf(why, n, "installed is version %s, the index offers %s: only versions of "
                 "numbers and dots are ordered, so this is refused rather than guessed "
                 "(remove and install to change version)", from_version, to_version);
        return -1;
    }
}
