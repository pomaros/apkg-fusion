/* HTTPS transport, ported from docs/spikes/network-download/dl.c, which is
 * where each of these rules was established and measured on mainline AROS.
 *
 * The spike's two deliberate differences from the older aros-hardcode
 * transport are the reason this file exists at all, and both are kept:
 *   - the CA bundle is mandatory; if it will not load we stop rather than
 *     "continue unchecked". An unverified download is the whole attack.
 *   - the certificate must match the name we asked for. SSL_VERIFY_PEER alone
 *     validates the chain and would accept any valid certificate for any name.
 *
 * The response is read into memory before being written out. That is what the
 * spike did and it is proven; it caps a download at max_bytes and is the
 * reason net_fetch takes one. Streaming to disk is worth doing when packages
 * get large, and is not worth doing before the rest of this works.
 */
#include "net.h"
#include "pkg.h"
#include "util.h"
#include "slow.h"
#include "sha256.h"

#include <errno.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include "tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAFILE   "ENV:SYS/Certificates/ca-bundle.crt"
#define MAXREDIR 5

/* libnet.a defines SocketBase but nothing opens it, and the TLS layer's
 * socket calls go through bsdsocket, so the base has to be ours and live
 * before any TLS work happens. */
struct Library *SocketBase;

const char *net_cafile(void) { return CAFILE; }

static net_progress prog_cb;
static void        *prog_user;
void net_set_progress(net_progress cb, void *user) { prog_cb = cb; prog_user = user; }
static int slow_ms;
void net_set_slow(int ms) { slow_ms = ms; }
static long break_at = -1;
static int  break_how;
void net_set_break(long b, int how) { break_at = b; break_how = how; }
static char body_sha[65];
const char *net_body_sha256(void) { return body_sha; }

typedef struct { char *p; size_t len, cap; } str;

/* What has arrived of a response: the body's length once the headers are
   complete (-1 before), and the declared Content-Length (-1 if none). Both
   count body bytes only, never the headers. */
static void resp_lengths(const str *raw, long *body, long *declared)
{
    char *he = raw->p ? strstr(raw->p, "\r\n\r\n") : NULL, *cl;
    *body = *declared = -1;
    if (!he) return;
    *body = (long)(raw->len - (size_t)(he + 4 - raw->p));
    cl = strstr(raw->p, "\nContent-Length:");
    if (!cl) cl = strstr(raw->p, "\ncontent-length:");
    if (cl && cl < he) *declared = atol(cl + 16);
}

/* A stream that ended with an error: say so, with what had arrived. A
   partial response is never passed on as if it were complete. */
static void broken(char why[240], const char *host, const str *raw, const char *reason)
{
    long body, declared;
    resp_lengths(raw, &body, &declared);
    if (body < 0)
        snprintf(why, 240, "the connection to %s broke before the response headers arrived: %s",
                 host, reason);
    else if (declared >= 0)
        snprintf(why, 240, "the connection to %s broke after %ld of %ld bytes: %s",
                 host, body, declared, reason);
    else
        snprintf(why, 240, "the connection to %s broke after %ld bytes of the response: %s",
                 host, body, reason);
}

static int str_add(str *s, const char *d, size_t n, long cap_max)
{
    if (s->len + n + 1 > s->cap) {
        size_t c = s->cap ? s->cap : 16384;
        while (c < s->len + n + 1) c *= 2;
        if (cap_max > 0 && (long)c > cap_max) return 0;
        { char *q = (char *)realloc(s->p, c); if (!q) return 0; s->p = q; s->cap = c; }
    }
    memcpy(s->p + s->len, d, n);
    s->len += n;
    s->p[s->len] = 0;
    return 1;
}

/* https://host/path only. Plain HTTP is refused on purpose. */
static int url_split(const char *url, char *host, size_t hn, char *path, size_t pn)
{
    const char *p;
    size_t n;
    if (strncmp(url, "https://", 8) != 0) return 0;
    p = url + 8;
    n = strcspn(p, "/");
    if (n == 0 || n >= hn) return 0;
    memcpy(host, p, n); host[n] = 0;
    if (p[n] == 0) { snprintf(path, pn, "/"); return 1; }
    if (strlen(p + n) >= pn) return 0;
    snprintf(path, pn, "%s", p + n);
    return 1;
}

static int tcp_connect(const char *host, char why[240])
{
    struct hostent *he = gethostbyname((char *)host);
    struct sockaddr_in sa;
    int s;
    if (!he || !he->h_addr_list[0]) {
        snprintf(why, 240, "cannot resolve %s", host);
        return -1;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { snprintf(why, 240, "cannot create a socket"); return -1; }
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(443);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        snprintf(why, 240, "cannot connect to %s:443", host);
        CloseSocket(s);
        return -1;
    }
    return s;
}

static size_t dechunk(char *b, size_t len)
{
    size_t in = 0, out = 0;
    while (in < len) {
        char *e;
        unsigned long sz = strtoul(b + in, &e, 16);
        if (e == b + in) break;
        while (in < len && b[in] != '\n') in++;
        in++;
        if (sz == 0) break;
        if (in + sz > len) sz = len - in;
        memmove(b + out, b + in, sz);
        out += sz; in += sz;
        while (in < len && (b[in] == '\r' || b[in] == '\n')) in++;
    }
    return out;
}

/* A Ctrl-C that arrived while connecting: the name lookup, connect and the
   TLS handshake fail with the stack's EINTR (or the break is still
   pending), and without this check they read as a network failure ("cannot
   resolve", "handshake failed -0x6c00", seen in Limpet and CON: on AROS One,
   2026-10-12). Only with a progress callback, which is what makes a
   download cancellable at all. */
static int break_seen(void)
{
    if (!prog_cb) return 0;
    if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) return 1;
    return SocketBase && Errno() == EINTR;
}

/* Returns 1 = body, 2 = redirect in `loc`, 0 = failure. */
static int fetch_once(const char *host, const char *path, str *body,
                      char *loc, size_t locn, long max_bytes, char why[240])
{
    tls *t = NULL;
    int s = -1, rc = 0, n, code, chunked;
    str raw = {0};
    char req[4096], *hdr_end, *stline, *b;
    size_t blen;

    if ((s = tcp_connect(host, why)) < 0) {
        if (break_seen()) snprintf(why, 240, "cancelled");
        goto out;
    }
    {   /* Normally the host we asked for. A test can substitute another name,
           and then the handshake must fail -- that is the only way to show the
           check is doing anything. */
        const char *want = pkg_verify_name();
        if (!want || !*want) want = host;
        if (want != host)
            printf("  (test) requiring the certificate to match %s\n", want);
        if (!(t = tls_start(s, host, want, CAFILE, why))) {
            if (break_seen()) snprintf(why, 240, "cancelled");
            goto out;
        }
    }

    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: arospkg/0.2\r\n"
             "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
    if (tls_write(t, req, strlen(req)) <= 0) {
        if (break_seen()) snprintf(why, 240, "cancelled");
        else snprintf(why, 240, "cannot send the request to %s", host);
        goto out;
    }

    for (;;) {
        char chunk[16384];
        n = tls_read(t, chunk, sizeof chunk);
        if (n < 0) {
            /* A Ctrl-C that the TCP stack turned into EINTR is the user's
               cancel, the same as one the progress callback saw. Only when
               there is a callback: without one nobody offered cancelling. */
            if (prog_cb && tls_interrupted(t)) { snprintf(why, 240, "cancelled"); goto out; }
            broken(why, host, &raw, tls_error(t)); goto out;
        }
        if (n == 0) break;
        if (!str_add(&raw, chunk, (size_t)n, max_bytes)) {
            snprintf(why, 240, "response from %s exceeds %ld bytes", host, max_bytes);
            goto out;
        }
        if (slow_ms > 0) Delay(pkg_slow_ticks(slow_ms));
        if (prog_cb) {
            /* The headers arrive in the same stream; once they are complete
               the body count and the declared length are both known, and
               that is what a front end wants to draw. Before that, nothing
               is reported rather than a number that would go backwards. */
            int cancel = 0;
            char *he = strstr(raw.p, "\r\n\r\n");
            /* A redirect's body is not the download: reporting its few
               hundred bytes as a completed transfer made the bar jump to
               full and back. Only a 2xx response is worth drawing. */
            if (he && raw.len > 12 && raw.p[9] == '2') {
                unsigned long total = 0, done = raw.len - (size_t)(he + 4 - raw.p);
                char *cl = strstr(raw.p, "\nContent-Length:");
                if (!cl) cl = strstr(raw.p, "\ncontent-length:");
                if (cl && cl < he) total = (unsigned long)atol(cl + 16);
                prog_cb(prog_user, done, total, &cancel);
                if (cancel) { snprintf(why, 240, "cancelled"); goto out; }
            }
        }
        /* Testing only. Decided here, where the body is counted, and done in
           the TLS adapter, so the failure comes back through the same path
           as a real one. Only a 2xx answer counts: a redirect never uses it. */
        if (break_at >= 0 && raw.len > 12 && raw.p[9] == '2') {
            long body, declared;
            resp_lengths(&raw, &body, &declared);
            if (body >= break_at) { break_at = -1; tls_test_fail_next_recv(t, break_how); }
        }
    }
    if (!raw.len) { snprintf(why, 240, "empty response from %s", host); goto out; }

    hdr_end = strstr(raw.p, "\r\n\r\n");
    if (!hdr_end) { snprintf(why, 240, "malformed response from %s", host); goto out; }
    *hdr_end = 0;

    stline = strchr(raw.p, ' ');
    if (!stline) { snprintf(why, 240, "no status line from %s", host); goto out; }
    code = atoi(stline + 1);

    if (code >= 300 && code < 400) {
        char *l = strstr(raw.p, "\nLocation:");
        size_t ln;
        if (!l) l = strstr(raw.p, "\nlocation:");
        if (!l) { snprintf(why, 240, "HTTP %d without a Location header", code); goto out; }
        l += 10;
        while (*l == ' ') l++;
        ln = strcspn(l, "\r\n");
        if (ln >= locn) { snprintf(why, 240, "redirect target too long"); goto out; }
        memcpy(loc, l, ln); loc[ln] = 0;
        rc = 2;
        goto out;
    }
    if (code < 200 || code > 299) {
        /* An HTTP error is a transport failure and must be reported as one.
           Letting a 404's error page reach the index parser would turn
           "the server does not have this" into "the index is corrupt", which
           sends the reader looking in the wrong place entirely. */
        snprintf(why, 240, "HTTP %d from %s%s", code, host,
                 code == 404 ? " -- the server does not have that file" : "");
        goto out;
    }

    chunked = (strstr(raw.p, "\nTransfer-Encoding: chunked") ||
               strstr(raw.p, "\ntransfer-encoding: chunked")) ? 1 : 0;
    b = hdr_end + 4;
    blen = raw.len - (size_t)(b - raw.p);
    if (chunked) blen = dechunk(b, blen);
    else {
        /* A connection that ends early is a broken download, and is said
           to be one: left to the checksum it read as a damaged file. */
        char *cl = strstr(raw.p, "\nContent-Length:");
        if (!cl) cl = strstr(raw.p, "\ncontent-length:");
        if (cl && (long)blen < atol(cl + 16)) {
            snprintf(why, 240, "the connection to %s ended after %lu of %ld bytes", host,
                     (unsigned long)blen, atol(cl + 16));
            goto out;
        }
    }
    if (!str_add(body, b, blen, max_bytes)) { snprintf(why, 240, "body too large"); goto out; }
    rc = 1;

out:
    tls_end(t);
    if (s >= 0) CloseSocket(s);
    free(raw.p);
    return rc;
}

int net_open(char why[240])
{
    if (SocketBase) return 0;
    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) {
        snprintf(why, 240,
                 "cannot open bsdsocket.library -- is the network stack running? "
                 "Try: Execute SYS:System/Network/AROSTCP/S/startnet");
        return -1;
    }
    return 0;
}

void net_close(void)
{
    if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
}

int net_fetch(const char *url, const char *dest, long max_bytes, char why[240])
{
    /* GitHub's release downloads redirect to signed URLs of about 950
       bytes, growing with the file name; 2048 leaves room. */
    char host[256], path[2048], cur[2048], loc[2048], part[PKG_UTIL_PATH];
    str body = {0};
    int hop, rc = -1;
    FILE *f;

    why[0] = 0;
    snprintf(cur, sizeof cur, "%s", url);

    for (hop = 0; hop <= MAXREDIR; hop++) {
        int r;
        if (!url_split(cur, host, sizeof host, path, sizeof path)) {
            snprintf(why, 240, "not an https URL: %s", cur);
            goto out;
        }
        /* Each hop is verified against ITS OWN host: the archives redirect
           across hosts (archives.arosworld.org -> arosarchives.os4depot.net),
           so carrying the original name forward would either fail or, worse,
           accept a certificate for the wrong server. */
        r = fetch_once(host, path, &body, loc, sizeof loc, max_bytes, why);
        if (r == 1) break;
        if (r != 2) goto out;
        /* The query of a signed download URL is a credential for that
           download: it is never printed, not even with --verbose. */
        if (pkg_verbose()) printf("  redirected to %.*s\n", (int)strcspn(loc, "?"), loc);
        snprintf(cur, sizeof cur, "%s", loc);
        body.len = 0;
        if (hop == MAXREDIR) { snprintf(why, 240, "too many redirects"); goto out; }
    }

    /* Only for --keep-rejected: the body as it is in memory, before the
       write. If it is right and the file is wrong, the network is cleared. */
    body_sha[0] = 0;
    if (pkg_keep_rejected()) {
        sha256_ctx h;
        sha256_init(&h);
        sha256_update(&h, body.p, body.len);
        sha256_final(&h, body_sha);
    }
    snprintf(part, sizeof part, "%s.part", dest);
    u_delete(part);
    f = fopen(part, "wb");
    if (!f) { snprintf(why, 240, "cannot write %s", part); goto out; }
    if (fwrite(body.p, 1, body.len, f) != body.len || fclose(f) != 0) {
        snprintf(why, 240, "cannot write %s", part);
        u_delete(part);
        goto out;
    }
    /* Left as .part on purpose: the caller verifies size and hash and only
       then publishes. An interrupted download must never be mistaken for a
       complete one. */
    rc = 0;
out:
    free(body.p);
    return rc;
}
