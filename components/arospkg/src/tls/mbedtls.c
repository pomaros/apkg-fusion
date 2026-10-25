/* TLS over Mbed TLS 3.6 (third_party/mbedtls), compiled into apkg.
 *
 * The chain is verified against the system's CA bundle and the certificate
 * must name `want`; MBEDTLS_SSL_VERIFY_REQUIRED, never optional. Randomness
 * comes from src/tls/entropy_aros.c, not from the C library. */
#include "../libpkg/tls.h"
#include <sys/socket.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/version.h>
#include <mbedtls/error.h>
#include <errno.h>
#include <psa/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct tls {
    int sock;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    int sock_errno;     /* Errno() at the last failed send or recv, else 0 */
    int ssl_err;        /* the last error tls_read or tls_write returned */
    int trap;           /* testing only: tls_test_fail_next_recv */
    char err[160];
};

const char *tls_name(void) { return "Mbed TLS " MBEDTLS_VERSION_STRING; }

static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    tls *t = (tls *)ctx;
    int r = send(t->sock, (void *)b, (int)n, 0);
    /* Errno() is read here, at the failed call: by the time Mbed TLS hands
       its own code back, another call may have changed it. */
    if (r < 0) { t->sock_errno = Errno(); return MBEDTLS_ERR_SSL_INTERNAL_ERROR; }
    return r;
}

static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    tls *t = (tls *)ctx;
    int r, i;
    if (t->trap == TLS_TEST_RESET) {
        t->trap = 0;
        t->sock_errno = ECONNRESET;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    r = recv(t->sock, b, (int)n, 0);
    if (r < 0) { t->sock_errno = Errno(); return MBEDTLS_ERR_SSL_INTERNAL_ERROR; }
    if (t->trap == TLS_TEST_CORRUPT && r > 0) {
        t->trap = 0;
        for (i = 0; i < r; i++) b[i] ^= 0x5a;
    }
    return r;
}

/* The bundle is read here rather than through Mbed TLS's file layer, which
 * is compiled out. A bundle with some certificates Mbed TLS cannot parse is
 * accepted as long as it parsed at least one: the others are then simply
 * not trusted, which is the safe direction. */
static int load_ca(mbedtls_x509_crt *ca, const char *path, char why[240])
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    LONG size, got;
    unsigned char *buf;
    int r;
    if (!fh) {
        snprintf(why, 240, "cannot load the CA bundle %s -- refusing to download", path);
        return -1;
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size <= 0 || size > 4 * 1024 * 1024 || !(buf = malloc((size_t)size + 1))) {
        Close(fh);
        snprintf(why, 240, "the CA bundle %s is empty or too large", path);
        return -1;
    }
    got = Read(fh, buf, size);
    Close(fh);
    if (got != size) {
        free(buf);
        snprintf(why, 240, "cannot read the CA bundle %s", path);
        return -1;
    }
    buf[size] = 0;
    r = mbedtls_x509_crt_parse(ca, buf, (size_t)size + 1);
    free(buf);
    if (r < 0 || ca->version == 0) {
        snprintf(why, 240, "no usable certificate in the CA bundle %s", path);
        return -1;
    }
    return 0;
}

tls *tls_start(int s, const char *host, const char *want, const char *cafile, char why[240])
{
    static const unsigned char pers[] = "apkg";
    tls *t = calloc(1, sizeof *t);
    int r;
    (void)host;
    if (!t) { snprintf(why, 240, "out of memory"); return NULL; }
    t->sock = s;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->ca);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->drbg);

    if (psa_crypto_init() != PSA_SUCCESS) {
        snprintf(why, 240, "no good randomness on this machine; refusing to connect");
        goto fail;
    }
    if (mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, pers, sizeof pers) != 0) {
        snprintf(why, 240, "no good randomness on this machine; refusing to connect");
        goto fail;
    }
    if (load_ca(&t->ca, cafile, why) != 0) goto fail;
    if (mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        snprintf(why, 240, "TLS setup failed");
        goto fail;
    }
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    if (mbedtls_ssl_setup(&t->ssl, &t->conf) != 0) { snprintf(why, 240, "TLS setup failed"); goto fail; }
    /* One name for SNI and for the check. With --verify-name a test asks
       for another name on purpose, and the handshake must then fail. */
    if (mbedtls_ssl_set_hostname(&t->ssl, want) != 0) {
        snprintf(why, 240, "cannot require the certificate name %s", want);
        goto fail;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);

    while ((r = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        {
            uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
            if (flags && flags != (uint32_t)-1) {
                char info[160];
                mbedtls_x509_crt_verify_info(info, sizeof info, "", flags);
                info[strcspn(info, "\n")] = 0;
                snprintf(why, 240, "certificate for %s rejected: %s", want, info);
            } else {
                snprintf(why, 240, "TLS handshake with %s failed (-0x%04x)", want, (unsigned)-r);
            }
        }
        goto fail;
    }
    return t;
fail:
    tls_end(t);
    return NULL;
}

int tls_write(tls *t, const void *b, size_t n)
{
    int r;
    t->sock_errno = t->ssl_err = 0;
    do r = mbedtls_ssl_write(&t->ssl, b, n);
    while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (r < 0) { t->ssl_err = r; return -1; }
    return r;
}

void tls_test_fail_next_recv(tls *t, int how) { if (t) t->trap = how; }

int tls_interrupted(tls *t) { return t && t->sock_errno == EINTR; }

/* The socket's error when there was one: that is what failed, and Mbed TLS
   only reports it as an internal error. Otherwise the TLS library's. */
const char *tls_error(tls *t)
{
    char m[100];
    if (!t) return "";
    if (t->sock_errno == ECONNRESET)
        snprintf(t->err, sizeof t->err, "connection reset");
    else if (t->sock_errno == ETIMEDOUT)
        snprintf(t->err, sizeof t->err, "connection timed out");
    else if (t->sock_errno == EINTR)
        snprintf(t->err, sizeof t->err, "interrupted (Ctrl-C)");
    else if (t->sock_errno)
        snprintf(t->err, sizeof t->err, "socket error %d", t->sock_errno);
    else if (t->ssl_err) {
        mbedtls_strerror(t->ssl_err, m, sizeof m);
        snprintf(t->err, sizeof t->err, "TLS error -0x%04x: %s", (unsigned)-t->ssl_err, m);
    } else t->err[0] = 0;
    return t->err;
}

int tls_read(tls *t, void *b, size_t n)
{
    int r;
    t->sock_errno = t->ssl_err = 0;
    for (;;) {
        r = mbedtls_ssl_read(&t->ssl, b, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        /* TLS 1.3 sends tickets after the handshake; they are not data. */
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
        break;
    }
    /* A server that closes without close_notify ends the stream too; the
       HTTP layer checks lengths and the caller checks the hash. */
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0) return 0;
    if (r < 0) { t->ssl_err = r; return -1; }
    return r;
}

void tls_end(tls *t)
{
    if (!t) return;
    mbedtls_ssl_close_notify(&t->ssl);
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->ca);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    free(t);
}
