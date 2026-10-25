#ifndef PKG_NEWS_H
#define PKG_NEWS_H
#include "pkg.h"
#include <stddef.h>

/* The pure half of pkg_news (pkg.h): catalogue texts in, rows out. */

/* How many of the catalogue's recent additions a first catalogue lists. */
#define NEWS_FIRST_LIMIT 5

/* The catalogue `nw` against the one it replaces (`old`; NULL when there was
 * none). `prev_day` (YYYY-MM-DD, may be NULL) is the date of the replaced
 * catalogue: an application new to it but first published before that day
 * is marked returned. No old catalogue, or one that cannot be read: out->first
 * and the newest additions as PKG_NEWS_RECENT. Returns 0 or a JS_E* code
 * for `nw`. */
int news_compare(const char *old, size_t olen, const char *nw, size_t nlen,
                 const char *my_arch, const char *my_abi, const char *prev_day, pkg_news *out);

/* apkg recent over one catalogue. Returns 0 or a JS_E* code. */
int news_recent(const char *json, size_t len, const char *my_arch, const char *my_abi,
                const pkg_recent_opts *, pkg_news *out);

/* The record pkg_news_json(…, "last-update", 0) wrote, read back. */
int news_from_json(const char *json, size_t len, pkg_news *out);
#endif
