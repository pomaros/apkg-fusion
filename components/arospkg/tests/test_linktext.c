/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak <https://tomaszstaniak.com>
 * Copyright (c) 2026 John Knipper */
/* Host tests for src/pkgmanager/linktext.c: which addresses PkgManager
 * links, how it shortens them, which it will not hand to a browser. */
#include "../src/pkgmanager/linktext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) { printf("FAIL %s\n", what); fails++; }
}

/* Every address in `text`, joined with " | ". */
static const char *all(const char *text)
{
    static char out[2048];
    const char *p = text, *at;
    size_t len, k = 0;
    out[0] = 0;
    for (;;) {
        at = text;
        len = lk_next_url(p, &at);
        if (!len) break;
        k += (size_t)snprintf(out + k, sizeof out - k, "%s%.*s", k ? " | " : "", (int)len, at);
        p = at + len;
    }
    return out;
}

static void found(const char *text, const char *want)
{
    const char *got = all(text);
    if (strcmp(got, want)) { printf("FAIL in [%s]\n  got  [%s]\n  want [%s]\n", text, got, want); fails++; }
}

int main(void)
{
    char buf[512];

    found("source: https://github.com/a/b", "https://github.com/a/b");
    found("see https://example.org/x.", "https://example.org/x");
    found("(https://example.org/x) and http://e.org/y, too", "https://example.org/x | http://e.org/y");
    found("https://en.wikipedia.org/wiki/A_(B)", "https://en.wikipedia.org/wiki/A_(B)");
    found("q https://e.org/?a=1&b=%2F&c=%41 end", "https://e.org/?a=1&b=%2F&c=%41");
    found("\"https://e.org/q\"", "https://e.org/q");
    found("<https://e.org/q>", "https://e.org/q");
    found("HTTPS://E.ORG/", "HTTPS://E.ORG/");
    found("ftp://e.org/x mailto:a@b.c file:///x", "");
    found("xhttps://e.org/ is a word", "");
    found("https:// alone", "");
    found("two https://a.org/1 https://a.org/1", "https://a.org/1 | https://a.org/1");
    found("line\nhttps://a.org/\nnext", "https://a.org/");

    check(lk_unsafe("https://e.org/a;b"), "; is unsafe");
    check(lk_unsafe("https://e.org/$HOME"), "$ is unsafe");
    check(lk_unsafe("https://e.org/a*b"), "* is unsafe");
    check(lk_unsafe("https://e.org/it's"), "' is unsafe");
    check(!lk_unsafe("https://e.org/?a=1&b=%2F#frag"), "& % # are safe");

    lk_label("https://example.org/a/very/long/path/that/goes/on/and/on/index.html", buf, sizeof buf, 32);
    check(strlen(buf) == 32, "label is max characters");
    check(!strncmp(buf, "https://example.org", 19), "label keeps the start");
    check(strstr(buf, "...") != NULL, "label marks the cut");
    check(!strcmp(buf + strlen(buf) - 10, "index.html"), "label keeps the end");
    lk_label("https://e.org/", buf, sizeof buf, 32);
    check(!strcmp(buf, "https://e.org/"), "short address unchanged");

    lk_shorten("failed (https://example.invalid/a/very/long/name-1.0.x86_64-aros-v11.zip) -- why",
               buf, sizeof buf, 30);
    check(strlen(buf) < 80 && strstr(buf, "failed (https://") == buf && strstr(buf, ") -- why"),
          "shorten keeps the text around");
    lk_shorten("no address here", buf, sizeof buf, 10);
    check(!strcmp(buf, "no address here"), "shorten without address");
    lk_shorten("abc https://a.org/1234567890", buf, 8, 40);
    check(strlen(buf) == 7, "shorten respects the buffer");

    /* Running text: <address> is taken as written. */
    found("get it from <https://example.org/download?token=abc!>.", "https://example.org/download?token=abc!");
    found("<https://e.org/a.> and https://e.org/b.", "https://e.org/a. | https://e.org/b");

    /* A field that is an address is used exactly as it is. */
    {
        lk_list l = {0};
        const char *f = "https://example.org/download?token=abc!";
        check(lk_exact(f), "field with final ! is an address");
        check(lk_list_url(&l, f) && l.n == 1 && !strcmp(l.v[0], f), "field kept whole, final ! included");
        check(lk_list_url(&l, "https://example.org/x.") && !strcmp(l.v[1], "https://example.org/x."),
              "field kept whole, final . included");
        check(!lk_exact("https://"), "scheme alone is no address");
        check(!lk_exact("https://a.org/b c"), "space: not one address");
        check(!lk_exact("ftp://a.org/"), "ftp is not linked");
        check(!lk_exact(""), "empty field");
        check(!lk_list_url(&l, "/local/path") && l.n == 2, "not an address: nothing added");
        lk_list_free(&l);
    }

    /* Fusion reports preserve exact fields without scanning raw JSON twice. */
    {
        lk_list l = {0};
        lk_list_report(&l, "Source repository: https://e.org/release!\n"
            "{\"presentation\":{\"repository\":\"https://e.org/release!\"}}\n"
            "Notes: see https://e.org/help.\nrepository https://e.org/other;\n");
        check(l.n == 3, "report has exact fields and prose, no raw JSON links");
        check(l.n == 3 && !strcmp(l.v[0], "https://e.org/release!") &&
            !strcmp(l.v[1], "https://e.org/help") &&
            !strcmp(l.v[2], "https://e.org/other;"), "report punctuation retained only for fields");
        lk_list_free(&l);
        lk_list_report(&l, NULL);
        lk_list_report(&l, "repository unknown\nSource repository: /local/tree\n");
        check(l.n == 0, "missing and non-web repositories add no link");
        lk_list_free(&l);
    }

    /* Every address of a text, however many: nine here, in order, no repeats. */
    {
        lk_list l = {0};
        char text[1024];
        size_t k = 0; int i;
        for (i = 1; i <= 9; i++)
            k += (size_t)snprintf(text + k, sizeof text - k, "link %d: https://e.org/%d, ", i, i);
        lk_list_text(&l, text, (size_t)-1);
        lk_list_text(&l, "again https://e.org/3", (size_t)-1);
        check(l.n == 9, "nine addresses, the repeat once");
        check(l.n == 9 && !strcmp(l.v[0], "https://e.org/1") && !strcmp(l.v[8], "https://e.org/9"), "in order");
        lk_list_free(&l);
    }

    /* Only the first n characters: an address must end inside them. */
    {
        lk_list l = {0};
        const char *t = "a https://e.org/one b https://e.org/two";
        lk_list_text(&l, t, 19);
        check(l.n == 1 && !strcmp(l.v[0], "https://e.org/one"), "part of a text");
        lk_list_text(&l, t, 0);
        check(l.n == 1, "an empty part adds nothing");
        lk_list_free(&l);
    }

    /* Long addresses (5000 characters) are listed whole, labelled short. */
    {
        lk_list l = {0}, m = {0};
        size_t len = 5000, i;
        char *u = malloc(len + 40), *t = malloc(len + 80), *sh = malloc(len + 80);
        strcpy(u, "https://example.org/");
        for (i = strlen(u); i < len; i++) u[i] = (char)('a' + i % 26);
        u[len - 1] = '!'; u[len] = 0;
        check(lk_list_url(&l, u) && strlen(l.v[0]) == len && !strcmp(l.v[0], u), "5000-character field whole");
        snprintf(t, len + 80, "see <%s> or", u);
        lk_list_text(&m, t, (size_t)-1);
        check(m.n == 1 && !strcmp(m.v[0], u), "5000-character <address> in text whole");
        check(lk_list_same(&l, &m), "same lists");
        lk_shorten(t, sh, len + 80, 40);
        check(strlen(sh) < 60 && strstr(sh, "...") != NULL, "5000-character address shortened on screen");
        check(lk_openable(u) == LK_OPEN_LONG, "5000 characters: copied, not opened");
        u[LK_OPEN_MAX] = 0;
        check(lk_openable(u) == LK_OPEN_OK, "LK_OPEN_MAX characters may be opened");
        u[LK_OPEN_MAX] = 'x'; u[LK_OPEN_MAX + 1] = 0;
        check(lk_openable(u) == LK_OPEN_LONG, "one more is copied only");
        u[LK_OPEN_MAX + 1] = 'x'; u[len] = 0;
        check(lk_openable("https://e.org/a;b") == LK_OPEN_CHARS, "; copied only");
        lk_label(u, buf, sizeof buf, 50);
        check(strlen(buf) == 50 && buf[49] == '!', "label of a long address keeps its end");
        lk_list_free(&l); lk_list_free(&m);
        free(u); free(t); free(sh);
    }

    if (fails) { printf("test_linktext: %d FAILED\n", fails); return 1; }
    printf("test_linktext: all passed\n");
    return 0;
}
