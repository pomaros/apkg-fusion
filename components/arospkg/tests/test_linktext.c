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

    /* Running text: <address> is taken as written. */
    found("get it from <https://example.org/download?token=abc!>.", "https://example.org/download?token=abc!");
    found("<https://e.org/a.> and https://e.org/b.", "https://e.org/a. | https://e.org/b");

    /* A field that is an address is used exactly as it is. */
    {
        const char *f = "https://example.org/download?token=abc!";
        lk_doc d = {0};
        lk_field fd;
        check(lk_exact(f), "field with final ! is an address");
        fd.label = NULL; fd.url = f;
        lk_doc_markup(&d, "from \1 now", 72, &fd, 1);
        check(d.np == 1 && !strcmp(d.p[0].target, f), "field kept whole, final ! included");
        check(!strcmp(d.text, "from https://example.org/download?token=abc! now"), "field shown in its sentence");
        lk_doc_free(&d);
        fd.label = "Project website"; fd.url = "https://example.org/x.";
        lk_doc_markup(&d, "\1", 72, &fd, 1);
        check(!strcmp(d.text, "Project website") && d.np == 1 && !strcmp(d.p[0].target, "https://example.org/x."),
              "label shown, target kept whole, final . included");
        lk_doc_free(&d);
        fd.label = NULL; fd.url = "/local/path";
        lk_doc_markup(&d, "x \1", 72, &fd, 1);
        check(!strcmp(d.text, "x /local/path") && d.np == 0, "a field that is no address is text");
        lk_doc_free(&d);
        check(!lk_exact("https://"), "scheme alone is no address");
        check(!lk_exact("https://a.org/b c"), "space: not one address");
        check(!lk_exact("ftp://a.org/"), "ftp is not linked");
        check(!lk_exact(""), "empty field");
    }

    /* Running text: links where they stand; repeats too; ends as read. */
    {
        lk_doc d = {0};
        char text[1024];
        size_t k = 0; int i;
        for (i = 1; i <= 9; i++)
            k += (size_t)snprintf(text + k, sizeof text - k, "link %d: https://e.org/%d, ", i, i);
        snprintf(text + k, sizeof text - k, "again https://e.org/3.");
        lk_doc_markup(&d, text, 72, NULL, 0);
        check(d.np == 10, "ten links, the repeat its own");
        check(!strcmp(d.p[0].target, "https://e.org/1") && !strcmp(d.p[8].target, "https://e.org/9"), "in order");
        check(!strcmp(d.p[9].target, "https://e.org/3"), "final . is left to the sentence");
        for (i = 0; i < d.np; i++)
            check(!memcmp(d.text + d.p[i].start, d.p[i].target, d.p[i].len), "the link stands on its address");
        lk_doc_free(&d);
    }

    /* Floattext's bold, a link inside it, spans in order and apart. */
    {
        lk_doc d = {0}, e = {0};
        lk_doc_markup(&d, "\33bNotes\33n\nsee <https://e.org/a!>, \33bbold https://e.org/b tail\33n plain\33x", 72, NULL, 0);
        check(!strcmp(d.text, "Notes\nsee <https://e.org/a!>, bold https://e.org/b tail plain"), "codes are gone");
        check(d.np == 5, "five pieces");
        check(d.p[0].bold && !d.p[0].target && d.p[0].len == 5, "bold text");
        check(!d.p[1].bold && !strcmp(d.p[1].target, "https://e.org/a!"), "<address> exact");
        check(d.p[2].bold && !d.p[2].target, "bold before the link");
        check(d.p[3].bold && !strcmp(d.p[3].target, "https://e.org/b"), "the link stays bold");
        check(d.p[4].bold && !d.p[4].target, "bold after the link");
        {
            int i;
            for (i = 1; i < d.np; i++)
                check(d.p[i].start >= d.p[i - 1].start + d.p[i - 1].len, "pieces in order, apart");
        }
        lk_doc_markup(&e, "\33bNotes\33n\nsee <https://e.org/a!>, \33bbold https://e.org/b tail\33n plain\33x", 72, NULL, 0);
        check(lk_doc_same(&d, &e), "same content");
        lk_doc_free(&e);
        lk_doc_markup(&e, "\33bNotes\33n\nsee <https://e.org/a?>, \33bbold https://e.org/b tail\33n plain\33x", 72, NULL, 0);
        check(!lk_doc_same(&d, &e), "another target is another content");
        lk_doc_free(&e);
        lk_doc_free(&d);
    }

    /* Long addresses (5000 characters) stay whole, shown short. */
    {
        lk_doc d = {0}, e = {0};
        lk_field fd;
        size_t len = 5000, i;
        char *u = malloc(len + 40), *t = malloc(len + 80);
        strcpy(u, "https://example.org/");
        for (i = strlen(u); i < len; i++) u[i] = (char)('a' + i % 26);
        u[len - 1] = '!'; u[len] = 0;
        fd.label = NULL; fd.url = u;
        lk_doc_markup(&d, "\1", 48, &fd, 1);
        check(d.np == 1 && strlen(d.p[0].target) == len && !strcmp(d.p[0].target, u), "5000-character field whole");
        check(d.n <= 48 && strstr(d.text, "...") != NULL, "5000-character address shortened on screen");
        snprintf(t, len + 80, "see <%s> or", u);
        lk_doc_markup(&e, t, 48, NULL, 0);
        check(e.np == 1 && !strcmp(e.p[0].target, u), "5000-character <address> in text whole");
        check(strstr(e.text, " or") != NULL && e.n < 70, "the sentence goes on after it");
        lk_doc_free(&d); lk_doc_free(&e);
        check(lk_openable(u) == LK_OPEN_LONG, "5000 characters: copied, not opened");
        u[LK_OPEN_MAX] = 0;
        check(lk_openable(u) == LK_OPEN_OK, "LK_OPEN_MAX characters may be opened");
        u[LK_OPEN_MAX] = 'x'; u[LK_OPEN_MAX + 1] = 0;
        check(lk_openable(u) == LK_OPEN_LONG, "one more is copied only");
        u[LK_OPEN_MAX + 1] = 'x'; u[len] = 0;
        check(lk_openable("https://e.org/a;b") == LK_OPEN_CHARS, "; copied only");
        lk_label(u, buf, sizeof buf, 50);
        check(strlen(buf) == 50 && buf[49] == '!', "label of a long address keeps its end");
        free(u); free(t);
    }

    /* Common reports: repository fields exact, raw JSON not searched,
       running text by the usual rules. */
    {
        lk_doc d = {0};
        lk_doc_report(&d, "Source repository: https://e.org/release!\n"
            "{\"presentation\":{\"repository\":\"https://e.org/json\"}}\n"
            "Notes: see https://e.org/help.\nrepository https://e.org/other;\n", 72);
        check(!d.failed && d.np == 3, "report: two fields and one address in prose, none from JSON");
        check(d.np == 3 && !strcmp(d.p[0].target, "https://e.org/release!") &&
              !strcmp(d.p[1].target, "https://e.org/help") &&
              !strcmp(d.p[2].target, "https://e.org/other;"), "report: punctuation kept only for fields");
        check(d.text && strstr(d.text, "\"repository\":\"https://e.org/json\"") != NULL, "report: the JSON line is shown as written");
        lk_doc_free(&d);
        lk_doc_report(&d, NULL, 72);
        lk_doc_report(&d, "repository unknown\nSource repository: /local/tree\n", 72);
        check(d.np == 0 && d.text && strstr(d.text, "/local/tree") != NULL, "report: missing and non-web repositories stay text");
        lk_doc_free(&d);
    }

    if (fails) { printf("test_linktext: %d FAILED\n", fails); return 1; }
    printf("test_linktext: all passed\n");
    return 0;
}
