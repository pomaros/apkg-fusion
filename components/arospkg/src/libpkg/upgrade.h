#ifndef PKG_UPGRADE_H
#define PKG_UPGRADE_H
#include "internal.h"

/* The plan of an upgrade (or of a rollback, which is an upgrade with the
 * roles swapped): what to add, replace, remove and keep, decided from three
 * inventories and nothing else, so it can be tested on the host and printed
 * before a byte moves.
 *
 *   old   what the registry says the installed revision contained
 *   new   what the staged tree of the target revision contains
 *   live  what is in the drawer now
 *
 * The rules, per path:
 *   in new and old, same content upstream  -> UNCHANGED: the live file stays
 *                                             whatever the user did to it;
 *                                             absent live -> ADD (the package
 *                                             is delivered complete)
 *   in new and old, changed upstream        -> live absent: ADD
 *                                             live == old: REPLACE
 *                                             live == new: UNCHANGED (already)
 *                                             otherwise:   CONFLICT
 *   in new only                             -> live absent: ADD
 *                                             live == new: UNCHANGED
 *                                             otherwise:   CONFLICT
 *   in old only                             -> live absent: nothing
 *                                             live == old: REMOVE
 *                                             otherwise:   KEEP_USER
 *   in live only                            -> KEEP_UNKNOWN
 *
 * A CONFLICT is a file the user changed (or put there) that the target
 * revision wants to overwrite. One conflict stops the whole operation before
 * anything is touched; there is no "take theirs" in this increment, because
 * the answer is the user's to give and nothing here can give it for them. */

typedef enum {
    UP_ADD, UP_REPLACE, UP_REMOVE, UP_UNCHANGED,
    UP_KEEP_USER, UP_KEEP_UNKNOWN, UP_CONFLICT
} up_act;

typedef struct { char rel[256]; up_act act; } up_step;

/* Returns the number of steps, or -1 if `max` is too small. *conflicts is the
 * count of UP_CONFLICT steps. Steps come out sorted by path. */
int upgrade_plan(const inv_ent *old, int nold,
                 const inv_ent *new_, int nnew,
                 const inv_ent *live, int nlive,
                 up_step *out, int max, int *conflicts);

const char *upgrade_actname(up_act);

/* Order two upstream versions, when that can be done without guessing.
 * A version is ordered if it is dotted numbers -- one to eight components,
 * each 1-9 digits ("0.2.0", "1.10", "2026.09") -- optionally followed by one
 * pre-release tag "-testN", "-previewN" or "-rcN" and then a rebuild tag
 * "-rN" ("0.9.0-test4", "0.7.0-preview2-r2", "1.0-r2"). Components compare
 * as numbers, so 1.10 > 1.9 and test9 < test10; a missing component counts
 * as 0. For the same numbers a pre-release comes before the release
 * (0.9.0-test4 < 0.9.0), and a rebuild after what it rebuilds (test2 <
 * test2-r2 < test4). Pre-releases of different kinds for the same numbers
 * ("-preview1" and "-test1") are not ordered.
 * Returns -1/0/1 (a < b, equal, a > b), or 2 when either version is not of
 * that form or the two cannot be ordered. Two versions equal as numbers but
 * written differently ("1.0" and "1.0.0", "1.02" and "1.2") return 0. */
int version_compare(const char *a, const char *b);

/* The upgrade rule, in one place. Returns 0 if `to` may replace `from`, or
 * -1 with the reason. Allowed: the same version with a higher revision, or
 * a newer version by version_compare (any revision). Refused, with the
 * reason in plain words: an older version (never a silent downgrade), the
 * same version written differently, a revision that does not increase, and
 * a change between versions that cannot be ordered ("1.3-", "v2", dates
 * with letters): those are not guessed. */
int upgrade_allowed(const char *from_version, long from_rev,
                    const char *to_version, long to_rev,
                    char *why, size_t n);
#endif
