/* Regression tests for the three debts reported on 2026-09-12. */
#include "../src/libpkg/listing.h"
#include <stdio.h>
#include <string.h>

static int bad = 0;
static void ok(int c, const char *l) { printf("%-4s %s\n", c ? "ok" : "FAIL", l); if (!c) bad++; }

int main(void)
{
    /* search: the joined buffer was 256 bytes for fields that total 320, so a
       term matching the end of a long summary, or the category, was lost. */
    /* Sized to the old buffers: a 63-character id and a 191-character
       summary put the category at byte 256, one past the old join. */
    char id[64], sum[192];
    memset(id, 'i', 63);  id[63] = 0;
    memset(sum, 's', 181); sum[181] = 0;
    strcat(sum, " tailword!");                          /* 191 characters */
    ok(pkg_match(id, sum, "game/board", "board"),
       "search matches the category after a long id and summary");
    ok(pkg_match(id, sum, "game/board", "tailword"),
       "search matches a word at the end of a long summary");
    ok(pkg_match("sdllopan", "Lopan - MahJong clone", "game/board", "MAHJONG"),
       "search is case-insensitive");
    ok(!pkg_match("zaphod", "A simple binary file editor", "development/edit", "mahjong"),
       "search does not match what is not there");
    ok(pkg_match("zaphod", "x", "y", "") && pkg_match("zaphod", "x", "y", NULL),
       "an empty term matches everything");

    /* list: filesystem order was newest-first on AROS One */
    {
        char a[] = "sdlpop", b[] = "zaphod", c[] = "sdllopan", d[] = "Abc";
        char *n[] = { a, b, c, d };
        pkg_sort_names(n, 4);
        ok(!strcmp(n[0], "Abc") && !strcmp(n[1], "sdllopan") &&
           !strcmp(n[2], "sdlpop") && !strcmp(n[3], "zaphod"),
           "list order is alphabetical, case-insensitive");
    }

    /* cache: a damaged cached archive used to block the package forever */
    ok(pkg_cache_decision(0, 0, 0) == CACHE_FETCH,             "no cached copy: fetch");
    ok(pkg_cache_decision(1, 1, 1) == CACHE_USE,               "matching cached copy: use it");
    ok(pkg_cache_decision(1, 0, 1) == CACHE_DISCARD_AND_FETCH, "wrong hash: discard and fetch");
    ok(pkg_cache_decision(1, 1, 0) == CACHE_DISCARD_AND_FETCH, "wrong size: discard and fetch");

    printf(bad ? "\nFAIL %d\n" : "\nPASS all checks\n", bad);
    return bad != 0;
}
