/* Pure helpers behind list and search, kept free of AROS calls so the host
 * tests link the same code the guest runs. */
#include "listing.h"
#include <stdlib.h>
#include <string.h>

static int lc(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch; }

static int ci_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle), i;
    if (!n) return 1;
    for (; *hay; hay++) {
        for (i = 0; i < n && hay[i] && lc((unsigned char)hay[i]) == lc((unsigned char)needle[i]); i++)
            ;
        if (i == n) return 1;
    }
    return 0;
}

int pkg_match(const char *id, const char *summary, const char *category,
              const char *term)
{
    size_t need;
    char *joined;
    int hit;
    if (!term || !*term) return 1;
    id = id ? id : ""; summary = summary ? summary : ""; category = category ? category : "";
    need = strlen(id) + strlen(summary) + strlen(category) + 3;
    joined = (char *)malloc(need);
    if (!joined) return 0;
    strcpy(joined, id); strcat(joined, " ");
    strcat(joined, summary); strcat(joined, " ");
    strcat(joined, category);
    hit = ci_contains(joined, term);
    free(joined);
    return hit;
}

static int ci_cmp(const void *a, const void *b)
{
    const char *x = *(const char * const *)a, *y = *(const char * const *)b;
    for (; *x && lc((unsigned char)*x) == lc((unsigned char)*y); x++, y++)
        ;
    return lc((unsigned char)*x) - lc((unsigned char)*y);
}

void pkg_sort_names(char **names, int n)
{
    if (names && n > 1) qsort(names, (size_t)n, sizeof *names, ci_cmp);
}

cache_act pkg_cache_decision(int present, int hash_matches, int size_matches)
{
    if (!present) return CACHE_FETCH;
    return (hash_matches && size_matches) ? CACHE_USE : CACHE_DISCARD_AND_FETCH;
}
