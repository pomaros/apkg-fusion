/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
/* apkg fetch: download one URL to a file, as `curl -fLo` would.
 *
 * No root, channel or trust policy is involved: this is the plain transport
 * the engine already uses for HTTP channels, curl on a host and the built-in
 * client with its own TLS on AROS. The body goes to a temporary file beside
 * the output and takes the output's name only when it is complete, and, when
 * a SHA-256 was given, only when it matches. An interrupted or refused fetch
 * leaves any earlier file of that name as it was. */
#include "internal.h"
#include "pkg_fs.h"
#include "pkg_sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The last path segment of the URL, without its query. */
static apkg_status default_name(const char *url, char *out, size_t size) {
    const char *host = strstr(url, "://"), *path, *end, *start;
    size_t n;
    if (!host)
        return APKG_INVALID;
    host += 3;
    path = host + strcspn(host, "/?");
    end = path + strcspn(path, "?");
    start = end;
    while (start > path && start[-1] != '/')
        start--;
    n = (size_t)(end - start);
    if (!n || n >= size || (n == 1 && *start == '.') ||
        (n == 2 && start[0] == '.' && start[1] == '.'))
        return APKG_INVALID;
    memcpy(out, start, n);
    out[n] = 0;
    for (size_t i = 0; i < n; i++)
        if (out[i] == ':' || out[i] == '%')
            return APKG_INVALID; /* an AROS volume or an encoded name */
    return APKG_OK;
}
/* "<dir>/.apkg-fetch" and eight hex digits, short enough for FFS. */
static int temporary(const char *output, char *out, size_t size) {
    const char *slash = strrchr(output, '/'), *colon = strrchr(output, ':');
    size_t dir;
    unsigned char r[4];
    int fd = -1;
    if (colon && (!slash || colon > slash))
        slash = colon;
    dir = slash ? (size_t)(slash - output) + 1 : 0;
    for (int tries = 0; tries < 8 && fd < 0; tries++) {
        if (pkg_fs_random(r, sizeof r))
            return -1;
        if (snprintf(out, size, "%.*s.apkg-fetch%02x%02x%02x%02x", (int)dir,
                     output, r[0], r[1], r[2], r[3]) >= (int)size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        fd = open(out, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0 && errno != EEXIST)
            return -1;
    }
    return fd;
}
static apkg_status file_digest(const char *path, apkg_digest *digest,
                               uint64_t *size) {
    struct pkg_sha256 h;
    unsigned char *buffer = malloc(65536);
    int fd = open(path, O_RDONLY);
    apkg_status code = APKG_OK;
    uint64_t total = 0;
    if (!buffer || fd < 0) {
        free(buffer);
        if (fd >= 0)
            close(fd);
        return buffer ? APKG_IO : APKG_NOMEM;
    }
    pkg_sha256_init(&h);
    for (;;) {
        ssize_t got = read(fd, buffer, 65536);
        if (got < 0) {
            code = APKG_IO;
            break;
        }
        if (!got)
            break;
        pkg_sha256_update(&h, buffer, (size_t)got);
        total += (uint64_t)got;
    }
    pkg_sha256_final(&h, digest->bytes);
    close(fd);
    free(buffer);
    *size = total;
    return code;
}
apkg_status apkg_client_fetch(const char *url, const char *output,
                              const char *expected_sha256, uint64_t maximum,
                              apkg_digest *digest, uint64_t *size,
                              apkg_error **e) {
    char name[256], tmp[AK_PATH + 32];
    unsigned char expected[32];
    apkg_digest got;
    uint64_t bytes = 0;
    apkg_status code;
    int fd;
    if (e)
        *e = NULL;
    if (!url || !ak_http_url_open(url))
        return ak_fail(e, APKG_INVALID,
                       "not an http:// or https:// URL apkg fetch accepts "
                       "(no spaces, credentials or #fragment; percent-encode "
                       "other characters)",
                       url ? url : "");
    if (expected_sha256 &&
        (strlen(expected_sha256) != 64 || ak_hex(expected_sha256, expected, 32)))
        return ak_fail(e, APKG_INVALID,
                       "--sha256 takes 64 lowercase hexadecimal digits",
                       expected_sha256);
    if (!maximum || maximum > 2147483647u)
        maximum = 2147483647u;
    if (!output) {
        if (default_name(url, name, sizeof name))
            return ak_fail(e, APKG_INVALID,
                           "the URL names no file; give the output name",
                           url);
        output = name;
    }
    if (!strcmp(output, "-")) {
        /* Standard output cannot be taken back: a digest mismatch is
         * reported after the bytes were written. */
        if (expected_sha256)
            return ak_fail(e, APKG_INVALID,
                           "--sha256 needs an output file, not standard "
                           "output",
                           url);
        code = ak_http_get_open(url, maximum, STDOUT_FILENO, NULL);
        return code ? ak_fail(e, code, "fetch failed", url) : APKG_OK;
    }
    if (strlen(output) >= AK_PATH || pkg_fs_is_dir(output) ||
        pkg_fs_path_is_link(output) != 0)
        return ak_fail(e, APKG_INVALID,
                       "the output must be a file name, not a drawer or link",
                       output);
    fd = temporary(output, tmp, sizeof tmp);
    if (fd < 0)
        return ak_fail(e, errno == ENAMETOOLONG ? APKG_LIMIT : APKG_IO,
                       "cannot create the temporary file beside the output",
                       output);
    code = ak_http_get_open(url, maximum, fd, NULL);
    if (!code && fsync(fd))
        code = APKG_IO;
    if (close(fd) && !code)
        code = APKG_IO;
    if (!code)
        code = file_digest(tmp, &got, &bytes);
    if (!code && expected_sha256 && memcmp(got.bytes, expected, 32))
        code = APKG_INTEGRITY;
    if (!code && pkg_fs_replace(tmp, output))
        code = APKG_IO;
    if (code) {
        unlink(tmp);
        return ak_fail(e, code,
                       code == APKG_INTEGRITY
                           ? "the downloaded file does not have the expected "
                             "SHA-256; nothing was kept"
                       : code == APKG_NOT_FOUND ? "the server has no such file"
                       : code == APKG_LIMIT
                           ? "the file is larger than the size limit"
                       : code == APKG_UNSUPPORTED
                           ? "no HTTP transport on this system (a host needs "
                             "curl)"
                       : code == APKG_NETWORK ? "the transfer failed or stalled"
                                              : "fetch failed",
                       url);
    }
    if (digest)
        *digest = got;
    if (size)
        *size = bytes;
    return APKG_OK;
}
