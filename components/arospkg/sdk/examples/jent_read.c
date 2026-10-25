/* jent_read: start jitterentropy on AROS and read 32 bytes from it, the way
 * any user of the library would, without Mbed TLS.
 *   jent_read      exit 0 and the bytes in hex; exit 20 if the source is
 *                  unusable on this machine (never data in that case).
 * Build: x86_64-aros-gcc -O2 jent_read.c -I<jent>/include -L<jent>/lib \
 *          -ljitterentropy -o jent_read */
#include <stdio.h>
#include <jitterentropy.h>
int main(void)
{
    struct rand_data *ec; char out[32]; int r, i;
    if ((r = jent_entropy_init()) != 0) {
        printf("jitterentropy cannot run here: start-up test error %d\n", r);
        return 20;
    }
    if (!(ec = jent_entropy_collector_alloc(0, 0))) { printf("no memory\n"); return 20; }
    if (jent_read_entropy(ec, out, sizeof out) != (ssize_t)sizeof out) {
        printf("read failed (health test)\n"); jent_entropy_collector_free(ec); return 20;
    }
    for (i = 0; i < (int)sizeof out; i++) printf("%02x", (unsigned char)out[i]);
    printf("\n");
    jent_entropy_collector_free(ec);
    return 0;
}
