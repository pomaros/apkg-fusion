/* jitterentropy on the target: does it start, does it give data through the
 * path Mbed TLS uses, and does a failure come back as a failure.
 *   jent-probe            init result, status, three reads
 *   jent-probe FAIL       ask for the internal timer, which is not built:
 *                         must fail, never return data */
#include <stdio.h>
#include <string.h>
#include <jitterentropy.h>
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
int aros_jent_status(void);
int main(int argc, char **argv)
{
    unsigned char b[32]; size_t ol; int i, k, r;
    if (argc > 1 && !strcmp(argv[1], "FAIL")) {
        r = jent_entropy_init_ex(0, JENT_FORCE_INTERNAL_TIMER);
        printf("init with the internal timer forced: %d (%s)\n", r, r ? "refused, as it must be" : "UNEXPECTED success");
        return r ? 0 : 10;
    }
    for (k = 0; k < 3; k++) {
        ol = 0;
        r = mbedtls_hardware_poll(NULL, b, sizeof b, &ol);
        printf("read %d: %s, %lu bytes, init rc %d: ", k + 1, r == 0 ? "PASS" : "FAIL", (unsigned long)ol, aros_jent_status());
        for (i = 0; i < 16; i++) printf("%02x", b[i]);
        printf("\n");
        if (r) return 10;
    }
    {
        static char st[2048];
        struct rand_data *ec = jent_entropy_collector_alloc(0, 0);
        if (ec && jent_status(ec, st, sizeof st) == 0) printf("%s\n", st);
        if (ec) jent_entropy_collector_free(ec);
    }
    return 0;
}
