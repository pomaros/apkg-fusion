#ifndef PKG_SLOW_H
#define PKG_SLOW_H
/* Conversion shared by the two --slow test paths. */
static inline unsigned long pkg_slow_ticks(int ms)
{
    /* Round up to a 1/50 s tick without overflowing ms + 19. */
    return ms > 0 ? (unsigned long)(ms / 20 + (ms % 20 != 0)) : 0;
}
#endif
