/* The classification behind verify and the dry run of a removal. Pure. */
#include "verify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *verify_statename(vf_state s)
{
    switch (s) {
    case VF_OK:      return "ok";
    case VF_CHANGED: return "changed locally";
    case VF_MISSING: return "missing";
    case VF_UNKNOWN: return "not the package's";
    }
    return "?";
}

static const inv_ent *find(const inv_ent *v, int n, const char *rel)
{
    int i;
    for (i = 0; i < n; i++) if (strcmp(v[i].rel, rel) == 0) return &v[i];
    return NULL;
}

static int cmp_item(const void *a, const void *b)
{ return strcmp(((const vf_item *)a)->rel, ((const vf_item *)b)->rel); }

int verify_classify(const inv_ent *reg, int nreg, const inv_ent *live, int nlive,
                    vf_item *out, int max, int *ok, int *changed, int *missing, int *unknown)
{
    int n = 0, i;
    *ok = *changed = *missing = *unknown = 0;
    for (i = 0; i < nreg; i++) {
        const inv_ent *l = find(live, nlive, reg[i].rel);
        vf_state s = !l ? VF_MISSING : strcmp(l->sha, reg[i].sha) == 0 ? VF_OK : VF_CHANGED;
        if (n >= max) return -1;
        snprintf(out[n].rel, sizeof out[n].rel, "%s", reg[i].rel);
        out[n++].state = s;
        if (s == VF_OK) (*ok)++; else if (s == VF_CHANGED) (*changed)++; else (*missing)++;
    }
    for (i = 0; i < nlive; i++) {
        if (find(reg, nreg, live[i].rel)) continue;
        if (n >= max) return -1;
        snprintf(out[n].rel, sizeof out[n].rel, "%s", live[i].rel);
        out[n++].state = VF_UNKNOWN;
        (*unknown)++;
    }
    if (n > 1) qsort(out, (size_t)n, sizeof *out, cmp_item);
    return n;
}

void rev_label(char *out, size_t n, const char *version, long revision)
{
    if (revision > 0) snprintf(out, n, "%s-aros%ld", version, revision);
    else snprintf(out, n, "%s", version);
}
