/* Time functions Mbed TLS needs on AROS, for every build. */
#include <mbedtls/platform_time.h>
#include <mbedtls/platform_util.h>
#include <sys/time.h>
#include <time.h>

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (mbedtls_ms_time_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* The ABIv11 C library has gmtime() but not gmtime_r(). apkg makes one TLS
 * connection at a time, from one task, so copying the static result out is
 * safe here. */
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *out)
{
    time_t t = (time_t)*tt;
    struct tm *g = gmtime(&t);
    if (!g) return NULL;
    *out = *g;
    return out;
}
