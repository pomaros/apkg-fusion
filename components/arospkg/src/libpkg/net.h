#ifndef PKG_NET_H
#define PKG_NET_H
#include <stddef.h>

/* Open and close bsdsocket.library. Networking is optional at run time: a
 * machine with no stack must still be able to list and remove. */
int  net_open(char why[240]);
void net_close(void);

/* Fetch `url` into `dest`, which is written as `<dest>.part` and renamed into
 * place only after the caller's checks pass -- so an interrupted download can
 * never be picked up as a complete file. Returns 0 on success.
 *
 * Every property the spike established is kept, because each of them is the
 * difference between a package manager and a way to run somebody else's code:
 * the CA bundle is mandatory and a failure to load it refuses the download;
 * the certificate must match the host we asked for, not merely chain to a
 * trusted root; only https is accepted, so a downgrade cannot happen by
 * accident; and redirects are followed but re-verified against the NEW host. */
int  net_fetch(const char *url, const char *dest, long max_bytes, char why[240]);

/* SHA-256 of the last fetched body as it was in memory before being written,
 * or "" -- computed only when pkg_keep_rejected() is set. */
const char *net_body_sha256(void);

/* Bytes of body received so far, and the total from Content-Length when the
 * server sent one (0 otherwise). Setting *cancel abandons the fetch; nothing
 * has been written to disk at that point, so there is nothing to undo. */
typedef void (*net_progress)(void *user, unsigned long done,
                             unsigned long total, int *cancel);
void net_set_progress(net_progress, void *user);
/* Testing only; see pkg_test_slow. */
void net_set_slow(int ms_per_chunk);
/* Testing only; see pkg_test_break_download. */
void net_set_break(long body_bytes, int how);

/* Where the trust store lives. "Only the binary" is not quite true: this file
 * belongs to the system and pkg refuses to download without it. */
const char *net_cafile(void);
#endif
