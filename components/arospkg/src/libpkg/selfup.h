/* Updating arospkg itself: apkg and the PkgManager that goes with it, to the
 * same release. `apkg self-update` and PkgManager's "Update arospkg" are two
 * fronts of this one engine; docs/self-update.md is the user's side.
 *
 * An installation is a home and its copies:
 *
 *   home    the drawer PkgManager lives in. Unpacked by hand from a release
 *           archive (SU_HOME_DRAWER), or installed by apkg as the package
 *           `arospkg` (SU_HOME_MANAGED: <root>/arospkg with a registry
 *           entry), or none at all for an apkg used on its own.
 *   copies  apkg copied elsewhere, usually C:apkg, as the release README
 *           says. A copy is tied to a home only by the record the user's
 *           own choice wrote (ENVARC:arospkg/copies.json), never by a
 *           search: two drawers on one machine are two installations.
 *
 * Which files change: in a drawer unpacked by hand, apkg, PkgManager,
 * BUILD.txt and application.json, each only where it already is (an
 * installation without PkgManager does not get one); icons and everything
 * else stay. A managed home is replaced as a package upgrade (swap.c), so
 * its registry, version and file sums agree afterwards; a user's icon is
 * kept there too. Copies are replaced file by file.
 *
 * The order is fixed so that an interruption can always be settled: every
 * new file is written beside its target as <file>.new and checked first;
 * then the copies and the drawer's files are renamed (<file> to <file>.old,
 * <file>.new to <file>), and a managed home's transaction comes last and
 * decides. A journal (ENVARC:arospkg/self-update.json) records the files
 * and their sums before the first rename; whoever finds it -- the next
 * self-update, or PkgManager at its start -- finishes or undoes the set
 * from what is on the disk, never from what the journal guesses.
 *
 * selfup.c holds the decisions, which the host tests run; selfup_io.c the
 * AROS side. */
#ifndef PKG_SELFUP_H
#define PKG_SELFUP_H
#include "pkg.h"
#include "release.h"
#include <stddef.h>

#define SU_MAXFILES 12
#define SU_MAXCAND  6
#define SU_PROTOCOL 1        /* the stage file's format; see su_apply_staged */

#define SU_COPIES_FILE  "ENVARC:arospkg/copies.json"
#define SU_JOURNAL_FILE "ENVARC:arospkg/self-update.json"
#define SU_RESULT_FILE  "ENV:arospkg/self-update-result.json"
#define SU_LOCK_FILE    "ENV:arospkg/self-update.lock"
/* A PkgManager announces its drawer under this public port name, so a
   self-update can tell whether one runs from the installation it is about
   to replace (su_running_pkgmanagers). */
#define SU_ANNOUNCE     "arospkg PkgManager"

typedef enum { SU_APKG = 0, SU_PKGMANAGER, SU_BUILD, SU_APPJSON } su_role;
typedef enum { SU_HOME_NONE = 0, SU_HOME_DRAWER, SU_HOME_MANAGED } su_home;
typedef enum { SU_KEEP = 0, SU_REPLACE } su_act;

typedef struct {
    su_role role;
    int  copy;                    /* a copy outside the home */
    char path[PKG_MAXPATH];       /* as DOS names it (NameFromLock) */
    char version[40];             /* programs: what the file says it is; "" unknown */
    char sha[65];                 /* now */
    char want_sha[65];            /* the release's, once staged */
    su_act act;
} su_file;

typedef struct {
    su_home home;
    char drawer[PKG_MAXPATH];     /* the home; "" when there is none */
    char root[PKG_MAXPATH];       /* SU_HOME_MANAGED: its package root */
    char managed_version[64];     /* SU_HOME_MANAGED: the registry's */
    long managed_revision;
    char started[PKG_MAXPATH];    /* the program that asked */
    su_file f[SU_MAXFILES];
    int  n;
    char note[400];               /* how the home was found, in the user's words */
} su_install;

/* ------------------------------------------------------------- pure */

/* The version a program file says it is: "$VER: <name> <version>", or for
   an apkg from before 0.7, which had no $VER, its "apkg <version>" string.
   Returns 0 with `out` filled, -1 when neither is there. */
int su_version_in(const char *data, size_t len, const char *name, char *out, size_t n);

/* js_str with the escapes js_vstr writes undone: paths and messages can
   hold quotes. */
#include "json.h"
int su_jstr(const char *js, const js_tok *t, int i, char *buf, size_t n);

/* The archive member a role is: "apkg", "PkgManager", ... */
const char *su_member(su_role);
const char *su_role_name(su_role);

/* Where a started program's installation is: the facts selfup_io.c reads
   from the disk, and the decision, which is pure. */
typedef struct {
    int  started_is_apkg;         /* 0: PkgManager asked */
    char started[PKG_MAXPATH];
    char dir[PKG_MAXPATH];        /* the started program's drawer */
    int  dir_managed;             /* <root>/arospkg with a registry entry */
    char dir_root[PKG_MAXPATH];   /*   ...and that root */
    int  dir_is_home;             /* holds PkgManager, or an arospkg BUILD.txt */
    int  recorded;                /* the record names `started` */
    char recorded_drawer[PKG_MAXPATH];   /* "" = recorded as used on its own */
    int  recorded_ok;             /* that drawer is still a home */
    int  recorded_managed;
    char recorded_root[PKG_MAXPATH];
    int  chose;                   /* --pkgmanager or --apkg-only was given */
    char chosen[PKG_MAXPATH];     /* the drawer given ("" = --apkg-only) */
    int  chosen_ok, chosen_managed;
    char chosen_root[PKG_MAXPATH];
    /* What a choice could be between: managed arospkg drawers in the root in
       use, and drawers running PkgManagers announce. */
    char cand[SU_MAXCAND][PKG_MAXPATH];
    char cand_how[SU_MAXCAND][80];
    int  ncand;
} su_facts;

typedef enum {
    SU_WHERE_HOME = 0,            /* started from the home itself */
    SU_WHERE_COPY,                /* a copy, tied to a home by the record or a choice */
    SU_WHERE_ALONE,               /* an apkg on its own: no PkgManager to update */
    SU_WHERE_CHOOSE,              /* a copy with candidates and no record: ask */
    SU_WHERE_BROKEN               /* the record or the choice names no home */
} su_where_kind;

typedef struct {
    su_where_kind kind;
    su_home home;
    char drawer[PKG_MAXPATH];
    char root[PKG_MAXPATH];
    int  record;                  /* the decision should be written to the record */
    char why[400];
} su_where;

void su_decide_where(const su_facts *, su_where *);

/* The files the update replaces, after staging: `want` holds the release's
   sum for each file (by index into in->f). Sets act; returns the count to
   replace, or -1 with the reason when a file is newer than the release
   (never a downgrade) -- `release` is its version. */
int su_plan(su_install *in, const char *release, char *why, size_t n);
/* A managed home whose registry is behind the release: its package is
   replaced even when its files already are the release's (an apkg
   replaced by an older self-update), so the record says what is there. */
int su_managed_behind(const su_install *in, const char *release);

/* --check, before anything is downloaded: per program file, "current",
   "older", "newer" or "unknown" against the release's version. */
const char *su_state_word(const su_file *, const char *release);
/* Every program already says the release's version, and a managed home's
   record does too: nothing to download. */
int su_all_current(const su_install *, const char *release);

/* The record of copies: JSON text in, the drawer recorded for `path` out.
   Returns 1 found (drawer may be "": used on its own), 0 not recorded. */
int   su_copies_lookup(const char *json, const char *path, char *drawer, size_t n);
/* The record with `path` set to `drawer`; caller frees. */
char *su_copies_set(const char *json, const char *path, const char *drawer);

/* The journal. One file per entry: where, the sums before and after. */
typedef struct {
    char path[PKG_MAXPATH];
    char from_sha[65], to_sha[65];
    /* what recovery found on the disk */
    int  now;                     /* SU_J_FROM, SU_J_TO, SU_J_ABSENT, SU_J_OTHER */
    int  have_new, have_old;      /* <path>.new is to_sha; <path>.old is from_sha */
} su_jfile;
enum { SU_J_ABSENT = 0, SU_J_FROM, SU_J_TO, SU_J_OTHER };

typedef struct {
    char from_version[40], to_version[40];
    char root[PKG_MAXPATH];       /* a managed home's root, "" for none */
    char managed_to[64];          /* the version its registry has once committed */
    su_jfile f[SU_MAXFILES];
    int  n;
} su_journal;

char *su_journal_text(const su_journal *);               /* caller frees */
int   su_journal_parse(const char *text, su_journal *);  /* 0, or -1 unreadable */

/* What recovery does with a journal and the disk. `managed` is -1 for no
   managed home, 0 when its registry is not at managed_to (its own
   transaction was rolled back or never ran), 1 when it is (committed).
   FORWARD: rename the .new files in; BACKWARD: put the .old ones back;
   DONE: nothing left; STUCK: neither is possible from what is there --
   `steps` then says file by file what to do by hand. */
enum { SU_REC_DONE = 0, SU_REC_FORWARD, SU_REC_BACKWARD, SU_REC_STUCK };
int su_recover_decide(const su_journal *, int managed, char *steps, size_t n);
/* What to do by hand, file by file, for a journal that cannot be settled. */
void su_manual_steps(const su_journal *, char *steps, size_t n);

/* ------------------------------------------------------------- AROS */

/* Read the installation `started` belongs to, the files and their versions
   now. `root` is the package root in use (for candidates); `chosen` is
   --pkgmanager's drawer, "" for --apkg-only, NULL when neither was given.
   PKG_OK with *where saying what was decided: on SU_WHERE_CHOOSE and
   SU_WHERE_BROKEN, `in` names no home and the caller asks or refuses. */
pkg_status su_locate(const char *started, int started_is_apkg, const char *root,
                     const char *chosen, su_install *in, su_where *where,
                     su_facts *facts, pkg_err *);

/* C:apkg when it is an apkg for this CPU and ABI, not `in`'s own file and
   not recorded at all: a copy the user should be asked about. Returns 1
   with its path and version. */
int su_unrecorded_c_apkg(const su_install *in, char *path, size_t n, char *version, size_t vn);
/* Add a copy to `in` (after the user said so). */
int su_add_copy(su_install *in, const char *path);
/* Write the decision for `copy` to the record: the home's drawer, or "". */
pkg_status su_record(const char *copy, const char *drawer, pkg_err *);

/* The latest stable release for this build, from `base` (NULL: GitHub's
   releases/latest/download/) or a local SHA256SUMS (`sums_file`, tests). */
pkg_status su_release(const char *base, const char *sums_file, pkg_release *, pkg_err *);

/* Download the release's archive, check it against SHA256SUMS, unpack the
   files `in` needs into `stage_dir` and check the programs against the
   archive's BUILD.txt and against this CPU and ABI; then fill want_sha and
   plan (su_plan). Also proves every target directory takes a new file and
   a rename. Progress through `cb` (phases "download", "verify"), which can
   cancel; nothing outside `stage_dir` is written. */
typedef struct {
    char dir[PKG_MAXPATH];
    char zip[PKG_MAXPATH];
    char url[600];
    char version[40], target[40], archive[160], sha[65];
    long size;
    int  nreplace;                /* files to replace */
    int  managed;                 /* and a managed home's package (su_managed_behind) */
} su_stage;
pkg_status su_stage_release(su_install *in, const pkg_release *, const char *base,
                            const char *stage_dir, pkg_progress cb, void *user,
                            su_stage *st, pkg_err *);

/* What would refuse the update later, asked before anything is downloaded
   or closed: an operation holding the root in use or a managed home's root,
   another self-update, or a PkgManager running from this installation
   (`from_window`: the asking PkgManager itself is not counted). */
pkg_status su_precheck(const su_install *in, const char *root, int from_window, pkg_err *);

/* Replace the files. Takes the self-update lock and the lock of the root
   in use (and of a managed home's root), refusing if an operation holds
   either; writes the journal; renames; settles. On any failure the
   previous set is put back. `fail` is a test hook (see su_test_fail). */
pkg_status su_apply(su_install *in, const su_stage *st, const char *root, pkg_err *);

/* Finish or undo an interrupted update, if the journal says there is one.
   Returns PKG_OK with *found 0 when there was nothing to do. On
   PKG_E_UNRESOLVED_TXN, `steps` says what to do by hand and the journal
   and every file are kept. */
pkg_status su_recover(int *found, char *steps, size_t n, pkg_err *);
int su_journal_pending(void);

/* PkgManagers running now: the drawers they announce (SU_ANNOUNCE), and
   how many run without announcing one (before 0.7). */
int su_running_pkgmanagers(char drawers[][PKG_MAXPATH], int max, int *unannounced);
/* A PkgManager announces its drawer while it runs; `nonce` names this run. */
void su_announce_start(const char *drawer, char *nonce, size_t n);
void su_announce_stop(void);

/* ------------------------------------------------- PkgManager's hand-off */

/* What PkgManager leaves for the apkg that finishes its update: the
   installation, the staged release, and how to start PkgManager again. */
typedef struct {
    int  wb;                      /* it was started from its icon */
    char program[PKG_MAXPATH];    /* the PkgManager to start again */
    char args[1024];              /* a Shell start: its arguments, quoted */
    unsigned long task;           /* the PkgManager process to wait for */
    unsigned long module;         /* started from a Shell: its seglist there,
                                     as the Shell process outlives it */
    char nonce[24];               /* its announcement */
    char root[PKG_MAXPATH];       /* the root it used */
} su_restart;

pkg_status su_handoff_save(const char *file, const su_install *, const su_stage *,
                           const su_restart *, pkg_err *);
pkg_status su_handoff_load(const char *file, su_install *, su_stage *, su_restart *, pkg_err *);
/* Wait until that PkgManager has closed and left: 0, or -1 after `seconds`. */
int su_wait_gone(const su_restart *, int seconds);
/* Start it again; PKG_E_LAUNCH with the reason when it did not start. */
pkg_status su_restart_pkgmanager(const su_restart *, pkg_err *);

/* The outcome, for the PkgManager that starts next (ENV:, this boot only). */
typedef struct {
    int  ok;                      /* the update is in place */
    int  changed;                 /* 0: nothing was changed */
    char from[40], to[40];
    char summary[200];
    char detail[700];
} su_result;
void su_result_write(const su_result *);
int  su_result_take(su_result *);  /* 1 when there was one; it is removed */

/* Testing only. "write:<n>": writing the n-th new file fails; "rename:<n>":
   the n-th rename fails; "stop:<n>": the process stops dead after the n-th
   rename, as if the machine had; "managed:<pkg_stop>": a managed home's
   transaction is interrupted there. */
void su_test_fail(const char *how);

/* The test release source of PkgManager and apkg: --self-update-from. */
void        su_set_base(const char *url);
const char *su_base(void);
#endif
