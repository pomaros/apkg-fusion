/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * First installation uses the same lock and flush lifecycle as later writes.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "pkg.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failures;
static void check(int condition, const char *name)
{
    checks++;
    printf("  %s %s\n", condition ? "ok" : "FAIL", name);
    if (!condition) failures++;
}
static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}
struct observed {
    const char *cli, *root, *channel;
    int compete, resolution, flush_begin, flush_end, end, end_code;
    int flushed;
    char reason[256];
};

/* exec discards the parent's libpkg operation state. Calling libpkg in a
 * fork-only child would merely exercise its re-entry guard. */
static int competing_install(const struct observed *s)
{
    int descriptors[2], status;
    pid_t child;
    char output[8192];
    size_t used = 0;
    ssize_t got;
    if (pipe(descriptors)) return 0;
    child = fork();
    if (child == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0 || dup2(descriptors[1], STDERR_FILENO) < 0) _exit(125);
        close(descriptors[1]);
        execl(s->cli, s->cli, "INSTALL", "hello", "ROOT", s->root,
              "CHANNEL", s->channel, "MACHINE", (char *)NULL);
        _exit(126);
    }
    close(descriptors[1]);
    if (child < 0) { close(descriptors[0]); return 0; }
    while (used + 1 < sizeof output) {
        got = read(descriptors[0], output + used, sizeof output - used - 1);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        used += (size_t)got;
    }
    output[used] = 0;
    close(descriptors[0]);
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == PKG_RC_CONFLICT
        && strstr(output, "next: retry-later") != NULL;
}
static void event(void *user, const struct pkg_event *e)
{
    struct observed *s = (struct observed *)user;
    char lock[1024];
    snprintf(lock, sizeof lock, "%s/.pkg/lock", s->root);
    if (e->type == PKG_EVENT_STEP_BEGIN && e->phase == PKG_PHASE_RESOLVE && !s->resolution++) {
        if (s->compete) {
            check(exists(s->root), "first resolution sees a created root");
            check(exists(lock), "first resolution holds the root lock");
            check(competing_install(s), "independent CLI at resolution refuses 15, retry-later");
        }
    }
    if (e->type == PKG_EVENT_STEP_BEGIN && e->phase == PKG_PHASE_FLUSH) {
        s->flush_begin++;
        if (s->compete) check(competing_install(s), "independent CLI during flush refuses 15, retry-later");
    }
    if (e->type == PKG_EVENT_STEP_END && e->phase == PKG_PHASE_FLUSH) {
        s->flush_end++;
        if (s->compete) check(exists(lock), "flush completes before the root lock is released");
    }
    if (e->type == PKG_EVENT_OP_END) {
        s->end++;
        s->end_code = e->code;
        if (s->compete) check(!exists(lock), "operation end follows root unlock");
    }
}
static void record(void *user, const char *key, const char *value)
{
    struct observed *s = (struct observed *)user;
    if (!strcmp(key, "flushed") && strcmp(value, "no")) s->flushed++;
    if (!strcmp(key, "reason")) snprintf(s->reason, sizeof s->reason, "%s", value);
}
static void reset(struct observed *s)
{
    s->resolution = s->flush_begin = s->flush_end = s->end = s->end_code = s->flushed = 0;
}
static void invalid(struct pkg_sink *sink, struct pkg_options *o, const char *label)
{
    check(pkg_install(sink, o) == PKG_RC_USAGE && !exists(o->root), label);
}
int main(int argc, char **argv)
{
    char tmp[] = "/tmp/apkg-first-install.XXXXXX";
    char key[512], drawer[512], payload[512], channel[512], root[512], lock[1024];
    struct observed s;
    struct pkg_sink sink;
    struct pkg_options o;
    FILE *f;
    int rc;
    if (argc != 2) { fprintf(stderr, "usage: test_first_install /path/to/pkg\n"); return 2; }
    if (!mkdtemp(tmp)) return 2;
    snprintf(key, sizeof key, "%s/key", tmp);
    snprintf(drawer, sizeof drawer, "%s/drawer", tmp);
    snprintf(payload, sizeof payload, "%s/drawer/hello", tmp);
    snprintf(channel, sizeof channel, "%s/channel", tmp);
    snprintf(root, sizeof root, "%s/parent/root", tmp);
    snprintf(lock, sizeof lock, "%s/.pkg/lock", root);
    if (mkdir(drawer, 0700)) return 2;
    f = fopen(payload, "wb");
    if (!f) return 2;
    fputs("first installation\n", f);
    if (fclose(f)) return 2;
    memset(&o, 0, sizeof o);
    o.file = key;
    if (pkg_keygen(NULL, &o)) return 2;
    memset(&o, 0, sizeof o);
    o.target = drawer; o.channel = channel; o.sign = key;
    o.name = "hello"; o.version = "1.0"; o.kind = "application"; o.arch = "generic";
    if (pkg_publish(NULL, &o)) return 2;
    memset(&s, 0, sizeof s);
    s.cli = argv[1]; s.root = root; s.channel = channel;
    memset(&sink, 0, sizeof sink);
    sink.user = &s; sink.structured = 1; sink.event = event; sink.record = record;
    memset(&o, 0, sizeof o);
    o.target = "hello"; o.root = root; o.channel = channel;
    o.dryrun = 1;
    check(pkg_install(&sink, &o) == 0 && !exists(root), "DRYRUN validates first install without creating root");
    check(s.flush_begin == 0 && s.flushed == 0, "DRYRUN emits no flush");
    o.dryrun = 0;
    reset(&s);
    check(pkg_usage_error(&sink, "install", "did you mean ROOT?") == PKG_RC_USAGE
          && !strcmp(s.reason, "did you mean ROOT?") && !exists(root)
          && s.end == 1 && s.resolution == 0 && s.flush_begin == 0,
          "INSTALL usage diagnostic preserves the caller's reason without preparation");
    o.target = NULL;
    invalid(&sink, &o, "missing package refuses before root creation");
    o.target = "../escape";
    invalid(&sink, &o, "invalid package refuses before root creation");
    o.target = "hello"; o.version = "bad version";
    invalid(&sink, &o, "invalid version refuses before root creation");
    o.version = NULL; o.arch = "bad arch";
    invalid(&sink, &o, "invalid architecture refuses before root creation");
    o.arch = NULL; o.channel = NULL;
    invalid(&sink, &o, "missing channel refuses before root creation");
    o.channel = "";
    invalid(&sink, &o, "empty channel refuses before root creation");
    o.channel = channel; o.nalso = 1;
    invalid(&sink, &o, "missing additional names refuse before root creation");
    o.nalso = 0;
    (void)pkg_list(&sink, &o);
    check(!exists(root), "read-only LIST creates no root");
    (void)pkg_verify(&sink, &o);
    check(!exists(root), "read-only VERIFY creates no root");
    (void)pkg_remove(&sink, &o);
    check(!exists(root), "REMOVE of an absent root creates no root");
    reset(&s);
    s.compete = 1;
    rc = pkg_install(&sink, &o);
    check(rc == 0 && s.end == 1 && s.end_code == 0, "first installation completes successfully");
    check(s.resolution > 0 && s.flush_begin == 1 && s.flush_end == 1 && s.flushed == 1,
          "first installation reports exactly one complete flush and its backend result");
    check(!exists(lock), "completed first installation leaves no lock file");
    s.compete = 0;
    check(pkg_verify(&sink, &o) == 0, "first installation verifies after unlocking");
    reset(&s);
    snprintf(root, sizeof root, "%s/failed-root", tmp);
    snprintf(lock, sizeof lock, "%s/.pkg/lock", root);
    o.target = "absent-package";
    check(pkg_install(&sink, &o) == PKG_RC_NOTFOUND, "unknown package keeps its not-found result");
    check(s.flush_begin == 1 && s.flush_end == 1 && !exists(lock), "failed first install flushes and unlocks");
    snprintf(root, sizeof root, "%s/root-is-file", tmp);
    f = fopen(root, "wb");
    if (!f) return 2;
    fputs("keep me", f); fclose(f);
    o.target = "hello";
    check(pkg_install(&sink, &o) == PKG_RC_IO, "a file at the root path is refused as IO");
    f = fopen(root, "rb");
    check(f && fgetc(f) == 'k', "root-path refusal preserves the existing file");
    if (f) fclose(f);
    /* A generated mkdtemp path contains no shell syntax. */
    {
        char cleanup[600];
        snprintf(cleanup, sizeof cleanup, "rm -rf %s", tmp);
        if (system(cleanup)) return 2;
    }
    printf("first install: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
