/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Host transport: curl owns TLS; the context operation owns byte bounds,
 * cancellation, output and subprocess lifetime. No shell or global hooks. */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "pkg_fs.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
int ak_http_url(const char *url) {
    size_t n = strlen(url);
    const char *host;
    if (!n || n >= AK_PATH || !ak_text_path_valid(url, n))
        return 0;
    if (!strncmp(url, "https://", 8))
        host = url + 8;
    else if (!strncmp(url, "http://", 7))
        host = url + 7;
    else
        return 0;
    const char *end = strchr(host, '/');
    size_t length = end ? (size_t)(end - host) : strlen(host);
    if (!length || memchr(host, '@', length) || strchr(url, '?') ||
        strchr(url, '#') || strchr(url, '\\'))
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)url[i] <= 32 || (unsigned char)url[i] > 126)
            return 0;
    return 1;
}
/* Only fixed protocol members or a validated digest are supplied as suffixes.
 */
apkg_status ak_http_path(const char *base, const char *member,
                         char out[AK_PATH]) {
    size_t n = strlen(base);
    while (n && base[n - 1] == '/')
        n--;
    if (!ak_http_url(base))
        return APKG_INVALID;
    int count = snprintf(out, AK_PATH, "%.*s/%s", (int)n, base, member);
    return count < 0 || count >= AK_PATH ? APKG_LIMIT : APKG_OK;
}
apkg_status ak_http_get(const char *url, uint64_t maximum, int output,
                        unsigned char **bytes, size_t *size,
                        ak_import_progress *progress) {
#if defined(__APPLE__) || defined(__linux__)
    int body[2] = {-1, -1}, errors[2] = {-1, -1}, status = 0, live = 0;
    pid_t child = 0;
    posix_spawn_file_actions_t actions;
    int actions_ready = 0, body_open = 1, error_open = 1;
    apkg_status code = APKG_OK;
    ak_buf memory = {0};
    char diagnostics[8192];
    size_t diagnostic_size = 0;
    uint64_t total = 0;
    long long started = pkg_fs_now_ms();
    if (bytes)
        *bytes = NULL;
    if (size)
        *size = 0;
    if (!ak_http_url(url) || (!bytes && output < 0) || maximum > 2147483647u)
        return APKG_INVALID;
    if (progress) {
        progress->subject = url;
        code = ak_import_tick(progress, 0);
        if (code)
            return code;
    }
    if (pipe(body) || pipe(errors)) {
        code = APKG_IO;
        goto done;
    }
    for (size_t i = 0; i < 2; i++) {
        if (body[i] < 3) {
            int fd = fcntl(body[i], F_DUPFD, 3);
            if (fd < 0) {
                code = APKG_IO;
                goto done;
            }
            close(body[i]);
            body[i] = fd;
        }
        if (errors[i] < 3) {
            int fd = fcntl(errors[i], F_DUPFD, 3);
            if (fd < 0) {
                code = APKG_IO;
                goto done;
            }
            close(errors[i]);
            errors[i] = fd;
        }
    }
    for (size_t i = 0; i < 2; i++) {
        if (fcntl(body[i], F_SETFD, FD_CLOEXEC) ||
            fcntl(errors[i], F_SETFD, FD_CLOEXEC)) {
            code = APKG_IO;
            goto done;
        }
    }
    if (fcntl(body[0], F_SETFL, O_NONBLOCK) ||
        fcntl(errors[0], F_SETFL, O_NONBLOCK)) {
        code = APKG_IO;
        goto done;
    }
    if (posix_spawn_file_actions_init(&actions)) {
        code = APKG_IO;
        goto done;
    }
    actions_ready = 1;
    if (posix_spawn_file_actions_adddup2(&actions, body[1], STDOUT_FILENO) ||
        posix_spawn_file_actions_adddup2(&actions, errors[1], STDERR_FILENO) ||
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                         O_RDONLY, 0)) {
        code = APKG_IO;
        goto done;
    }
    char limit[24];
    snprintf(limit, sizeof limit, "%llu", (unsigned long long)maximum);
    const char *protocols =
        !strncmp(url, "https://", 8) ? "=https" : "=http,https";
    char *argv[] = {"curl",
                    "-q",
                    "-sS",
                    "--fail",
                    "--location",
                    "--max-redirs",
                    "5",
                    "--connect-timeout",
                    "10",
                    "--max-time",
                    "120",
                    "--proto",
                    (char *)protocols,
                    "--proto-redir",
                    (char *)protocols,
                    "--max-filesize",
                    limit,
                    "--write-out",
                    "%{stderr}\nAPKG_HTTP=%{http_code}\n",
                    "--url",
                    (char *)url,
                    NULL};
    int spawned = posix_spawnp(&child, "curl", &actions, NULL, argv, environ);
    if (spawned) {
        code = spawned == ENOENT ? APKG_UNSUPPORTED : APKG_NETWORK;
        goto done;
    }
    live = 1;
    close(body[1]);
    body[1] = -1;
    close(errors[1]);
    errors[1] = -1;
    memory.max = (size_t)maximum;
    while (body_open || error_open) {
        if (pkg_fs_now_ms() - started > 125000) {
            code = APKG_NETWORK;
            goto done;
        }
        if (progress && ak_cancelled(progress->observer)) {
            code = APKG_CANCELLED;
            goto done;
        }
        struct pollfd fds[2] = {{body_open ? body[0] : -1, POLLIN, 0},
                                {error_open ? errors[0] : -1, POLLIN, 0}};
        int ready = poll(fds, 2, 50);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            code = APKG_NETWORK;
            goto done;
        }
        for (size_t stream = 0; stream < 2; stream++) {
            if (!(fds[stream].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            unsigned char buffer[65536];
            ssize_t n = read(fds[stream].fd, buffer, sizeof buffer);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                code = APKG_NETWORK;
                goto done;
            }
            if (!n) {
                if (stream)
                    error_open = 0;
                else
                    body_open = 0;
                continue;
            }
            if (stream) {
                size_t take = (size_t)n;
                if (take > sizeof diagnostics)
                    take = sizeof diagnostics;
                if (diagnostic_size + take > sizeof diagnostics) {
                    size_t discard =
                        diagnostic_size + take - sizeof diagnostics;
                    memmove(diagnostics, diagnostics + discard,
                            diagnostic_size - discard);
                    diagnostic_size -= discard;
                }
                memcpy(diagnostics + diagnostic_size, buffer + (size_t)n - take,
                       take);
                diagnostic_size += take;
                continue;
            }
            if ((uint64_t)n > maximum - total) {
                code = APKG_LIMIT;
                goto done;
            }
            total += (uint64_t)n;
            if (output >= 0) {
                size_t offset = 0;
                while (offset < (size_t)n) {
                    ssize_t written =
                        write(output, buffer + offset, (size_t)n - offset);
                    if (written < 0 && errno == EINTR)
                        continue;
                    if (written <= 0) {
                        code = APKG_IO;
                        goto done;
                    }
                    offset += (size_t)written;
                }
            } else {
                /* Binary metadata is copied without printf/string conversion.
                 */
                size_t required = memory.size + (size_t)n + 1;
                if (required > memory.cap) {
                    size_t capacity = memory.cap ? memory.cap * 2 : 4096;
                    if (capacity < required)
                        capacity = required;
                    if (capacity > memory.max + 1)
                        capacity = memory.max + 1;
                    char *next = malloc(capacity);
                    if (!next) {
                        code = APKG_NOMEM;
                        goto done;
                    }
                    if (memory.size)
                        memcpy(next, memory.data, memory.size);
                    free(memory.data);
                    memory.data = next;
                    memory.cap = capacity;
                }
                memcpy(memory.data + memory.size, buffer, (size_t)n);
                memory.size += (size_t)n;
                memory.data[memory.size] = 0;
            }
            if (progress) {
                code = ak_import_tick(progress, (uint64_t)n);
                if (code)
                    goto done;
            }
        }
    }
    for (;;) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            break;
        if (waited < 0) {
            if (errno == EINTR)
                continue;
            code = APKG_NETWORK;
            goto done;
        }
        if (progress && ak_cancelled(progress->observer)) {
            code = APKG_CANCELLED;
            goto done;
        }
        if (pkg_fs_now_ms() - started > 125000) {
            code = APKG_NETWORK;
            goto done;
        }
        poll(NULL, 0, 50);
    }
    live = 0;
    int http = 0;
    const char marker[] = "\nAPKG_HTTP=";
    size_t length = sizeof marker - 1;
    if (diagnostic_size >= length + 4) {
        size_t at = diagnostic_size - length - 4;
        if (!memcmp(diagnostics + at, marker, length) &&
            diagnostics[diagnostic_size - 1] == '\n') {
            const char *p = diagnostics + at + length;
            if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' &&
                p[2] >= '0' && p[2] <= '9')
                http = (p[0] - '0') * 100 + (p[1] - '0') * 10 + p[2] - '0';
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) || http != 200) {
        code = http == 404                                      ? APKG_NOT_FOUND
               : WIFEXITED(status) && WEXITSTATUS(status) == 63 ? APKG_LIMIT
                                                                : APKG_NETWORK;
        goto done;
    }
    if (bytes) {
        if (!memory.data) {
            memory.data = ak_copy("", 0);
            if (!memory.data) {
                code = APKG_NOMEM;
                goto done;
            }
        }
        *bytes = (unsigned char *)memory.data;
        memory.data = NULL;
    }
    if (size)
        *size = (size_t)total;
done:
    if (live) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }
    if (actions_ready)
        posix_spawn_file_actions_destroy(&actions);
    for (size_t i = 0; i < 2; i++) {
        if (body[i] >= 0)
            close(body[i]);
        if (errors[i] >= 0)
            close(errors[i]);
    }
    free(memory.data);
    return code;
#else
    (void)url;
    (void)maximum;
    (void)output;
    (void)progress;
    if (bytes)
        *bytes = NULL;
    if (size)
        *size = 0;
    return APKG_UNSUPPORTED;
#endif
}
