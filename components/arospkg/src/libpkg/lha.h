#ifndef PKG_LHA_H
#define PKG_LHA_H
#include <stddef.h>

/* LHA, the Amiga's archive format. Same shape as zip.h on purpose: the two
 * readers are interchangeable behind arc.c, and their validation rules are
 * identical because the danger is. */

typedef int (*lha_cb)(const char *rel, const char *abs_path, void *user);

int lha_extract(const char *archive, const char *dest, const char *strip,
                long max_file, long max_total,
                lha_cb cb, void *user, char why[160]);

/* One member, named by its full path inside the archive, to the file
 * `dest_file`. Fails if it is not there. */
int lha_extract_member(const char *archive, const char *member,
                       const char *dest_file, long max_file, char why[160]);

/* Does this file look like an LHA? Read from the file, never from its name. */
int lha_is_lha(const char *archive);
#endif
