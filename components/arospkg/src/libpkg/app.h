#ifndef PKG_APP_H
#define PKG_APP_H
/* The application description: how an installed drawer is started.
 * docs/rfc/0002-application-folder.md is the format; this is its reader,
 * validator and writer. Pure logic -- no file system, no AROS calls -- so the
 * host tests exercise every rule (tests/test_app.c). Starting the program is
 * launch.c. */
#include "pkg.h"
#include "json.h"
#include "req.h"
#include <stddef.h>

/* The file inside the drawer. JSON rather than TOML: libpkg already reads
   JSON, so a runtime needs no second parser, and the author never writes it
   -- apkg-pack or apkg generates it from the manifest's [launch] table. */
#define PKG_APP_FILE    "application.json"
#define PKG_APP_SCHEMA  1
#define PKG_APP_MAXSIZE (64L * 1024)

/* Reads one `launch` object: from a catalogue entry, a registry entry or an
   application.json. Returns 1 and fills `l` when present and valid, 0 when
   absent, -1 when present but unusable (`why` says what is wrong). */
int  launch_from_object(const char *js, const js_tok *t, int ntok, int obj,
                        pkg_launch *l, char *why, size_t n);

/* The path rules of RFC 0002 section 6 for one relative path: not empty, not
   absolute, no ':', '\\', '..' component, empty component or control
   character, printable ASCII without '"' (the JSON writer does not escape).
   0 = acceptable. */
int  app_path_ok(const char *rel, char *why, size_t n);

/* The whole description's consistency: the paths, the mode, and the
   workbench rule that the program starts in its own drawer. 0 = valid. */
int  launch_valid(const pkg_launch *l, char *why, size_t n);

/* Writes `"launch": {...}` (no leading comma) for a registry entry. */
void launch_emit(js_out *o, const pkg_launch *l);

/* The application.json for an installed package, from what the catalogue
   said at installation. Caller frees. NULL when out of memory. */
char *app_text(const char *id, const char *version, long revision,
               const char *arch, const char *abi,
               const req_ent *reqs, int nreq, const pkg_launch *l);

/* What a program file's first bytes say it is built for: "x86_64", "i386",
   "aarch64", "arm", "m68k" (an Amiga hunk file), or "" when it is not a
   program this code recognises. */
const char *app_binary_arch(const unsigned char *head, size_t n);

/* Parses a whole application.json text into `a` (every field the runtime
   needs). 0 = valid; otherwise -1 and `why`. Requirements go to reqs[]. */
typedef struct {
    char id[PKG_MAXID];
    char name[64];
    char version[64];
    long revision;
    char arch[16], abi[8];      /* its first target */
    int  ntargets;
    pkg_launch launch;
} app_desc;
int  app_parse(const char *text, size_t len, app_desc *a,
               req_ent *reqs, int maxreq, int *nreq, char *why, size_t n);
#endif
