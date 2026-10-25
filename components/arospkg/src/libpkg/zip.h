#ifndef PKG_ZIP_H
#define PKG_ZIP_H
#include <stddef.h>

/* Called for each extracted file. `rel` is the path relative to the extraction
 * root, already validated. Return 0 to continue. */
typedef int (*zip_cb)(const char *rel, const char *abs_path, void *user);

/* Extract `archive` into `dest`, which must exist. `strip` names a leading
 * directory to remove from every member ("" = keep as is). Returns 0, or a
 * negative code; `why` gets a one-line reason. */
int zip_extract(const char *archive, const char *dest, const char *strip,
                long max_file, long max_total,
                zip_cb cb, void *user, char why[160]);

/* Extract exactly one member, named by its full path inside the archive, to
 * the file `dest_file`. Fails if the member is not there. */
int zip_extract_member(const char *archive, const char *member,
                       const char *dest_file, long max_file, char why[160]);
#endif
