/* https_get: fetch one HTTPS page with Mbed TLS on AROS, verifying the
 * server certificate, with entropy from jitterentropy.
 *
 *   https_get <host> [path]
 *
 * What it shows, in order:
 *   1. the entropy source is checked first; if it is unusable the program
 *      stops and makes no connection (exit 20);
 *   2. the CA bundle is loaded from ENV:SYS/Certificates/ca-bundle.crt; no
 *      bundle, no connection;
 *   3. the certificate must be valid for <host> (MBEDTLS_SSL_VERIFY_REQUIRED);
 *   4. the status line of the response is printed.
 *
 * Build (see README.txt for the full command and the link order):
 *   x86_64-aros-gcc -O2 https_get.c -I<mbedtls>/include -I<jent>/include \
 *     -DMBEDTLS_CONFIG_FILE="<mbedtls_aros_config.h>" \
 *     -L<mbedtls>/lib -L<jent>/lib \
 *     -lmbedtls -lmbedx509 -lmbedcrypto -lmbedtls_aros \
 *     -ljitterentropy_mbedtls -ljitterentropy -o https_get */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <psa/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Library *SocketBase;
#define CAFILE "ENV:SYS/Certificates/ca-bundle.crt"

/* From the jitterentropy package's adapter. */
int aros_jent_status(void);
void aros_jent_shutdown(void);

static int sock = -1;
static int net_send(void *c, const unsigned char *b, size_t n)
{ int r = send(sock, (void *)b, (int)n, 0); (void)c; return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r; }
static int net_recv(void *c, unsigned char *b, size_t n)
{ int r = recv(sock, b, (int)n, 0); (void)c; return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r; }

static int load_ca(mbedtls_x509_crt *ca)
{
    BPTR fh = Open((CONST_STRPTR)CAFILE, MODE_OLDFILE);
    LONG size; unsigned char *buf; int r;
    if (!fh) return -1;
    Seek(fh, 0, OFFSET_END); size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size <= 0 || !(buf = malloc((size_t)size + 1))) { Close(fh); return -1; }
    if (Read(fh, buf, size) != size) { Close(fh); free(buf); return -1; }
    Close(fh); buf[size] = 0;
    r = mbedtls_x509_crt_parse(ca, buf, (size_t)size + 1);   /* >0: some certs skipped */
    free(buf);
    return (r < 0 || ca->version == 0) ? -1 : 0;
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : NULL, *path = argc > 2 ? argv[2] : "/";
    mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_context ssl; mbedtls_ssl_config conf; mbedtls_x509_crt ca;
    char req[512], err[128]; unsigned char buf[512];
    struct hostent *he; struct sockaddr_in sa; int r, rc = 10;

    if (!host) { printf("usage: https_get <host> [path]\n"); return 20; }
    mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&conf); mbedtls_x509_crt_init(&ca);

    /* 1. Entropy. Seeding the DRBG runs the source; a failure here means no
       TLS at all. psa_crypto_init() seeds PSA's own generator the same way. */
    if (psa_crypto_init() != PSA_SUCCESS ||
        mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)"https_get", 9) != 0) {
        printf("no usable entropy source (jitterentropy status %d); not connecting\n",
               aros_jent_status());
        rc = 20; goto out;
    }
    /* 2. Trust anchors. */
    if (load_ca(&ca) != 0) { printf("cannot load %s; not connecting\n", CAFILE); goto out; }
    if (!(SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        printf("no network (bsdsocket.library)\n"); goto out;
    }
    if (!(he = gethostbyname((char *)host))) { printf("cannot resolve %s\n", host); goto out; }
    sock = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons(443);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    if (sock < 0 || connect(sock, (struct sockaddr *)&sa, sizeof sa) < 0) {
        printf("cannot connect to %s:443\n", host); goto out;
    }
    /* 3. TLS with required verification against <host>. Without the host
       name the certificate would be checked against nothing, so a failure
       to set it ends the program like any other. */
    if ((r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)) != 0) goto tlserr;
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    if ((r = mbedtls_ssl_setup(&ssl, &conf)) != 0) goto tlserr;
    if ((r = mbedtls_ssl_set_hostname(&ssl, host)) != 0) goto tlserr;
    mbedtls_ssl_set_bio(&ssl, NULL, net_send, net_recv, NULL);
    while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        mbedtls_strerror(r, err, sizeof err);
        printf("handshake failed: %s (verify flags 0x%x)\n", err,
               (unsigned)mbedtls_ssl_get_verify_result(&ssl));
        goto out;
    }
    printf("connected: %s, %s\n", mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl));
    /* 4. One request, the status line of the answer. */
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    do r = mbedtls_ssl_write(&ssl, (const unsigned char *)req, strlen(req));
    while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (r < 0) goto tlserr;
    do r = mbedtls_ssl_read(&ssl, buf, sizeof buf - 1);
    while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET);
    if (r < 0) goto tlserr;
    if (r > 0) { buf[r] = 0; buf[strcspn((char *)buf, "\r\n")] = 0; printf("%s\n", buf); rc = 0; }
    mbedtls_ssl_close_notify(&ssl);
    goto out;
tlserr:
    mbedtls_strerror(r, err, sizeof err);
    printf("TLS error: %s\n", err);
out:
    if (sock >= 0) CloseSocket(sock);
    if (SocketBase) CloseLibrary(SocketBase);
    mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&conf); mbedtls_x509_crt_free(&ca);
    mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&entropy);
    mbedtls_psa_crypto_free();
    aros_jent_shutdown();
    return rc;
}
