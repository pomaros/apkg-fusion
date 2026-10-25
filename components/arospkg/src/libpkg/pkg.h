/* libpkg -- the whole of arospkg's logic. `pkg` and `PkgManager` are fronts.
 *
 * This is the first implementation, deliberately smaller than section 3's API:
 * it covers the flow the design most needs verified -- local index, install one
 * package, registry, safe removal -- with the transaction journal and recovery
 * that section 2 specifies. Networking is not here yet; archives come from the
 * cache. What exists is meant to be correct, not complete.
 */
#ifndef PKG_H
#define PKG_H

#include <stddef.h>

#define PKG_MAXPATH 512

/* The catalogue apkg update fetches. index-v3.json is the generation of the
   file this client reads: every package, including those that need
   relations (client_features). index-v2.json beside it leaves those out,
   for apkg 0.4 and 0.5; index.json stays within what clients up to 0.3.2
   read (8192 values), for them. */
#define PKG_INDEX_URL \
    "https://raw.githubusercontent.com/tomaszstaniak/arospkg-index/main/index-v3.json"
#define PKG_MAXID    64

typedef enum {
    PKG_OK = 0,
    PKG_E_LOCKED,          /* another instance holds the lock            */
    PKG_E_STALE_LOCK,      /* holder is gone; pkg unlock is needed       */
    PKG_E_UNRESOLVED_TXN,  /* recovery stopped short; needs a human      */
    PKG_E_NOT_FOUND,
    PKG_E_EXISTS,
    PKG_E_VERIFY,          /* hash or size mismatch                      */
    PKG_E_CONFLICT,
    PKG_E_ARCHIVE,         /* malformed or unsafe archive                */
    PKG_E_IO,
    PKG_E_NOMEM,
    PKG_E_INTERRUPTED,     /* deliberate, from --interrupt-at            */
    PKG_E_NETWORK,         /* no stack, no trust store, or the fetch failed */
    /* Appended, not inserted: --expect in the test scripts names these by
     * number, so renumbering would silently re-point every existing test. */
    PKG_E_REQUIRES,        /* the MACHINE does not provide something needed */
    PKG_E_CANCELLED,       /* the caller's progress callback asked to stop  */
    PKG_E_LAUNCH,          /* the application cannot be started as described */
    PKG_E_REQUIRED         /* other installed packages depend on it          */
} pkg_status;

typedef struct {
    pkg_status status;
    char summary[160];     /* one line, already user-facing */
    char detail[320];
    char subject[PKG_MAXPATH];
} pkg_err;

typedef struct pkg_ctx pkg_ctx;

/* One row of the index or of the registry. Fixed-size fields on purpose: a
 * Zune list, a Shell table and a JSON printer all read the same struct with
 * no allocation of their own. The text `apkg search` prints is formatted from
 * these rows, so no front end can see a different index from another. */
typedef struct {
    char id[PKG_MAXID];
    char version[64];      /* upstream's string, compared only for equality */
    long revision;         /* our release of that version; 0 = not stated */
    char summary[160];
    char category[64];
    char arch[16];
    char abi[8];           /* "v1" or "v11"; empty when the index omits it */
    char kind[16];
    long size;             /* archive bytes from the index, or -1 */
    char requires[192];    /* requires_system ids, comma separated; "" if none */
    int  ours;             /* built for the ABI this binary runs on */
    int  installed;        /* a registry entry exists for this id */
    char installed_version[64];   /* from the registry, when installed */
    long installed_revision;      /* from the registry; 0 when it stated none */
} pkg_entry;

/* What an upgrade or a rollback would do, from the library's own planner --
 * a front end shows this, it never parses the Shell's printout. `allowed` is
 * the revision rule (same upstream version, higher revision) or, for a
 * rollback, whether the previous revision and its archive are still there;
 * `why` says so in the user's words when it is not. The counts are the plan;
 * `conflicts` > 0 means the operation would be refused, and `conflict_list`
 * names the files. */
typedef struct {
    char from_version[64]; long from_revision;
    char to_version[64];   long to_revision;
    int  allowed;
    char why[240];
    int  add, replace, remove, kept, unchanged, conflicts;
    char conflict_list[320];
    int  incomplete;       /* a dry run that may not fetch could not make the file plan; why says so */
} pkg_plan;

/* verify: the installed files against the registry, repairing nothing. */
typedef struct {
    int  ok, changed, missing, unknown;
    char icon[24];         /* "ours", "changed locally", "missing", "kept-existing", "none" */
} pkg_verify_result;

typedef struct { pkg_entry *v; int n, cap; } pkg_entries;   /* zero-init */

/* How an application is started: the `launch` table of its package
 * manifest (docs/rfc/0002-application-folder.md), carried by the catalogue,
 * recorded in the registry and written into the drawer as application.json.
 * Only what the program's icon cannot say: which file is the program, and
 * whether it is a Workbench or a Shell program. Stack and tool types stay in
 * the icon, which is authoritative for a Workbench start. */
typedef enum {
    PKG_LAUNCH_NONE = 0,       /* no description: Open folder is all there is */
    PKG_LAUNCH_WORKBENCH,      /* as a double-click on the program's icon does */
    PKG_LAUNCH_SHELL           /* as a Shell command, in the working directory */
} pkg_launch_mode;

typedef struct {
    pkg_launch_mode mode;
    char executable[256];      /* relative to the drawer */
    char workdir[256];         /* relative to the drawer; "." when not stated */
} pkg_launch;

/* Progress for a front end. One event per step: the phase -- "download",
 * "verify", "extract", "publish" -- the package it concerns, done/total in
 * bytes where known (total 0 when it is not), and whether cancelling is
 * still possible. `can_cancel` is stated, not left for the caller to infer
 * from the phase name: a Cancel button must grey out the moment the library
 * stops taking the request, or it looks broken.
 *
 * Setting *cancel stops the operation only while can_cancel is 1 -- during
 * the download and after verification, before any transaction exists. Once a
 * transaction has begun the events keep coming with can_cancel 0 and the
 * flag is ignored, because a half-done transaction is what the journal exists
 * to finish, and a front end has no business leaving one for the next run.
 *
 * The callback runs on the task that called pkg_install; it must return
 * quickly and must not touch a GUI directly -- hand the event to the GUI's
 * own loop. */
typedef struct {
    const char   *phase;
    const char   *id;
    unsigned long done, total;
    int           can_cancel;
} pkg_progress_ev;

typedef void (*pkg_progress)(void *user, const pkg_progress_ev *ev, int *cancel);

typedef enum {
    PKG_OPEN_READ,    /* no lock, no recovery, never mutates              */
    PKG_OPEN_WRITE,   /* lock, recover, clean tmp; refuses if unresolved  */
    PKG_OPEN_DOCTOR   /* lock and recover, but do NOT refuse and do NOT   *
                       * clean tmp: this is the mode that has to work     *
                       * precisely when the root is in a state the normal *
                       * path declines to touch                           */
} pkg_mode;

/* Injection points for the interruption test. The design owes crash injection
 * between a mutation and its marker; this is how that is reached from a test
 * without patching the library. */
typedef enum {
    PKG_STOP_NONE = 0,
    PKG_STOP_AFTER_PLAN,        /* plan written, nothing done            */
    PKG_STOP_AFTER_DIR,         /* package directory in place, no marker */
    PKG_STOP_AFTER_DIR_MARKER,
    PKG_STOP_AFTER_REGISTRY,    /* registry written, no commit marker    */
    PKG_STOP_BEFORE_COMMIT,
    PKG_STOP_AFTER_REMOVE_FILES,/* remove: files gone, registry intact   */
    PKG_STOP_IN_PUBLISH,        /* inside u_publish, between the delete   *
                                 * and the rename: the window Rename()'s  *
                                 * refusal to overwrite forces on us      */
    PKG_STOP_AFTER_ICON,        /* drawer icon published, no marker, no  *
                                 * registry: the one file outside the    *
                                 * package directory, on disk and unowned */
    /* Upgrade and rollback swap two trees; these are the two states
     * between the renames. Appended, as --interrupt-at names them by number. */
    PKG_STOP_AFTER_OLD_MOVED,   /* live tree moved aside, nothing published */
    PKG_STOP_AFTER_NEW_PUBLISHED,/* new tree in place, registry not written  */
    PKG_STOP_BEFORE_REPLACED    /* replacement installed, old one not yet  *
                                 * removed: what recovery must finish      */
} pkg_stop;

/* Fault injection for the paths that only run when a write fails. These are
 * not reachable by stopping early: the code has to actually try and fail. */
typedef enum {
    PKG_FAIL_NONE = 0,
    PKG_FAIL_COMMIT,     /* 005-commit cannot be written  */
    PKG_FAIL_DOCTOR      /* the leftovers report cannot be written */
} pkg_fault;

void pkg_test_fault(pkg_fault);

/* Testing only: slow things down so a person or a script can act mid-way.
 * ms per 16 KB received, and a pause of 20*ms after extraction begins --
 * the first gives a cancel test a window it can hit every time, the second
 * gives the non-cancellable phase a duration a close-during-it test needs.
 * 0 restores normal speed. */
void pkg_test_slow(int ms);
/* Testing only: in the next download whose answer is a 2xx, once that many
 * bytes of its body have arrived, the following receive fails in the TLS
 * adapter: as a reset socket (how = 1) or with corrupted bytes (how = 2).
 * It fires once, so a following attempt in the same process succeeds. A
 * negative byte count turns it off. */
void pkg_test_break_download(long body_bytes, int how);

/* Testing only: make recovery abort part-way through, so the next run has to
 * finish a rollback that was itself interrupted. Recovery claims to be
 * idempotent; this is how that claim is checked rather than asserted. */
void       pkg_test_break_recovery(int after_n_files);

pkg_status pkg_open (const char *root, pkg_mode, pkg_ctx **, pkg_err *);
void       pkg_close(pkg_ctx *);

/* Recovery runs inside pkg_open(PKG_OPEN_WRITE), before tmp/ is cleaned.
 * Reports how many transactions it resolved and how many it could not. */
void       pkg_recovery_counts(const pkg_ctx *, int *resolved, int *unresolved);

/* Uncommitted transactions seen when the root was opened read-only. A
 * read-only view must report these rather than showing their half-written
 * state as settled. */
int        pkg_pending_txns(const pkg_ctx *);

/* Identifies the build. The source hash is compiled in; it says nothing about
 * the toolchain, the flags or what was linked, which is why the provenance
 * below is recorded beside it and why the binary hashes ITSELF at run time. */
const char *pkg_build_id(void);
const char *pkg_toolchain(void);

/* The ABI this binary was built for: "v1" (mainline) or "v11" (distributions
 * such as AROS One).
 *
 * This is also the ABI of the machine it is running on, and not by assumption:
 * a binary built for one ABI does not start on the other, so the fact that this
 * code is executing at all is the evidence. ArosInquire() is a cross-check
 * rather than the source. */
const char *pkg_abi(void);

/* Testing and diagnosis only. --abi installs a package built for another ABI,
 * which will install correctly and then not start; --all-abi makes search list
 * those packages instead of hiding them. Neither is a user feature. */
void        pkg_set_abi_override(const char *);
const char *pkg_abi_override(void);
void        pkg_set_abi_show_all(int);

/* Testing only: require the certificate to match this name instead of the host
 * we connected to. The handshake must then FAIL. Without a test like this,
 * "the download worked" is not evidence that a wrong certificate would be
 * refused -- it only shows the right one was accepted. */
void        pkg_set_verify_name(const char *);
const char *pkg_verify_name(void);
int         pkg_abi_show_all(void);
const char *pkg_sdk(void);
const char *pkg_cc(void);

/* SHA-256 of the running executable, found through GetProgramName(). This is
 * the only identifier that covers the toolchain, the flags and the libraries
 * actually linked in. Returns 0 on success; on failure `out` is "unknown" and
 * a report must say so rather than fall back to the source hash. */
int pkg_self_sha256(char out[65]);

pkg_status pkg_install(pkg_ctx *, const char *index_path, const char *id,
                       pkg_stop, pkg_err *);
pkg_status pkg_remove (pkg_ctx *, const char *id, pkg_stop, pkg_err *);

/* Replace an installed package by the index's newer revision of the SAME
 * upstream version, or go back to the revision an upgrade replaced. Both
 * print their plan -- add, replace, remove, keep -- before touching the
 * drawer, refuse on any conflict with a file the user changed, and keep
 * every file the user changed or added. A rollback needs what the upgrade
 * saved: the previous registry entry and archive. Neither resolves
 * dependencies or touches anything outside the package's drawer and its
 * icon. See upgrade.h for the rules. */
pkg_status pkg_upgrade (pkg_ctx *, const char *index_path, const char *id,
                        pkg_stop, pkg_err *);
pkg_status pkg_rollback(pkg_ctx *, const char *id, pkg_stop, pkg_err *);

/* The plan without the operation. An upgrade preview fetches and stages the
 * target archive to compute it (with progress; cancellable), then discards
 * the staging; nothing in the drawer or the registry is touched. Returns
 * PKG_OK with the plan filled in, including the refusals (`allowed` 0, or
 * `conflicts` > 0), which are information here rather than errors. */
pkg_status pkg_upgrade_preview (pkg_ctx *, const char *index_path, const char *id,
                                int allow_fetch, pkg_plan *, pkg_err *);
pkg_status pkg_rollback_preview(pkg_ctx *, const char *id, pkg_plan *, pkg_err *);

/* The cheap questions a button needs answered from a row alone (upgrade:
 * the revision rule over the row's index and registry fields) or from the
 * registry (rollback: is there anything to go back to). 1 = yes; 0 = no,
 * with the reason in `why`. */
int pkg_can_upgrade (const pkg_entry *, char *why, size_t n);
int pkg_can_rollback(pkg_ctx *, const char *id, char *why, size_t n);

/* Compare an installed package with its registry entry: which files are as
 * installed, changed locally, missing, or in the drawer without being the
 * package's; and the drawer icon. Changes nothing. `listing` (caller frees)
 * is one line per file, "<state>  <path>", sorted. */
pkg_status pkg_verify(pkg_ctx *, const char *id, pkg_verify_result *, char **listing, pkg_err *);

/* Open the installed package's drawer on the desktop (desktop.c). The
 * drawer is the one its registry entry records; `where` gets its path
 * whenever it is known, also on failure, so a front end can say where it
 * should have been. PKG_E_NOT_FOUND: not installed, or the drawer is not
 * there; PKG_E_IO: no desktop, or it would not open it. Changes nothing. */
pkg_status pkg_open_folder(pkg_ctx *, const char *id, char *where, size_t n, pkg_err *);

/* ------------------------------------------------------------ starting */

/* Whether an installed application can be started, and how: `apkg run`,
 * PkgManager's Run button and ARexx RUN all ask this, so a button can never
 * offer what the command would refuse. Everything is checked on the disk
 * now -- the description, the program, its icon, its CPU and the libraries
 * it names -- and nothing is started or changed. */
typedef enum {
    PKG_RUN_READY = 0,         /* it can be started as described              */
    PKG_RUN_NO_DESCRIPTION,    /* the drawer says nothing about starting it    */
    PKG_RUN_INVALID,           /* the description is there but unusable        */
    PKG_RUN_MISSING,           /* the program, its icon or a directory is gone */
    PKG_RUN_INCOMPATIBLE,      /* built for another CPU or ABI                 */
    PKG_RUN_REQUIRES,          /* a library it needs is not on this machine    */
    PKG_RUN_NOT_INSTALLED      /* no registry entry, or no drawer              */
} pkg_run_state;

typedef struct {
    pkg_run_state state;
    char why[240];               /* the reason, in the user's words */
    char id[PKG_MAXID];
    char name[64];               /* what to call it: the description's name, or the id */
    char version[64];
    long revision;
    char drawer[PKG_MAXPATH];    /* the application drawer */
    char program[PKG_MAXPATH];   /* the program, full path ("" when unknown) */
    char workdir[PKG_MAXPATH];   /* the current directory it gets */
    pkg_launch launch;
    long stack;                  /* from the program's icon; 0 without one */
    char requirements[16];       /* "satisfied", "missing", "undetermined", "none" */
} pkg_app_info;

/* `what` is an installed package's id, or the path of an application drawer
 * (anything with ':' or '/'), which needs no registry and no context: the
 * description travels with the drawer. `c` may be NULL for a path.
 * PKG_OK whenever the question could be answered; the answer, including
 * "cannot be started", is in `info`. */
pkg_status pkg_app_info_get(pkg_ctx *c, const char *what, pkg_app_info *info, pkg_err *);
const char *pkg_run_word(pkg_run_state);   /* "ready", "no-description", ... */

/* Start it. Workbench mode asks workbench.library to start the program as
 * from its icon (icon stack and tool types; its own drawer as the current
 * directory), with `args` as Workbench arguments (files to open). Shell mode
 * runs it as a command in the working directory, with `args` quoted; with
 * PKG_RUN_WAIT it uses the caller's console and waits for it, otherwise it
 * gets a console window of its own and runs on.
 *
 * workbench.library loads the program on its own process and reports a
 * failure to nobody, so success is only claimed for what was seen: a new
 * process of the program's name (`confirmed`), or a Shell command's return
 * code. PKG_E_LAUNCH, with the reason, when it cannot be started. */
#define PKG_RUN_WAIT 1
typedef struct {
    pkg_launch_mode mode;
    int  confirmed;      /* workbench: a new process of that name appeared */
    int  waited;         /* shell + PKG_RUN_WAIT: rc is the command's       */
    long rc;
    char how[200];       /* what was done, in the user's words */
} pkg_run_result;
pkg_status pkg_run(pkg_ctx *c, const char *what, const char *const *args, int nargs,
                   int flags, pkg_run_result *, pkg_err *);

/* What the last pkg_remove left in place, for the front end to say: files
 * of the package that were changed locally (up to 10 named; all of them in
 * `list_file`), the drawer itself when anything is still in it, and a
 * drawer icon that was changed after installation. */
typedef struct {
    int  kept;                 /* changed files kept */
    int  named;                /* how many of them are in names[] */
    char names[10][256];
    int  others;               /* files in the drawer apkg did not install */
    int  others_named;
    char other_names[10][256];
    char dir[PKG_MAXPATH];     /* the drawer, when it is still there */
    char icon[PKG_MAXPATH];    /* a changed drawer icon that was kept */
    char list_file[PKG_MAXPATH];
} pkg_removal;
void pkg_last_removal(pkg_ctx *, pkg_removal *);

/* Diagnostic lines (URLs, redirect hosts, cache and index paths) are
   printed only when this is set; front ends show their own progress. */
void pkg_set_verbose(int);
int  pkg_verbose(void);
/* Diagnosis only: a download that fails verification is kept as
   <archive>.rejected instead of deleted. It is still refused. */
void pkg_set_keep_rejected(int);
int  pkg_keep_rejected(void);

/* What an operation would do, from the same planners the operation uses,
 * with nothing changed: not the drawer, not the registry, and no recovery of
 * pending transactions (a read-only context; pkg_pending_txns says whether
 * any exist, and the report repeats it). `op` is "install", "remove",
 * "upgrade" or "rollback". `allow_fetch` says whether an archive may be
 * downloaded INTO THE CACHE to complete the plan; without it, and without a
 * cached copy, the report says the plan is incomplete rather than guess.
 * `report` (caller frees) is the plan in the user's words. */
pkg_status pkg_dry_run(pkg_ctx *, const char *index_path, const char *op, const char *id,
                       int allow_fetch, char **report, pkg_err *);

/* Listing: ids of installed packages, newline separated, caller frees. */
/* Download the index to <root>/db/index.json. Networking is only ever entered
 * from here and from pkg_install; every other command works offline. After
 * the new catalogue is in place, what changed for this machine is recorded
 * in <root>/db/news.json (pkg_news_last reads it). */
pkg_status pkg_update(pkg_ctx *, const char *index_url, pkg_err *);

/* Substring match over id, summary and category in the local index. */
pkg_status pkg_search(pkg_ctx *, const char *index_path, const char *term,
                      char **out, pkg_err *);

/* The same query as rows. Rows for another ABI are counted in *hidden (if
 * given) and left out unless pkg_set_abi_show_all(1). `installed` is filled
 * from the registry. Free with pkg_entries_free. */
pkg_status pkg_query(pkg_ctx *, const char *index_path, const char *term,
                     pkg_entries *out, int *hidden, pkg_err *);

/* What is installed, as rows, sorted by id. With an index_path (may be NULL)
 * the summary, category and kind are filled in from the index, since the
 * registry does not record them. */
pkg_status pkg_installed(pkg_ctx *, const char *index_path,
                         pkg_entries *out, pkg_err *);
void       pkg_entries_free(pkg_entries *);

/* One line per system requirement of `id`, probed on THIS machine now:
 * "crt.library: satisfied (opens, version 3)". Undetermined is reported as
 * such, never as satisfied. A package with no requirements yields "". */
pkg_status pkg_requirements(pkg_ctx *, const char *index_path, const char *id,
                            char *out, size_t n, pkg_err *);

/* Whether a package is compatible with this machine: its declared CPU and
 * ABI against this client's. It is a judgement from the catalogue, not a
 * test that the program runs -- and the machine's requirements are a
 * separate question (pkg_details.requirements). A package's own target and
 * the way this machine could run it are two facts: running a 32-bit ABIv0
 * package through EmuV0 would be a third verdict of its own, never "native". */
typedef enum {
    PKG_COMPAT_NATIVE = 0,      /* built for this machine's CPU and ABI       */
    PKG_COMPAT_INCOMPATIBLE,    /* built for another CPU or ABI               */
    PKG_COMPAT_UNDETERMINED     /* the entry does not say enough to decide     */
} pkg_compat;

/* "native", "incompatible", "undetermined": the word CLI, window and ARexx
 * all show. */
const char *pkg_compat_word(pkg_compat);

/* The verdict for a package built for `arch`/`abi` on a machine that is
 * `my_arch`/`my_abi`, with the reason in the user's words. Pure. An empty
 * `my_arch` does not check the CPU. */
pkg_compat pkg_compat_of(const char *arch, const char *abi, const char *my_arch,
                     const char *my_abi, char *why, size_t n);

/* This build's CPU: "x86_64", "aarch64" or "i386". */
const char *pkg_arch(void);

/* Everything about one package that can be said without changing anything
 * or downloading anything: what the catalogue offers, whether it can run
 * here, and what is installed. `apkg show`, the window's detail panel and
 * ARexx INFO read this, so they cannot disagree. */
/* post_install_notes: at most PKG_NOTES_LINES lines of at most
 * PKG_NOTES_LINE printable ASCII characters each, without '"' or '\\'
 * (tools/mkindex.py refuses anything else). */
#define PKG_NOTES_LINES 8
#define PKG_NOTES_LINE  159
#define PKG_NOTES_MAX   (PKG_NOTES_LINES * (PKG_NOTES_LINE + 1) + 1)

#define PKG_TESTS_MAX 4

typedef struct {
    pkg_entry e;              /* the variant described; installed_* from the registry */
    int  in_index;            /* 0: only the registry knows it, or there is no index */
    int  index_missing;       /* 1: there is no index on this machine at all */
    char url[512];
    char sha256[65];
    char source[256];         /* where its source is published; "" when not stated */
    char license[64];         /* "" when not stated */
    char variants[160];       /* every target the index has for this id: "x86_64/v11, i386/v0" */
    int  nvariants;
    int  ambiguous;           /* >1: that many entries are for this machine; install refuses */
    pkg_compat compat;
    char compat_why[200];
    /* The machine's requirements, probed now, only for a compatible package:
     * "satisfied", "missing", "undetermined", "none" (it states none), or
     * "not checked". */
    char requirements[16];
    /* installed: */
    char installed_arch[16];
    char installed_abi[8];    /* "" in entries written before 0.3 */
    char installed_dir[PKG_MAXPATH];
    char installed_when[32];
    /* post_install_notes: what the package's author or the index maintainer
     * says a user should know after installing (game data to supply, a
     * folder to choose at first start). Plain lines joined by '\n'.
     * `notes` is the index's current text; `notes_recorded` the copy the
     * registry kept at installation, for offline reading. notes_bad is 1
     * when the index has notes this client will not show: not plain lines
     * within the limits below. */
    char notes[PKG_NOTES_MAX];
    char notes_recorded[PKG_NOTES_MAX];
    int  notes_bad;
    /* How the catalogue says it is started; mode NONE when it does not say,
       or says something this client cannot use (launch_bad 1). */
    pkg_launch launch;
    int  launch_bad;
    /* What has been checked about this build's archive, two separate facts
     * (docs/guide/metadata.md, "Listed, Reviewed, runtime tests"):
     * `review` is "reviewed" when the catalogue's automatic checks passed
     * for these very bytes, "listed" when it is published without that,
     * and "" when the catalogue says neither (one made before 0.6).
     * Runtime tests are a maintainer's record of installing and starting
     * these bytes on a named system; none means nobody recorded one, not
     * that it fails. */
    char review[12];
    int  review_suite;
    char review_date[11];
    char review_checked[96];       /* "metadata, download, archive, paths, layout, cpu" */
    char review_unchecked[64];     /* what the files cannot decide: "abi" */
    int  ntests;                   /* runtime tests for these bytes; the first PKG_TESTS_MAX described */
    char tests[PKG_TESTS_MAX][200];
    /* When the catalogue first published this build and the application
       (any build), as YYYY-MM-DD; "" when the catalogue does not say. */
    char first_added[11];
    char app_first_added[11];
    /* The address the catalogue in use was fetched from, as recorded by the
       last apkg update; "" when unknown (fetched by an apkg before 0.6). */
    char catalogue[512];
} pkg_details;

/* The review status and the dates of `d` as one line each, in the words
   apkg show and PkgManager both use. */
void pkg_review_text(const pkg_details *d, char *out, size_t n);
void pkg_added_text(const pkg_details *d, char *out, size_t n);

/* PKG_E_NOT_FOUND when neither the index nor the registry knows `id`. */
pkg_status pkg_details_get(pkg_ctx *, const char *index_path, const char *id,
                           pkg_details *, pkg_err *);

/* ---------------------------------------------------------------- updates */

/* Installed packages set against the catalogue: the one model behind
 * `apkg outdated`, the summary after `apkg update`, PkgManager's "Update
 * available" state and the ScreenBar widget, so no two of them can count
 * differently.
 *
 * It reads the catalogue and the registry and nothing else: no download, no
 * staging. A newer version is decided by upgrade_allowed() (upgrade.h), and
 * the catalogue entry by index_select_variant() for the installed CPU and
 * ABI, exactly as `apkg upgrade` decides them. Two questions are kept apart:
 * whether the catalogue has something newer (`newer`), and whether an
 * upgrade could be started now (PKG_UPD_AVAILABLE). Neither says the
 * upgrade will succeed: conflicts with files the user changed are found by
 * the preview, which needs the new archive. */
typedef enum {
    PKG_UPD_AVAILABLE = 0,   /* newer, and nothing known stops an upgrade      */
    PKG_UPD_BLOCKED,         /* newer, but this machine or the entry stops it   */
    PKG_UPD_CURRENT,         /* the catalogue offers what is installed          */
    PKG_UPD_NEWER_INSTALLED, /* installed is newer: never a silent downgrade    */
    PKG_UPD_INCOMPARABLE,    /* versions that cannot be ordered without guessing */
    PKG_UPD_AMBIGUOUS,       /* several entries for the installed target        */
    PKG_UPD_NOT_IN_INDEX,    /* the catalogue does not list the id               */
    PKG_UPD_OTHER_TARGET,    /* listed, but not for the installed CPU and ABI    */
    PKG_UPD_NO_CATALOGUE     /* no catalogue, or one that cannot be read        */
} pkg_upd_state;

typedef struct {
    char id[PKG_MAXID];
    char installed_version[64]; long installed_revision;
    char arch[16]; char abi[8];        /* the installed target */
    char available_version[64]; long available_revision;  /* "" when none */
    long size;                         /* download bytes of the catalogue entry, or -1 */
    pkg_upd_state state;
    int  newer;                        /* AVAILABLE or BLOCKED */
    char requirements[16];             /* "satisfied", "missing", "undetermined",
                                          "none", "not checked" */
    char why[240];                     /* the reason, in the user's words */
} pkg_update_row;

typedef enum { PKG_CAT_OK = 0, PKG_CAT_MISSING, PKG_CAT_UNREADABLE } pkg_cat_state;

/* After this many days without a successful `apkg update` the catalogue is
   called stale: an empty list then says little about what is current. */
#define PKG_CATALOGUE_STALE_DAYS 14

typedef struct {
    pkg_cat_state catalogue;
    char catalogue_error[160];
    /* The date of the catalogue file. apkg update replaces it only after a
       download that parsed, so this is the last successful refresh, read from
       the machine's clock (local time). 0 and "" when unknown. */
    long catalogue_time;               /* seconds since 1970, machine's local clock */
    char catalogue_when[24];           /* "2026-10-11 10:25" */
    int  catalogue_stale;
    int  installed;                    /* registry entries read */
    int  newer, ready, blocked;        /* counts over rows */
    int  pending;                      /* uncommitted transactions (pkg_updates_get only) */
    pkg_update_row *rows; int n, cap;  /* one per installed package, by id */
} pkg_updates;

/* `probe` 1 also probes requirements of the newer entries on this machine
 * (opens libraries; no network); 0 leaves them "not checked" and never
 * blocks on them. PKG_OK also when there is no catalogue: that is a state
 * of the answer (out->catalogue), not a failure of the question. */
pkg_status pkg_updates_get(pkg_ctx *, const char *index_path, int probe,
                           pkg_updates *out, pkg_err *);
/* The same without a context, for a reader that must not take the root's
 * lock or run recovery (the ScreenBar widget). Read-only. */
pkg_status pkg_updates_scan(const char *root, const char *index_path, int probe,
                            pkg_updates *out, pkg_err *);
void        pkg_updates_free(pkg_updates *);
const char *pkg_upd_word(pkg_upd_state);   /* "available", "blocked", ... */
const char *pkg_cat_word(pkg_cat_state);   /* "ok", "missing", "unreadable" */
/* The answer as one JSON object (caller frees), the form `apkg outdated
   --json` prints and integrations read. NULL when out of memory. */
char       *pkg_updates_json(const pkg_updates *);
/* The row for `id`, or NULL. */
const pkg_update_row *pkg_updates_find(const pkg_updates *, const char *id);

/* ------------------------------------------------------------------- news */

/* What is new in the catalogue for THIS machine (its CPU and ABI): the one
 * model behind the summary after `apkg update`, `apkg recent` and
 * PkgManager's "Recently added", so no two of them can count differently.
 *
 * Two kinds of date are kept apart. The catalogue's own: `first_added` (this
 * build) and `app_first_added` (the application, any build), the day the
 * catalogue first published them, YYYY-MM-DD in UTC, "" when it does not
 * say -- never filled in from a file date. And this machine's: what changed
 * between the catalogue it had and the one the last successful update
 * fetched, recorded in <root>/db/news.json by pkg_update. A failed or
 * cancelled update changes neither the catalogue nor that record. */
typedef enum {
    PKG_NEWS_APP = 0,       /* the catalogue had no entry with this id before   */
    PKG_NEWS_BUILD,         /* it had the application, not a build for here     */
    PKG_NEWS_UPDATED,       /* the build for here has another version/revision  */
    PKG_NEWS_RECENT         /* first catalogue on this machine: recent additions */
} pkg_news_kind;

typedef struct {
    pkg_news_kind kind;
    int  returned;                      /* APP: published before, removed, now back */
    int  installed;                     /* a registry entry exists for this id */
    char id[PKG_MAXID];
    char version[64];     long revision;
    char old_version[64]; long old_revision;   /* UPDATED: what it was */
    char arch[16]; char abi[8];
    char summary[160];
    char first_added[11];               /* "" unknown */
    char app_first_added[11];           /* "" unknown */
} pkg_news_row;

typedef struct {
    int  recorded;          /* pkg_news_last: a refresh was recorded at all       */
    int  current;           /* pkg_news_last: it describes the catalogue in use   */
    int  first;             /* nothing earlier to compare with; rows are RECENT   */
    char first_why[120];    /* why, in the user's words, when first               */
    char when[24];          /* the refresh, machine's local clock, "2026-10-14 10:25" */
    char previous[24];      /* the catalogue it was compared with, by its date    */
    char catalogue[512];    /* the address it was fetched from                    */
    char catalogue_sha256[65];  /* the catalogue file it describes               */
    int  apps, builds, updated, recent;   /* rows per kind */
    int  unknown_dates;     /* pkg_recent: applications for here with no date     */
    pkg_news_row *rows; int n, cap;
} pkg_news;

/* apkg recent. `limit` 0 = all; `since` "" = no lower bound, else
   YYYY-MM-DD compared with the catalogue's dates; `builds` also lists new
   builds for this machine of applications first published earlier. */
typedef struct { int limit; char since[11]; int builds; } pkg_recent_opts;

/* Applications for this machine, newest first by app_first_added (and, with
   builds, new builds by first_added). Unknown dates are counted in
   out->unknown_dates and never listed. PKG_E_NOT_FOUND without a catalogue. */
pkg_status pkg_recent(pkg_ctx *, const char *index_path, const pkg_recent_opts *,
                      pkg_news *out, pkg_err *);
/* What the last successful apkg update found. out->recorded 0 when none was
   recorded; out->current 0 when the catalogue in use is not the one it
   describes (replaced by other means since). PKG_OK in both cases. */
pkg_status pkg_news_last(pkg_ctx *, const char *index_path, pkg_news *out, pkg_err *);
void        pkg_news_free(pkg_news *);
const char *pkg_news_word(pkg_news_kind);   /* "new-application", "new-build", ... */
/* As one JSON object (caller frees); `mode` "recent" or "last-update".
   `limit` > 0 keeps at most that many rows of each kind. */
char       *pkg_news_json(const pkg_news *, const char *mode, int limit);
/* The machine's date `days` days ago, YYYY-MM-DD, for --days. */
void        pkg_days_ago(int days, char out[11]);

/* -------------------------------------------------------------- relations */

/* Relations between packages (docs/guide/metadata.md, "Relations"): what a
 * package needs (depends), suggests (recommends), refuses (conflicts) and
 * takes over (replaces). One model for apkg, PkgManager and ARexx.
 *
 * What apkg can deliver is decided by where it installs: every package in
 * its own drawer, <root>/<id>. A dependency is therefore another package
 * in the same root, for this CPU and ABI, that the program finds there;
 * a library or a command the system must provide stays a requires_system,
 * which apkg checks and never installs. */

/* One relation of a package, for show and the details panel. */
typedef struct {
    int  kind;                    /* PKG_REL_DEPENDS ... */
    char id[PKG_MAXID];
    char min_version[64];         /* "" when any version */
    char reason[160];             /* recommends: what it adds */
    int  listed;                  /* the catalogue has the id, for any system */
    int  compatible;              /* it has a build for this system, new enough */
    char available[64];           /* that build's version */
    int  installed;               /* registry entry in this root */
    char installed_version[64];
    int  satisfied;               /* installed, for this system, new enough */
    char state[200];              /* in the user's words */
} pkg_relation;
enum { PKG_REL_DEPENDS = 0, PKG_REL_RECOMMENDS, PKG_REL_CONFLICTS, PKG_REL_REPLACES,
       PKG_REL_NEEDED_BY      /* an installed package whose depends name this one */ };
#define PKG_REL_ROWS 64

/* Every relation `id` declares (its catalogue build for this system, or its
   registry entry when the catalogue does not list it), with its state here. */
pkg_status pkg_relations_get(pkg_ctx *, const char *index_path, const char *id,
                             pkg_relation *rows, int max, int *n, pkg_err *);
const char *pkg_rel_word(int kind);       /* "depends", "recommends", ... */

/* What `apkg install <id>` will do, before anything is downloaded: the
 * packages it needs installed first (oldest dependency first), the package,
 * and the installed packages it replaces, removed after it. Refusals --
 * a dependency missing from the catalogue or too old, a cycle, a conflict
 * either way, a replacement something else still needs -- are in `why`
 * with `ok` 0. Nothing is ever removed that the plan does not name. */
enum { PKG_OP_DEPENDENCY = 0, PKG_OP_INSTALL, PKG_OP_REMOVE_REPLACED, PKG_OP_MARK_EXPLICIT };
#define PKG_PLAN_OPS 24
typedef struct {
    int  kind;
    char id[PKG_MAXID];
    char version[64];
    long revision;
    char note[200];
    int  done;                    /* after pkg_install_plan_run: 1 done, 0 not reached */
    pkg_status status;            /* after the run: how it ended, when attempted */
    char error[200];
} pkg_plan_op;
typedef struct {
    int  ok;
    pkg_status status;            /* the refusal's status when ok is 0 */
    char why[700];
    pkg_plan_op ops[PKG_PLAN_OPS];
    int  nops;
    int  extra;                   /* operations beyond the one asked for: confirm first */
} pkg_install_plan;

pkg_status pkg_plan_install(pkg_ctx *, const char *index_path, const char *id,
                            pkg_install_plan *out, pkg_err *);
/* Run the plan, one transaction per package. A replacement is recorded
 * before it starts (db/replace.json), so an interruption between installing
 * the new package and removing the old one is finished by the next apkg
 * that opens the root for writing; the whole is NOT one transaction: after
 * a failure, what was done stays done and the plan says which steps ran. */
pkg_status pkg_install_plan_run(pkg_ctx *, const char *index_path, pkg_install_plan *,
                                pkg_stop stop, pkg_err *);
/* The plan as one JSON object (caller frees). */
char      *pkg_install_plan_json(const pkg_install_plan *);
/* Replacements finished by recovery when the root was opened: "hardcode by
   bishop", or "". */
const char *pkg_replacements_finished(const pkg_ctx *);

/* NULL clears it. See pkg_progress for what cancelling can and cannot do. */
void       pkg_set_progress(pkg_ctx *, pkg_progress, void *user);

pkg_status pkg_list(pkg_ctx *, char **out, pkg_err *);
pkg_status pkg_info(pkg_ctx *, const char *id, char **out, pkg_err *);

pkg_status pkg_unlock(const char *root, pkg_err *);

/* A read-only account of what is unresolved and why. Never repairs, never
 * deletes: `doctor --retry` re-runs recovery, and there is deliberately no
 * "force" that throws away state nobody has looked at. */
pkg_status pkg_doctor(pkg_ctx *, char **out, pkg_err *);

const char *pkg_strstatus(pkg_status);


/* Run `fn` as the program's main on a stack of at least 256 KB: a Shell's
 * default 40960 bytes is not enough for a TLS download. See bigstack.c. */
typedef int (*pkg_main_fn)(int argc, char **argv);
int pkg_run_with_stack(pkg_main_fn fn, int argc, char **argv);

#endif
