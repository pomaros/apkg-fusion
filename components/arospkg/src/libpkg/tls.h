#ifndef PKG_TLS_H
#define PKG_TLS_H
#include <stddef.h>

/* A TLS client connection over an already connected bsdsocket socket.
 *
 * Two implementations exist, chosen by the build script: src/tls/mbedtls.c
 * (the default) and src/tls/openssl.c (the SDK's OpenSSL, kept to compare
 * against). Both verify the chain against the CA bundle and require the
 * certificate to name `verify_name`; neither has an "insecure" mode. */
typedef struct tls tls;

/* Returns a connection, or NULL with the reason in why. */
tls *tls_start(int sock, const char *host, const char *verify_name,
               const char *cafile, char why[240]);
/* Both return bytes transferred, 0 at the end of the stream, <0 on error.
 * After an error, tls_error says what failed: the socket's own error,
 * taken at the failed call on this connection, or the TLS library's. */
int  tls_write(tls *t, const void *buf, size_t n);
int  tls_read(tls *t, void *buf, size_t n);
const char *tls_error(tls *t);
/* 1 when the last read failed because the call was interrupted (EINTR):
   on mainline AROS the TCP stack breaks a blocking recv on Ctrl-C, so that
   is how a Ctrl-C during a download arrives there. */
int tls_interrupted(tls *t);
/* Testing only: the next receive on this connection fails as a reset socket
 * (TLS_TEST_RESET: recv returns -1, Errno() ECONNRESET) or delivers corrupted
 * bytes, which the TLS layer rejects (TLS_TEST_CORRUPT). Once. */
enum { TLS_TEST_RESET = 1, TLS_TEST_CORRUPT = 2 };
void tls_test_fail_next_recv(tls *t, int how);
void tls_end(tls *t);
/* Which library, for `apkg --version`. */
const char *tls_name(void);
#endif
