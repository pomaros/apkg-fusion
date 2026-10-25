#ifndef PKG_ICON_H
#define PKG_ICON_H

/* The drawer icon, <root>/<id>.info -- the one managed file that lives
 * OUTSIDE the package directory. See icon.c for why each rule is what it is. */

typedef enum {
    ICON_DELETE,        /* ours, unchanged: remove it                    */
    ICON_KEEP_CHANGED,  /* ours once, changed since: leave it, report it */
    ICON_NOTHING,       /* not ours, or not there: nothing to do         */
    ICON_UNKNOWN        /* cannot tell whose it is: stop, keep everything */
} icon_act;

/* Install: do we take charge of <root>/<id>.info? Only when nothing is there.
 * Returns 1 to install and manage it, 0 to leave whatever is there alone. */
int      icon_manage_on_install(int package_has_icon, int present_before);

/* Removal, and the roll-forward of an interrupted removal. */
icon_act icon_on_remove(int managed, int present,
                        const char *disk_sha, const char *ours);

/* Roll-back of an install that never committed. */
icon_act icon_on_rollback(int managed, int staged_present, int present,
                          const char *disk_sha, const char *ours);
#endif
