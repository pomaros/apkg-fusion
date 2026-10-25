#ifndef PKG_UTIL_H
#define PKG_UTIL_H
#include <stddef.h>

#define PKG_UTIL_PATH 512

void  u_join(char *out, size_t n, const char *a, const char *b);
int   u_exists(const char *path);
int   u_isdir(const char *path);
int   u_mkdir(const char *path);          /* one level; 0 = ok or existed */
int   u_mkpath(const char *path);         /* every level                  */
int   u_delete(const char *path);         /* file or empty dir            */
int   u_deltree(const char *path);
int   u_copy(const char *from, const char *to);
long  u_size(const char *path);

/* Publish `tmp` as `dest`. AROS Rename() refuses to overwrite, so an existing
 * destination is removed first -- the window that leaves is exactly why the
 * journal exists. Returns 0 on success. */
int   u_publish(const char *tmp, const char *dest);

/* Write `text` to `path` via a sibling temporary and u_publish. */
int   u_write_atomicish(const char *path, const char *text);
char *u_read_all(const char *path, size_t *len);   /* caller frees */

int   u_listdir(const char *path, char ***names, int *count); /* caller frees */
void  u_freelist(char **names, int count);
#endif
