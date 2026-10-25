/* The rows behind search, list and the Zune front end: one reading of the
 * index, checked here on the host with the same code the guest runs. */
#include "../src/libpkg/entries.h"
#include "../src/libpkg/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad = 0;
static void ok(int c, const char *l) { printf("%-4s %s\n", c ? "ok" : "FAIL", l); if (!c) bad++; }

static const char INDEX[] =
"{ \"schema\": 1, \"packages\": [\n"
" { \"id\": \"sdllopan\", \"version\": \"1.0\", \"revision\": 2, \"summary\": \"Lopan - MahJong clone\",\n"
"   \"arch\": \"x86_64\", \"abi\": \"v11\", \"category\": \"game/board\", \"size\": 490689, \"kind\": \"app\",\n"
"   \"requires_system\": [ { \"type\": \"library\", \"id\": \"SDL.library\" }, { \"type\": \"library\", \"id\": \"crt.library\" } ] },\n"
" { \"id\": \"zaphod\", \"version\": \"0.9\", \"summary\": \"A simple binary file editor\",\n"
"   \"arch\": \"x86_64\", \"abi\": \"v1\", \"category\": \"development/edit\", \"size\": 10, \"kind\": \"app\" },\n"
" { \"id\": \"antiword\", \"version\": \"0.37\", \"summary\": \"Convert Word documents\",\n"
"   \"arch\": \"x86_64\", \"abi\": \"v11\", \"category\": \"utility/text\", \"size\": 291711, \"kind\": \"app\" }\n"
"] }\n";

static const char REG[] =
"{\n  \"schema\": 1,\n  \"name\": \"sdllopan\",\n  \"version\": \"1.0\",\n  \"arch\": \"x86_64\",\n"
"  \"revision\": 2,\n  \"installed\": \"2026-09-22\",\n  \"origin\": \"https://x/y.zip\",\n  \"archive_sha256\": \"ab\",\n"
"  \"archive_size\": 490689,\n  \"reason\": \"explicit\",\n  \"dir\": \"sdllopan\",\n  \"contents\": []\n}\n";

int main(void)
{
    pkg_entries es = {0};
    int hidden = -1;

    ok(entries_from_index(INDEX, sizeof INDEX - 1, NULL, "v11", 0, &es, &hidden) == 0,
       "the index parses into rows");
    ok(es.n == 2 && hidden == 1, "on v11: two rows, the v1 package counted as hidden");
    ok(!strcmp(es.v[0].id, "antiword") && !strcmp(es.v[1].id, "sdllopan"),
       "rows come out sorted by id, whatever order the file had");
    ok(es.v[0].revision == 0 && es.v[1].revision == 2,
       "revision is 0 when absent, the number when present");
    ok(es.v[1].size == 490689 && !strcmp(es.v[1].kind, "app") &&
       !strcmp(es.v[1].category, "game/board"),
       "size, kind and category are carried");
    ok(es.v[0].ours && es.v[1].ours && !es.v[0].installed,
       "ABI match is flagged; installed is the caller's to fill");
    ok(!strcmp(es.v[1].requires, "SDL.library, crt.library") && es.v[0].requires[0] == 0,
       "requires_system ids are carried, comma separated; none means empty");
    pkg_entries_free(&es);

    ok(entries_from_index(INDEX, sizeof INDEX - 1, NULL, "v11", 1, &es, &hidden) == 0 &&
       es.n == 3 && hidden == 0 && !es.v[2].ours,
       "show_all keeps the other ABI's row and marks it not ours");
    pkg_entries_free(&es);

    ok(entries_from_index(INDEX, sizeof INDEX - 1, NULL, "", 0, &es, &hidden) == 0 &&
       es.n == 3 && es.v[2].ours,
       "a build that recorded no ABI hides nothing -- it cannot tell");
    pkg_entries_free(&es);

    ok(entries_from_index(INDEX, sizeof INDEX - 1, "MAHJONG", "v11", 0, &es, &hidden) == 0 &&
       es.n == 1 && !strcmp(es.v[0].id, "sdllopan"),
       "the term filters as search does, case-insensitively");
    pkg_entries_free(&es);

    ok(entries_from_index("{ \"nothing\": 1 }", 16, NULL, "v11", 0, &es, &hidden) == -1,
       "text without a packages array is refused, not read as empty");
    pkg_entries_free(&es);

    {
        pkg_entry e;
        ok(entry_from_registry(REG, sizeof REG - 1, &e) == 0 &&
           !strcmp(e.id, "sdllopan") && !strcmp(e.version, "1.0") &&
           e.installed && e.size == 490689 && e.summary[0] == 0 &&
           e.installed_revision == 2 && !strcmp(e.installed_version, "1.0"),
           "a registry entry becomes a row: id, version, revision, size; no summary");
        ok(entry_from_registry("[]", 2, &e) == -1, "a non-object is refused");
    }

    {
        pkg_entry a = {0}, b = {0}, c = {0};
        strcpy(a.id, "zaphod"); strcpy(b.id, "Abc"); strcpy(c.id, "sdllopan");
        entries_push(&es, &a); entries_push(&es, &b); entries_push(&es, &c);
        entries_sort(&es);
        ok(!strcmp(es.v[0].id, "Abc") && !strcmp(es.v[1].id, "sdllopan") &&
           !strcmp(es.v[2].id, "zaphod"),
           "sort is case-insensitive by id, as AROS compares names");
        pkg_entries_free(&es);
    }

    ok(pkg_abi_known("v1") && pkg_abi_known("v11") && !pkg_abi_known("v2") && !pkg_abi_known(""),
       "only v1 and v11 are known ABIs");

    /* whether a target runs here, and why */
    {
        char why[200];
        ok(pkg_compat_of("x86_64", "v11", "x86_64", "v11", why, sizeof why) == PKG_COMPAT_NATIVE &&
           strstr(why, "built for this machine"), "same CPU and ABI: native");
        ok(pkg_compat_of("x86_64", "v1", "x86_64", "v11", why, sizeof why) == PKG_COMPAT_INCOMPATIBLE &&
           strstr(why, "ABIv1, mainline AROS") && strstr(why, "ABIv11"), "other ABI: no, and says which");
        ok(pkg_compat_of("aarch64", "v11", "x86_64", "v11", why, sizeof why) == PKG_COMPAT_INCOMPATIBLE &&
           strstr(why, "built for aarch64; this machine is x86_64"), "other CPU: no, before the ABI is looked at");
        ok(pkg_compat_of("x86_64", "v9", "x86_64", "v11", why, sizeof why) == PKG_COMPAT_UNDETERMINED &&
           strstr(why, "\"v9\""), "an ABI this client does not know: undetermined, never native");
        ok(pkg_compat_of("i386", "v0", "x86_64", "v11", why, sizeof why) == PKG_COMPAT_INCOMPATIBLE &&
           strstr(why, "built for i386; this machine is x86_64"), "i386/v0 on x86_64: the CPU refuses it");
        ok(pkg_compat_of("i386", "v0", "i386", "v0", why, sizeof why) == PKG_COMPAT_NATIVE &&
           strstr(why, "ABIv0"), "i386/v0 on i386/v0: native");
        ok(pkg_compat_of("x86_64", "v11", "i386", "", why, sizeof why) == PKG_COMPAT_INCOMPATIBLE,
           "a build that does not know its ABI still refuses another CPU");
        ok(pkg_compat_of("x86_64", "", "x86_64", "v11", why, sizeof why) == PKG_COMPAT_UNDETERMINED,
           "no ABI in the entry: undetermined");
        ok(pkg_compat_of("aarch64", "v11", "", "v11", why, sizeof why) == PKG_COMPAT_NATIVE,
           "no CPU for the machine: the CPU is not checked");
        ok(!strcmp(pkg_compat_word(PKG_COMPAT_NATIVE), "native") && !strcmp(pkg_compat_word(PKG_COMPAT_INCOMPATIBLE), "incompatible") &&
           !strcmp(pkg_compat_word(PKG_COMPAT_UNDETERMINED), "undetermined"), "the three words");
    }

    /* the rows search gives now check the CPU too */
    {
        static const char IX[] = "{\"packages\":["
            "{\"id\":\"a\",\"arch\":\"x86_64\",\"abi\":\"v11\"},"
            "{\"id\":\"b\",\"arch\":\"aarch64\",\"abi\":\"v11\"},"
            "{\"id\":\"c\",\"arch\":\"x86_64\",\"abi\":\"v1\"}]}";
        int hidden = 0;
        pkg_entries rows = {0};
        ok(entries_from_index_on(IX, sizeof IX - 1, NULL, "x86_64", "v11", 0, &rows, &hidden) == 0 &&
           rows.n == 1 && !strcmp(rows.v[0].id, "a") && hidden == 2, "other CPU and other ABI are both hidden");
        pkg_entries_free(&rows);
        ok(entries_from_index(IX, sizeof IX - 1, NULL, "v11", 0, &rows, &hidden) == 0 && rows.n == 2 && hidden == 1,
           "without a CPU, only the ABI hides, as before");
        pkg_entries_free(&rows);
        ok(entries_from_index_on(IX, sizeof IX - 1, NULL, "x86_64", "", 0, &rows, &hidden) == 0 &&
           hidden >= 1 && rows.n >= 1,
           "a build that does not know its ABI still hides other CPUs");
        {   int i, other = 0;
            for (i = 0; i < rows.n; i++) if (strcmp(rows.v[i].arch, "x86_64")) other++;
            ok(other == 0, "and lists none of them");
        }
        pkg_entries_free(&rows);
    }

    /* one package's details, and which variant they describe */
    {
        static const char IX[] = "{\"packages\":["
            "{\"id\":\"rescode\",\"version\":\"1.2\",\"arch\":\"i386\",\"abi\":\"v0\",\"url\":\"u-i386\"},"
            "{\"id\":\"rescode\",\"version\":\"1.2\",\"arch\":\"x86_64\",\"abi\":\"v11\",\"revision\":3,"
              "\"url\":\"u-v11\",\"sha256\":\"abc\",\"size\":89500,\"source\":\"https://src\",\"license\":\"GPL-2.0\","
              "\"requires_system\":[{\"type\":\"library\",\"id\":\"crt.library\"}]},"
            "{\"id\":\"old\",\"version\":\"1\",\"arch\":\"x86_64\",\"abi\":\"v1\",\"url\":\"u-old\"}]}";
        pkg_details d;
        ok(details_from_index(IX, sizeof IX - 1, "rescode", "x86_64", "v11", &d) == 0 &&
           !strcmp(d.url, "u-v11") && d.compat == PKG_COMPAT_NATIVE && d.e.revision == 3 &&
           d.nvariants == 2 && !strcmp(d.variants, "i386/v0, x86_64/v11"),
           "two variants: the native one is described, both are listed, whatever their order");
        ok(!strcmp(d.sha256, "abc") && d.e.size == 89500 && !strcmp(d.source, "https://src") &&
           !strcmp(d.license, "GPL-2.0") && !strcmp(d.e.requires, "crt.library") && d.in_index,
           "download, source, licence and requirements come with it");
        ok(details_from_index(IX, sizeof IX - 1, "old", "x86_64", "v11", &d) == 0 &&
           d.compat == PKG_COMPAT_INCOMPATIBLE && !strcmp(d.url, "u-old") && d.nvariants == 1,
           "only another ABI: described, and said not to run");
        ok(details_from_index(IX, sizeof IX - 1, "nosuch", "x86_64", "v11", &d) == 1, "no such id: 1");
        ok(details_from_index("[]", 2, "rescode", "x86_64", "v11", &d) == -1, "not an index: -1");
    }

    /* which entry an operation means, whatever the order of the index */
    {
        static const char A[] = "{\"packages\":["
            "{\"id\":\"p\",\"arch\":\"i386\",\"abi\":\"v0\",\"url\":\"u-v0\"},"
            "{\"id\":\"p\",\"arch\":\"x86_64\",\"abi\":\"v11\",\"url\":\"u-v11\"},"
            "{\"id\":\"p\",\"arch\":\"aarch64\",\"abi\":\"v11\",\"url\":\"u-arm\"}]}";
        static const char B[] = "{\"packages\":["
            "{\"id\":\"p\",\"arch\":\"aarch64\",\"abi\":\"v11\",\"url\":\"u-arm\"},"
            "{\"id\":\"p\",\"arch\":\"x86_64\",\"abi\":\"v11\",\"url\":\"u-v11\"},"
            "{\"id\":\"p\",\"arch\":\"i386\",\"abi\":\"v0\",\"url\":\"u-v0\"}]}";
        static const char TWO1[] = "{\"packages\":["
            "{\"id\":\"p\",\"arch\":\"x86_64\",\"abi\":\"v11\",\"url\":\"u-b\"},"
            "{\"id\":\"p\",\"arch\":\"x86_64\",\"abi\":\"v11\",\"url\":\"u-a\"}]}";
        static const char TWO2[] = "{\"packages\":["
            "{\"id\":\"p\",\"arch\":\"x86_64\",\"abi\":\"v11\",\"url\":\"u-a\"},"
            "{\"id\":\"p\",\"arch\":\"x86_64\",\"abi\":\"v11\",\"url\":\"u-b\"}]}";
        static const char NONE1[] = "{\"packages\":["
            "{\"id\":\"p\",\"arch\":\"x86_64\",\"abi\":\"v1\",\"url\":\"u-v1\"},"
            "{\"id\":\"p\",\"arch\":\"aarch64\",\"abi\":\"v11\",\"url\":\"u-arm\"}]}";
        static const char NONE2[] = "{\"packages\":["
            "{\"id\":\"p\",\"arch\":\"aarch64\",\"abi\":\"v11\",\"url\":\"u-arm\"},"
            "{\"id\":\"p\",\"arch\":\"x86_64\",\"abi\":\"v1\",\"url\":\"u-v1\"}]}";
        struct { const char *j; size_t n; const char *arch, *abi; const char *url; int how, same; const char *what; } c[] = {
            { A, sizeof A - 1, "x86_64", "v11", "u-v11", VARIANT_ONE, 1, "one for this machine among three" },
            { B, sizeof B - 1, "x86_64", "v11", "u-v11", VARIANT_ONE, 1, "the same, the index reversed" },
            { A, sizeof A - 1, "i386", "v0", "u-v0", VARIANT_ONE, 1, "an i386/v0 client takes the i386 entry" },
            { B, sizeof B - 1, "i386", "v0", "u-v0", VARIANT_ONE, 1, "the same, the index reversed" },
            { A, sizeof A - 1, "aarch64", "v11", "u-arm", VARIANT_ONE, 1, "an installed aarch64 build is upgraded to aarch64" },
            { TWO1, sizeof TWO1 - 1, "x86_64", "v11", "u-a", VARIANT_SEVERAL, 2, "two for this machine: several, the smaller url" },
            { TWO2, sizeof TWO2 - 1, "x86_64", "v11", "u-a", VARIANT_SEVERAL, 2, "the same, the index reversed" },
            { NONE1, sizeof NONE1 - 1, "x86_64", "v11", "u-arm", VARIANT_NONE, 0, "none for this machine: the smaller (arch, abi, url) is shown" },
            { NONE2, sizeof NONE2 - 1, "x86_64", "v11", "u-arm", VARIANT_NONE, 0, "the same, the index reversed" },
        };
        size_t i;
        for (i = 0; i < sizeof c / sizeof c[0]; i++) {
            js_tok *t = (js_tok *)malloc(sizeof(js_tok) * 256);
            int ntok = js_parse(c[i].j, c[i].n, t, 256), arr = js_member(c[i].j, t, ntok, 0, "packages");
            int how = -1, same = -1, el = index_select_variant(c[i].j, t, ntok, arr, "p", c[i].arch, c[i].abi, &how, &same);
            char url[32] = "";
            if (el >= 0) js_str(c[i].j, t, js_member(c[i].j, t, ntok, el, "url"), url, sizeof url);
            ok(el >= 0 && !strcmp(url, c[i].url) && how == c[i].how && same == c[i].same, c[i].what);
            {
                int h2; ok(index_select_variant(c[i].j, t, ntok, arr, "q", c[i].arch, c[i].abi, &h2, NULL) == -1,
                           "no such id: -1");
            }
            free(t);
        }
    }

    /* post_install_notes: plain lines within limits, or nothing at all. */
    {
        static const struct { const char *j; int want; const char *out; const char *what; } c[] = {
            { "{\"post_install_notes\": [\"Needs the original game data.\", \"Copy it to the drawer.\"]}", 2,
              "Needs the original game data.\nCopy it to the drawer.", "two plain lines, joined by a newline" },
            { "{\"id\": \"x\"}", 0, "", "absent: no notes, not an error" },
            { "{\"post_install_notes\": []}", 0, "", "an empty list: no notes" },
            { "{\"post_install_notes\": \"one string\"}", -1, "", "not a list: refused" },
            { "{\"post_install_notes\": [\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}", -1, "",
              "nine lines: refused, not cut to eight" },
            { "{\"post_install_notes\": [\"tab\there\"]}", -1, "", "an escape sequence: refused, not shown undecoded" },
            { "{\"post_install_notes\": [\"bell \a\x1b[31m red\"]}", -1, "", "control characters (a terminal sequence): refused" },
            { "{\"post_install_notes\": [1, 2]}", -1, "", "not strings: refused" },
        };
        size_t i;
        char longline[300];
        for (i = 0; i < sizeof c / sizeof c[0]; i++) {
            js_tok t[64]; char out[PKG_NOTES_MAX] = "x";
            int ntok = js_parse(c[i].j, strlen(c[i].j), t, 64);
            int r = ntok > 0 ? notes_from_element(c[i].j, t, ntok, 0, out, sizeof out) : -9;
            ok(r == c[i].want && !strcmp(out, c[i].out), c[i].what);
        }
        memset(longline, 0, sizeof longline);
        strcpy(longline, "{\"post_install_notes\": [\"");
        memset(longline + strlen(longline), 'a', PKG_NOTES_LINE + 1);
        strcat(longline, "\"]}");
        {
            js_tok t[16]; char out[PKG_NOTES_MAX] = "x";
            int ntok = js_parse(longline, strlen(longline), t, 16);
            ok(notes_from_element(longline, t, ntok, 0, out, sizeof out) == -1 && out[0] == 0,
               "a line one character over the limit: refused, not cut");
            longline[strlen("{\"post_install_notes\": [\"") + PKG_NOTES_LINE] = '"';
            strcpy(longline + strlen("{\"post_install_notes\": [\"") + PKG_NOTES_LINE + 1, "]}");
            ntok = js_parse(longline, strlen(longline), t, 16);
            ok(notes_from_element(longline, t, ntok, 0, out, sizeof out) == 1 && strlen(out) == PKG_NOTES_LINE,
               "a line of exactly the limit: kept whole");
        }
    }

    printf(bad ? "\nFAIL %d\n" : "\nPASS all checks\n", bad);
    return bad != 0;
}
