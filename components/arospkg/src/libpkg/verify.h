#ifndef PKG_VERIFY_H
#define PKG_VERIFY_H
#include "internal.h"

/* What `verify` says about each path, from the registry's inventory and the
 * drawer's -- pure, so the host tests cover it, and shared with the dry run
 * of a removal, which is the same classification read as intentions.
 *
 *   OK        in the registry, on disk, same content
 *   CHANGED   in the registry, on disk, different content (changed locally)
 *   MISSING   in the registry, not on disk
 *   UNKNOWN   on disk, not in the registry (not the package's)
 *
 * Nothing here is a verdict about who is right; verify repairs nothing. */
typedef enum { VF_OK, VF_CHANGED, VF_MISSING, VF_UNKNOWN } vf_state;
typedef struct { char rel[256]; vf_state state; } vf_item;

/* Returns the item count, or -1 if `max` is too small. Sorted by path. */
int verify_classify(const inv_ent *reg, int nreg, const inv_ent *live, int nlive,
                    vf_item *out, int max, int *ok, int *changed, int *missing, int *unknown);

const char *verify_statename(vf_state);

/* "2.2" for a revision the index did not state, "2.2-aros2" otherwise. Zero
 * is the internal marker for "not stated" and is never shown as aros0. */
void rev_label(char *out, size_t n, const char *version, long revision);
#endif
