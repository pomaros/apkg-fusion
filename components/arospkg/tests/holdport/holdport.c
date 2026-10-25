/* holdport -- a stand-in for a PkgManager that is slow to answer. It opens
 * a public port (default PKGMANAGER), takes each message sent to it and
 * replies only after HOLD seconds, so a test can show that a client keeps
 * working while its request is unanswered. Ends after RUN seconds or on
 * Ctrl-C, replying to whatever it still holds.
 *
 *   holdport [HOLD [RUN [PORT]]]        defaults 20 60 PKGMANAGER
 *
 * Testing only; not part of any release archive. Messages are replied with
 * rm_Result1 = 0 and no result string when they look like RexxMsgs.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <rexx/storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXHELD 8

int main(int argc, char **argv)
{
    long hold = argc > 1 ? atol(argv[1]) : 20, run = argc > 2 ? atol(argv[2]) : 60;
    const char *name = argc > 3 ? argv[3] : "PKGMANAGER";
    struct MsgPort *port = CreateMsgPort();
    struct { struct Message *m; time_t at; } held[MAXHELD];
    int n = 0, i;
    time_t start = time(NULL);
    if (!port) return 20;
    port->mp_Node.ln_Name = (char *)name;
    port->mp_Node.ln_Pri = 0;
    AddPort(port);
    printf("holdport: %s open, hold %ld s, run %ld s\n", name, hold, run); fflush(stdout);
    for (;;) {
        struct Message *m;
        time_t now = time(NULL);
        while ((m = GetMsg(port))) {
            if (n < MAXHELD) { held[n].m = m; held[n].at = now; n++; }
            else ReplyMsg(m);
            printf("holdport: got a message at +%lds\n", (long)(now - start)); fflush(stdout);
        }
        for (i = 0; i < n; ) {
            if (now - held[i].at >= hold) {
                struct RexxMsg *rm = (struct RexxMsg *)held[i].m;
                rm->rm_Result1 = 0; rm->rm_Result2 = 0;
                ReplyMsg(held[i].m);
                printf("holdport: replied at +%lds\n", (long)(now - start)); fflush(stdout);
                held[i] = held[--n];
            } else i++;
        }
        if (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) break;
        if (now - start >= run) break;
        Delay(10);
    }
    RemPort(port);
    while (n > 0) ReplyMsg(held[--n].m);
    {   struct Message *m; while ((m = GetMsg(port))) ReplyMsg(m); }
    DeleteMsgPort(port);
    printf("holdport: closed\n");
    return 0;
}
