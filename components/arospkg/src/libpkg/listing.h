#ifndef PKG_LISTING_H
#define PKG_LISTING_H
#include <stddef.h>

/* Case-insensitive substring match of `term` over id, summary and category,
 * joined with spaces. No fixed buffer: the previous one was 256 bytes against
 * fields that can total 320, so a term matching the end of a long summary or
 * the category silently failed. An empty or NULL term matches everything. */
int  pkg_match(const char *id, const char *summary, const char *category,
               const char *term);

/* Sort names in place, case-insensitively, as AROS compares names. */
void pkg_sort_names(char **names, int n);

/* What to do with an archive already in the cache.
 *
 * The cache is ours: <root>/cache/<id>.zip, written only after the download
 * matched the index. A cached copy that no longer matches -- truncated,
 * damaged, or from an index that has since moved to a new version -- is
 * therefore discarded and fetched again, once. The earlier behaviour refused
 * every later install of that package until someone deleted the file by hand. */
typedef enum { CACHE_FETCH, CACHE_USE, CACHE_DISCARD_AND_FETCH } cache_act;
cache_act pkg_cache_decision(int present, int hash_matches, int size_matches);
#endif
