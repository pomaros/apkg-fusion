#ifndef PKG_UTIL_H
#define PKG_UTIL_H
#include <stddef.h>

#define PKG_UTIL_PATH 512

/* a + "/" + b into out; -1 (out cut off) when it does not fit in n bytes */
int   u_join(char *out, size_t n, const char *a, const char *b);
int   u_exists(const char *path);
int   u_isdir(const char *path);
int   u_mkdir(const char *path);          /* one level; 0 = ok or existed */
int   u_mkpath(const char *path);         /* every level                  */
int   u_delete(const char *path);         /* file or empty dir            */
int   u_rename(const char *from, const char *to);   /* 0 = ok              */
/* 1 when another lock holds `path` (an exclusive lock is refused with
 * ERROR_OBJECT_IN_USE); 0 when it is free or does not exist. */
int   u_in_use(const char *path);
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
/* Move every entry of directory `from` into directory `to` by renaming it,
 * keeping `to` itself: locks on `to` (a Shell path, an assign, a running
 * program's PROGDIR) stay on the same directory. Refuses an entry whose name
 * exists in `to`. Stops after `max` entries when max >= 0. Returns 0 when
 * done (or stopped at max), -1 on a failed or refused move; *moved counts. */
int   u_move_children(const char *from, const char *to, int max, int *moved);
void  u_freelist(char **names, int count);
#endif
