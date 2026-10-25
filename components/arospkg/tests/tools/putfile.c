/* putfile -- send a file from the guest to the host, for test evidence only.
 *
 *   putfile <file> [name] [port]      PUT to 10.0.2.2:<port>/<name>
 *
 * Not part of pkg, and deliberately so. pkg refuses plain HTTP, and that
 * refusal is one of its tested properties; a transport for test reports does
 * not belong in the binary whose downloads it would be weakening.
 *
 * It exists because every other way out of the guest has failed: the shared
 * FAT directory turns guest writes into garbage of the right size, and the
 * serial port delivers nothing. Under QEMU's user networking 10.0.2.2 is the
 * host's loopback, where tests/tools/recv.py listens. The receiver answers with
 * the size and SHA-256 it got; this prints that line, so a report arrives as a
 * file on the host with a hash both ends agree on rather than as a screenshot.
 */
#include <proto/exec.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Library *SocketBase;

int main(int argc, char **argv)
{
    const char *file, *name;
    int port, s = -1, rc = 20;
    FILE *f;
    long size;
    char hdr[512], buf[8192], *slash;
    struct sockaddr_in sa;
    size_t n;

    if (argc < 2) { printf("usage: putfile <file> [name] [port]\n"); return 5; }
    file = argv[1];
    slash = strrchr(file, '/');
    name = argc > 2 ? argv[2] : (slash ? slash + 1 : (strrchr(file, ':') ? strrchr(file, ':') + 1 : file));
    port = argc > 3 ? atoi(argv[3]) : 8765;

    f = fopen(file, "rb");
    if (!f) { printf("putfile: cannot open %s\n", file); return 10; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) { printf("putfile: no TCP/IP stack\n"); fclose(f); return 10; }

    s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl((10u << 24) | (0u << 16) | (2u << 8) | 2u);
    if (s < 0 || connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        printf("putfile: cannot connect to 10.0.2.2:%d\n", port);
        goto out;
    }
    n = (size_t)snprintf(hdr, sizeof hdr,
        "PUT /%s HTTP/1.0\r\nHost: 10.0.2.2\r\nContent-Length: %ld\r\n\r\n", name, size);
    if (send(s, hdr, n, 0) != (long)n) { printf("putfile: send failed\n"); goto out; }
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        size_t off = 0;
        while (off < n) {
            long w = send(s, buf + off, n - off, 0);
            if (w <= 0) { printf("putfile: send failed\n"); goto out; }
            off += (size_t)w;
        }
    }
    {   /* Print only the receiver's own line: "OK <name> <size> <sha256>". */
        long got, total = 0;
        char resp[1024];
        while ((got = recv(s, resp + total, sizeof resp - 1 - total, 0)) > 0) total += got;
        resp[total] = 0;
        {
            char *body = strstr(resp, "\r\n\r\n");
            printf("%s", body ? body + 4 : resp);
            rc = (body && strncmp(body + 4, "OK ", 3) == 0) ? 0 : 10;
        }
    }
out:
    if (s >= 0) CloseSocket(s);
    CloseLibrary(SocketBase);
    fclose(f);
    return rc;
}
