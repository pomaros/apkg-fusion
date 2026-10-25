#ifndef PKG_RELATIONS_H
#define PKG_RELATIONS_H
#include "pkg.h"
#include "json.h"
#include <stddef.h>

/* Relations between packages, the pure half (docs/guide/metadata.md,
 * "Relations"): what a catalogue entry or a registry entry declares, and
 * the plan an installation needs. No AROS calls; relations_io.c reads the
 * registry and runs the plan.
 *
 *   depends     needed in the same root, same CPU and ABI, at least the
 *               version given; installed first, as "dependency"
 *   recommends  optional, with the benefit in words; never installed or
 *               required by apkg
 *   conflicts   never installed together; checked in both directions
 *   replaces    this package takes over an installed one: installed, then
 *               the old one removed as `apkg remove` removes (changed files
 *               kept). Not a provides: what depends on the old id is not
 *               satisfied by the new one. */

#define REL_MAX 16
#define REL_PLAN_MAX 24

typedef struct {
    char id[PKG_MAXID];
    char min_version[64];       /* "" when any version will do */
    char reason[160];           /* recommends: the benefit */
} rel_item;

typedef struct {
    rel_item depends[REL_MAX];    int ndepends;
    rel_item recommends[REL_MAX]; int nrecommends;
    rel_item conflicts[REL_MAX];  int nconflicts;
    rel_item replaces[REL_MAX];   int nreplaces;
    char unknown_feature[32];     /* a client feature this client lacks; "" when none */
} rel_decl;

/* The relations of object `obj` (a catalogue entry or a registry entry).
   0, or -1 with the reason when a declaration cannot be read: a package
   whose requirements cannot be read is refused, never installed without
   them. */
int  rel_read(const char *json, const js_tok *t, int ntok, int obj, rel_decl *out, char *why, size_t n);
/* `"depends": [...], "conflicts": [...], ...` for a registry entry, each
   preceded by ",\n  ". Nothing for empty lists. */
void rel_emit(js_out *o, const rel_decl *d, int with_recommends);
int  rel_any(const rel_decl *d);

/* What the registry knows of one installed package. `rel_known` is 0 for an
   entry written before 0.6, which recorded none: the caller then fills
   `rel` from the catalogue's entry for that build, if it has one. */
typedef struct {
    char id[PKG_MAXID];
    char version[64];
    long revision;
    char arch[16], abi[8];
    char reason[16];               /* "explicit" or "dependency" */
    rel_decl rel;
    int  rel_known;
} rel_inst;

enum { RELOP_DEP = 0, RELOP_TARGET, RELOP_REMOVE_REPLACED };

typedef struct {
    int  kind;
    char id[PKG_MAXID];
    char version[64];
    long revision;
    char note[200];               /* why it is in the plan */
} rel_op;

typedef struct {
    int  ok;                      /* 0: refused, `why` says why */
    char why[700];
    pkg_status status;            /* the refusal's status */
    rel_op ops[REL_PLAN_MAX];
    int  nops;
    int  extra;                   /* operations beyond installing the target */
} rel_plan;

/* The plan for installing `id` (upgrade 0) or for upgrading it to the
 * catalogue's build (upgrade 1: the target is installed; its new depends
 * must already be satisfied, as an upgrade installs nothing else). */
int rel_plan_install(const char *json, size_t len, const rel_inst *inst, int ninst,
                     const char *id, const char *my_arch, const char *my_abi,
                     int upgrade, rel_plan *out);

/* Installed packages whose depends name `id`, as "bishop (needs limpet >= 0.4.1)",
   comma separated. Returns how many. `except` (may be NULL) is left out. */
/* Whether a build of `id` at `version` with relations `d` may be what is
   installed here (a rollback): its depends installed and new enough, no
   conflict either way, and no installed dependent needing a newer one.
   0, or -1 with the reason. */
int rel_check_decl(const rel_decl *d, const char *id, const char *version, const rel_inst *inst, int ninst,
                   const char *my_arch, const char *my_abi, char *why, size_t wn);

int rel_required_by(const rel_inst *inst, int ninst, const char *id, const char *except,
                    char *out, size_t n);

/* One relation of a package, as show and the details panel describe it. */
typedef pkg_relation rel_row;
#define REL_DEPENDS    PKG_REL_DEPENDS
#define REL_RECOMMENDS PKG_REL_RECOMMENDS
#define REL_CONFLICTS  PKG_REL_CONFLICTS
#define REL_REPLACES   PKG_REL_REPLACES

int rel_rows(const char *json, size_t len, const rel_decl *d, const rel_inst *inst, int ninst,
             const char *my_arch, const char *my_abi, rel_row *rows, int max);
const char *rel_kind_word(int kind);   /* "depends", "recommends", ... */
#endif
