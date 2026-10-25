#ifndef PKG_ENTRIES_H
#define PKG_ENTRIES_H
#include "pkg.h"
#include "json.h"
#include <stddef.h>

/* Rows behind search, list and any front end. Pure: no AROS calls, so the host
 * tests link the same code the guest runs. The text that `apkg search` prints
 * is formatted FROM these rows, so a Zune window and the Shell cannot drift
 * apart -- there is one reading of the index, not two.
 */

/* The two ABIs a build can record. Anything else is refused rather than
 * guessed; see abi_check() in ops.c for why. */
int pkg_abi_known(const char *abi);

/* post_install_notes of an index element (or a registry object), joined by
 * '\n' into out. Returns the number of lines, 0 when there are none, or -1
 * when the member is not an array of plain lines within PKG_NOTES_LINES and
 * PKG_NOTES_LINE: then out is empty, never a shortened or odd reading. */
int notes_from_element(const char *json, const js_tok *t, int ntok, int el,
                       char *out, size_t n);

/* Rows from the index text. `term` filters as pkg_match does (NULL or empty
 * matches everything). Rows for another ABI are kept only with `show_all`;
 * otherwise they are counted in *hidden (if non-NULL) and dropped. Rows come
 * out sorted by id, case-insensitively, whatever order the file had. Returns
 * 0, or a JS_E* code (json.h): not a readable index, too large, or out of
 * memory. */
int entries_from_index(const char *json, size_t len, const char *term,
                       const char *my_abi, int show_all,
                       pkg_entries *out, int *hidden);

/* The same with the CPU checked too: a row is ours only when pkg_compat_of
 * says native. entries_from_index is this with no CPU (my_arch ""), as the
 * rows were before CPUs other than x86_64 reached the index. */
int entries_from_index_on(const char *json, size_t len, const char *term,
                          const char *my_arch, const char *my_abi, int show_all,
                          pkg_entries *out, int *hidden);

/* Which of the index's entries for `id` a command means. Every command that
 * picks an entry by id -- install, upgrade, dry run, requirements, show --
 * goes through this. The choice never depends on the order of the entries:
 * among several, the one with the smallest (arch, abi, url) is taken.
 *
 * Returns the element, or -1 when the index has no entry with this id, and
 * sets *how:
 *   VARIANT_ONE      exactly one entry is for my_arch/my_abi: that one
 *   VARIANT_NONE     none is: the element is the one to explain the refusal
 *                    with (or to describe, for show) -- never to install
 *   VARIANT_SEVERAL  more than one is: the element is the first of them;
 *                    an operation must refuse rather than pick
 * Pure. */
enum { VARIANT_ONE = 0, VARIANT_NONE, VARIANT_SEVERAL };
int index_select_variant(const char *json, const js_tok *t, int ntok, int arr,
                         const char *id, const char *my_arch, const char *my_abi,
                         int *how, int *n_same);

/* The catalogue's side of pkg_details for `id`: every variant the index has
 * is listed, and the one described is the first that runs here natively,
 * else the first. Returns 0, 1 when the index has no such id, -1 when the
 * text is not an index. Pure. */
int details_from_index(const char *json, size_t len, const char *id,
                       const char *my_arch, const char *my_abi, pkg_details *out);

/* One row from a registry file (<root>/db/installed/<id>.json). The registry
 * records no abi, summary or category -- those are the index's -- so the
 * caller fills what it knows. Returns 0, or -1 if the text is not an entry. */
int entry_from_registry(const char *json, size_t len, pkg_entry *out);

int  entries_push(pkg_entries *, const pkg_entry *);

/* first_added and app_first_added of an index element; "" when absent or
   not YYYY-MM-DD. news_date_ok: exactly that shape. */
void dates_from_element(const char *json, const js_tok *t, int ntok, int el, char first[11], char app[11]);
int  news_date_ok(const char *d);
/* Case-insensitive by id, as AROS compares names. */
void entries_sort(pkg_entries *);
#endif
