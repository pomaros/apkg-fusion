/* Only the DOS boundary is substituted: stdout is the real renderer output. */
#include "../src/pkg/present.h"
#include "../third_party/aros-xterm/aros_tty_client.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include "../src/libpkg/slow.h"
static int width = 80, tty = 1, known = 15, query_ok = 1, interp = 0, styles = 1;
static time_t tick = 100;
time_t time(time_t *out) { if (out) *out = tick; return tick; }
BPTR Output(void) { return 1; }
LONG IsInteractive(BPTR h) { (void)h; return tty; }
LONG aros_tty_control(BPTR h, XtyControl *q)
{
    (void)h;
    if (!query_ok) return DOSFALSE;
    q->values[0] = known; q->values[1] = 1; q->values[2] = styles; q->values[6] = interp;
    q->values[3] = 3; q->values[4] = 24; q->values[5] = width;
    return DOSTRUE;
}
int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "progress";
    if (argc > 2) width = atoi(argv[2]);
    if (!strcmp(mode, "slow")) { printf("%lu\n", pkg_slow_ticks(width)); return 0; }
    if (!strcmp(mode, "con")) query_ok = 0;
    if (!strcmp(mode, "unknown")) known = 0;
    /* colors KNOWN INTERP STYLES HOW: the three verdict colours as the window
       would get them, for a capability reply the test describes; HOW is
       auto, never, always, plain, json or redirect. Level is ANSI16. */
    if (!strcmp(mode, "colors")) {
        const char *how = argc > 5 ? argv[5] : "auto";
        known = atoi(argv[2]); interp = atoi(argv[3]); styles = atoi(argv[4]);
        if (!strcmp(how, "redirect")) tty = 0;
        pr_init(!strcmp(how, "json"), !strcmp(how, "plain"),
                !strcmp(how, "never") ? PR_COLOR_NEVER : !strcmp(how, "always") ? PR_COLOR_ALWAYS : PR_COLOR_AUTO);
        pr_word(PR_GREEN, "native"); putchar(' ');
        pr_word(PR_YELLOW, "undetermined"); putchar(' ');
        pr_word(PR_RED, "missing"); putchar('\n');
        return 0;
    }
    pr_init(0, 0, PR_COLOR_AUTO);
    if (!strcmp(mode, "plain")) pr_init(0, 1, PR_COLOR_AUTO);
    if (!strcmp(mode, "redirect")) { tty = 0; pr_init(0, 0, PR_COLOR_AUTO); }
    if (!strcmp(mode, "json")) pr_init(1, 0, PR_COLOR_AUTO);
    if (!strcmp(mode, "layout")) {
        pr_heading("Micropolis 0.1.0-rc3");
        pr_text("A city simulation with scenarios and saved cities.", 2);
        pr_field("Location", "Work:Applications/Micropolis", -1);
        pr_search_heading();
        pr_search_row("micropolis", "0.1.0-rc3-aros2", "installed",
                      "A city simulation with scenarios and saved cities.", "");
        pr_search_row("a-very-long-package-identifier", "12345678901234567890", "available",
                      "An application with a long name.", "other CPU: i386/v0");
        return 0;
    }
    if (!strcmp(mode, "overflow")) {
        pr_progress("p", "download", ~0UL / 2, ~0UL);
        pr_progress("p", "download", 2, 1);
        pr_end_line();
        return 0;
    }
    if (!strcmp(mode, "unknown-length")) {
        pr_progress("p", "download", 580, 0);
        puts("verified and cached test.zip");
        pr_progress("p", "verify", 1, 1);
        pr_end_line();
        return 0;
    }
    if (!strcmp(mode, "resize")) {
        pr_progress("long-package-name", "download", 100, 1000);
        width = 24;
        tick++;
    }
    if (!strncmp(mode, "resize-cycle", 12)) {
        width = 80;
        pr_progress("p", "download", 100, 1000);
        puts("<narrow>");
        width = 12; /* Same clock tick: geometry must not be time-cached. */
        pr_progress("p", "download", 200, 1000);
        pr_progress("p", "download", 210, 1000);
        pr_progress("p", "download", 300, 1000);
        puts("<wide>");
        width = 100;
        pr_progress("p", "download", 400, 1000);
        if (!strcmp(mode, "resize-cycle-success"))
            pr_progress("p", "download", 1000, 1000);
        pr_end_line();
        puts(!strcmp(mode, "resize-cycle-cancel") ? "cancelled" :
             !strcmp(mode, "resize-cycle-error") ? "connection reset" : "installed");
        return 0;
    }
    pr_progress("long-package-name", "download", 580, 1000);
    pr_progress("long-package-name", "verify", 1, 1);
    pr_progress("long-package-name", "extract", 0, 1000);
    pr_progress("long-package-name", "extract", 1, 1);
    pr_end_line();
    return 0;
}
