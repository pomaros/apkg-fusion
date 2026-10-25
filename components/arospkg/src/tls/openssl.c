/* TLS over the SDK's OpenSSL. Kept so the two implementations can be
 * compared; release builds use src/tls/mbedtls.c, which takes its
 * randomness from a source this program controls. */
#include "../libpkg/tls.h"
#include <proto/bsdsocket.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdio.h>
#include <stdlib.h>

/* OpenSSL 4.0 renamed SSL_set1_host() to SSL_set1_dnsname(). Without it,
 * SSL_VERIFY_PEER validates the chain and accepts any valid certificate for
 * any name at all. */
#if OPENSSL_VERSION_NUMBER >= 0x40000000L
#  define PKG_SET_HOSTNAME(ssl, host)  SSL_set1_dnsname((ssl), (host))
#else
#  define PKG_SET_HOSTNAME(ssl, host)  SSL_set1_host((ssl), (host))
#endif

struct tls { SSL_CTX *ctx; SSL *ssl; };

const char *tls_name(void) { return OPENSSL_VERSION_TEXT; }

tls *tls_start(int s, const char *host, const char *want, const char *cafile, char why[240])
{
    tls *t = calloc(1, sizeof *t);
    long vr;
    if (!t) { snprintf(why, 240, "out of memory"); return NULL; }
    t->ctx = SSL_CTX_new(TLS_client_method());
    if (!t->ctx) { snprintf(why, 240, "SSL_CTX_new failed"); goto fail; }
    if (SSL_CTX_load_verify_locations(t->ctx, cafile, NULL) != 1) {
        snprintf(why, 240, "cannot load the CA bundle %s -- refusing to download", cafile);
        goto fail;
    }
    SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER, NULL);
    t->ssl = SSL_new(t->ctx);
    if (!t->ssl) { snprintf(why, 240, "SSL_new failed"); goto fail; }
    SSL_set_fd(t->ssl, s);
    SSL_set_tlsext_host_name(t->ssl, host);
    if (PKG_SET_HOSTNAME(t->ssl, want) != 1) {
        snprintf(why, 240, "cannot require the certificate name %s", want);
        goto fail;
    }
    if (SSL_connect(t->ssl) != 1) {
        vr = SSL_get_verify_result(t->ssl);
        if (vr != X509_V_OK)
            snprintf(why, 240, "certificate for %s rejected: %s", host,
                     X509_verify_cert_error_string(vr));
        else
            snprintf(why, 240, "TLS handshake with %s failed", host);
        goto fail;
    }
    return t;
fail:
    tls_end(t);
    return NULL;
}

int tls_write(tls *t, const void *b, size_t n) { return SSL_write(t->ssl, b, (int)n); }
int tls_read(tls *t, void *b, size_t n)
{
    int r = SSL_read(t->ssl, b, (int)n);
    return r < 0 ? -1 : r;
}
/* The comparison backend says only that it failed. */
const char *tls_error(tls *t) { (void)t; return "TLS or socket error (OpenSSL backend)"; }
/* Not provided by the comparison backend: its tests do not cover breaks. */
void tls_test_fail_next_recv(tls *t, int how) { (void)t; (void)how; }
int tls_interrupted(tls *t) { (void)t; return 0; }
void tls_end(tls *t)
{
    if (!t) return;
    if (t->ssl) { SSL_shutdown(t->ssl); SSL_free(t->ssl); }
    if (t->ctx) SSL_CTX_free(t->ctx);
    free(t);
}
