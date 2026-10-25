#ifndef PKG_ARC_H
#define PKG_ARC_H
#include <stddef.h>

/* One door to the two archive readers. Which one is decided by what is IN the
 * file, never by its name: a package's URL is metadata and metadata can be
 * wrong, while the first bytes of an archive cannot lie about what they are. */

typedef int (*arc_cb)(const char *rel, const char *abs_path, void *user);

int arc_extract(const char *archive, const char *dest, const char *strip,
                long max_file, long max_total,
                arc_cb cb, void *user, char why[160]);

int arc_extract_member(const char *archive, const char *member,
                       const char *dest_file, long max_file, char why[160]);

/* "zip", "lha", or NULL when it is neither. */
const char *arc_kind(const char *archive);
#endif
