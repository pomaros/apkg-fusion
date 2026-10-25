#ifndef PKG_JSON_H
#define PKG_JSON_H
#include <stddef.h>

/* A tokenising JSON reader, sized for what libpkg reads: the generated index
 * and its own registry files. Not a general parser -- it does not unescape
 * beyond the few escapes our writer emits, because the only producers are
 * tools/mkindex.py and this library. */

typedef enum { JS_UNDEF = 0, JS_OBJ, JS_ARR, JS_STR, JS_PRIM } js_type;

/* next: the token after this one and everything nested in it, so walking
   siblings does not rescan their contents. */
typedef struct { js_type type; int start, end, size, parent, next; } js_tok;

/* The most values libpkg will take from one file. Not a format limit: it only
   stops a hostile or broken file from asking for unbounded memory. The public
   index measured 17.4 bytes per value (2026-10-04), so the 16 MiB download
   ceiling holds about 1M values; tokens are 24 bytes, so this is 24 MiB at
   worst, and about 6 MiB for 5000 packages. */
#define PKG_JSON_MAXTOK (1 << 20)

/* js_parse_alloc's failures, kept apart because the user's remedy differs. */
#define JS_EINVAL  (-1)   /* not JSON this reader accepts */
#define JS_ELIMIT  (-2)   /* more values than `limit` */
#define JS_ENOMEM  (-3)   /* the tokens did not fit in memory */

/* Returns token count, or -1. With toks NULL it only counts. */
int  js_parse(const char *json, size_t len, js_tok *toks, int max);
/* Counts, allocates exactly that many tokens and parses into them. Returns
   the count and *out (free() it), or a JS_E* code with *out NULL. */
int  js_parse_alloc(const char *json, size_t len, js_tok **out, int limit);
/* First child of a container, and the next sibling: -1 when there is none. */
int  js_child(const js_tok *t, int ntok, int c);
int  js_sibling(const js_tok *t, int ntok, int i);

/* Index of the value of member `name` in object token `obj`, or -1. */
int  js_member(const char *json, const js_tok *t, int ntok, int obj, const char *name);
/* Index of element `i` of array token `arr`, or -1. */
int  js_elem(const js_tok *t, int ntok, int arr, int i);
/* Copy a string/primitive token into buf. Returns 0 on success. */
int  js_str(const char *json, const js_tok *t, int i, char *buf, size_t n);
long js_long(const char *json, const js_tok *t, int i, long dflt);

/* Writer: just enough to emit our registry and plan files. */
typedef struct { char *buf; size_t len, cap; int fail; } js_out;
void js_out_init(js_out *, char *buf, size_t cap);
void js_raw (js_out *, const char *);
void js_key (js_out *, const char *);
void js_vstr(js_out *, const char *);
void js_vlong(js_out *, long);
#endif
