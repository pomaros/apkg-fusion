/* Which stdio calls lose their output when stdout is redirected to a file,
 * on a given AROS build. Found while tracing an empty `apkg search >file`
 * on mainline (pool v1-1, 2026-10-11) while `apkg show >file` was written.
 *   stdoutprobe <mode>   modes: fputs printf fwrite puts putchar fputs-big
 *                        printf-then-fputs fputs-then-printf fputs-flush
 * Each run writes one line (or 700 bytes for fputs-big) and exits. */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *m = argc > 1 ? argv[1] : "";
    static char big[701];
    if (!strcmp(m, "fputs")) fputs("fputs line\n", stdout);
    else if (!strcmp(m, "printf")) printf("printf line\n");
    else if (!strcmp(m, "fwrite")) fwrite("fwrite line\n", 1, 12, stdout);
    else if (!strcmp(m, "puts")) puts("puts line");
    else if (!strcmp(m, "putchar")) { putchar('c'); putchar('\n'); }
    else if (!strcmp(m, "fputs-big")) { memset(big, 'x', 699); big[699] = '\n'; fputs(big, stdout); }
    else if (!strcmp(m, "printf-then-fputs")) { printf("printf first\n"); fputs("fputs second\n", stdout); }
    else if (!strcmp(m, "fputs-then-printf")) { fputs("fputs first\n", stdout); printf("printf second\n"); }
    else if (!strcmp(m, "fputs-flush")) { fputs("fputs flushed\n", stdout); fflush(stdout); }
    else { printf("usage: stdoutprobe <mode>\n"); return 5; }
    return 0;
}
