#ifndef PKG_INTERNAL_H
#define PKG_INTERNAL_H
#include "pkg.h"
#include "req.h"
#include "json.h"
#include <stddef.h>

#define MAXINV 2048

/* An index js_parse_alloc or the entries readers refused (a JS_E* code),
   with the cause the user can act on. */
pkg_status index_unreadable(pkg_err *e, int code, const char *path);

struct pkg_ctx {
    char root[PKG_MAXPATH];
    char db[PKG_MAXPATH], installed[PKG_MAXPATH], txns[PKG_MAXPATH];
    char cache[PKG_MAXPATH], tmp[PKG_MAXPATH], doctor[PKG_MAXPATH];
    char lock_owner[PKG_MAXPATH];
    char nonce[40];
    int  holds_lock;
    int  recovered, unresolved;
    int  pending;      /* uncommitted transactions seen at open */
    pkg_progress progress;
    void *progress_user;
    const char *progress_id;   /* the package an event is about, during install */
    pkg_removal removal;       /* what the last remove left in place */
    /* Relations: the reason the next install records ("explicit" or
       "dependency"), whether it runs inside a plan (pkg_install refuses on
       its own when the plan would need more than the one package), and the
       replacements recovery finished at open. */
    char install_reason[16];
    int  in_plan;
    char replaced_note[200];
};

typedef struct { char rel[256]; char sha[65]; long size; } inv_ent;

pkg_status pkg_fail(pkg_err *, pkg_status, const char *sum,
                    const char *det, const char *subj);

int  inv_of_tree(const char *base, const char *prefix, inv_ent *, int *n, int max);
int  inv_from_registry(const char *json, size_t len, inv_ent *, int max, int *n);
void inv_remove(const char *dir, inv_ent *, int n, int *removed, int *kept);
int  prune_empty(const char *dir);

int  txn_marker(struct pkg_ctx *, const char *id, const char *name);
int  txn_has   (struct pkg_ctx *, const char *id, const char *name);
int  txn_write_plan(struct pkg_ctx *, const char *id, const char *text);

void pkg_recover_all(struct pkg_ctx *);
extern int pkg_break_recovery_after;
extern int pkg_slow_ms;
extern pkg_fault pkg_injected_fault;
int  txn_marker_faulty(struct pkg_ctx *, const char *id, const char *name, int fail);
void pkgctx_resolved(struct pkg_ctx *);
void pkgctx_unresolved(struct pkg_ctx *);
int  leftovers_report(struct pkg_ctx *, const char *id, const char *dest,
                      inv_ent *, int ninv, int removed, int kept,
                      const char *kept_icon);

/* Shared between install (ops.c) and upgrade/rollback (swap.c). */
void  registry_path(struct pkg_ctx *, const char *id, char *out, size_t n);

/* news_io.c: the catalogue a refresh replaces, read before it is replaced,
   and the record written once the new one is in place. */
typedef struct {
    char *text; size_t len;          /* NULL: there was no catalogue */
    char when[24], day[11];          /* its date */
    char url[512];                   /* where the last refresh fetched it; "" unknown */
} news_prev;
void news_before_update(struct pkg_ctx *, news_prev *);
/* relations_io.c */
int  rel_fragment(const char *json, const js_tok *t, int ntok, int obj, char *out, size_t n, char *why, size_t wn);
pkg_status rel_check_alone(struct pkg_ctx *, const char *index_path, const char *id, int upgrade,
                           const char *arch, const char *abi, pkg_err *);
pkg_status rel_check_rollback(struct pkg_ctx *, const char *id, const char *version, const char *rel_json,
                              const char *arch, const char *abi, pkg_err *);
pkg_status rel_check_remove(struct pkg_ctx *, const char *id, pkg_err *);
void rel_finish_replacements(struct pkg_ctx *);
int  news_after_update(struct pkg_ctx *, const char *url, news_prev *);
void news_prev_free(news_prev *);
void details_catalogue(struct pkg_ctx *, pkg_details *);
void  txn_id(char out[64], const char *id);
void  now_iso(char out[32]);
int   prog(struct pkg_ctx *, const char *phase, unsigned long done,
           unsigned long total, int can_cancel);
pkg_status abi_check(const char *pkg_id, const char *want, const char *force, pkg_err *);
/* The CPU, then the ABI (abi_check): the whole target, before any download. */
pkg_status target_check(const char *pkg_id, const char *arch, const char *abi,
                        const char *force, pkg_err *);
char *registry_text(const char *id, const char *version, long revision,
                    const char *arch, const char *abi, const char *subdir,
                    const char *url, const char *sha, long size,
                    inv_ent *inv, int n, req_ent *reqs, int nreq,
                    const char *icon_state, const char *icon_sha, long icon_size,
                    const char *icon_member,
                    const char *prev_registry, const char *prev_archive,
                    const char *notes, const pkg_launch *launch,
                    const char *reason, const char *rel_json);
/* Write application.json into a staged tree from the catalogue's launch
   table, unless the archive brought its own (which then stands). Returns 0,
   or -1 when the file could not be written. */
int   app_stage(const char *staging, const char *id, const char *version, long revision,
                const char *arch, const char *abi, const req_ent *reqs, int nreq,
                const pkg_launch *l);
/* What upgrade, rollback, verify and a dry run need to know about a revision,
 * read from a registry entry or from an index entry (swap.c). */
/* depends, conflicts and replaces as the registry records them: 48 entries
   of at most 130 characters with their quoting. */
#define REL_JSON_MAX 8192
typedef struct {
    char version[64];
    long revision;
    char arch[16], abi[8];
    char reason[16];             /* registry: "explicit" or "dependency" */
    char rel_json[REL_JSON_MAX]; /* the relations, as the registry records them */
    int  rel_known;              /* registry: they were recorded (0.6 and later) */
    char url[512], sha[65];
    long size;
    char subdir[128];
    char icon_member[128];
    char icon_state[24], icon_sha[65];
    char notes[PKG_NOTES_MAX];   /* post_install_notes, '\n'-joined */
    pkg_launch launch;           /* how it is started; mode NONE when not said */
    req_ent app_reqs[MAXREQ];    /* requires_system, for application.json only */
    int  app_nreq;
    char prev_registry[PKG_MAXPATH], prev_archive[PKG_MAXPATH];
} rev_info;
int        swap_read_registry(const char *path, rev_info *, inv_ent *, int *ninv, char **text_out);
/* The entry an operation on `id` means for a target of `arch`/`abi` (see
 * index_select_variant): refused when several entries are for that target;
 * when none is, the entry returned is the one target_check will refuse. */
pkg_status swap_read_index(const char *index_path, const char *id,
                           const char *arch, const char *abi, rev_info *,
                           req_ent *reqs, int *nreq, pkg_err *);
void       swap_archive_ext(const char *url, char *ext, size_t n);
/* A managed arospkg replaced by a release's archive, kept icons and all;
   see swap.c. */
pkg_status pkg_selfupdate_swap(struct pkg_ctx *, const char *id, const char *archive, const char *version,
                               const char *url, const char *sha, long size, const char *subdir,
                               const char *expect_version, long expect_revision,
                               pkg_stop stop, pkg_err *);

/* Download `url` to `archive` unless a cached copy already matches; verify
 * size and sha256 before anything is published into the cache. */
pkg_status fetch_verified(struct pkg_ctx *, const char *id, const char *url,
                          const char *want_sha, long want_size,
                          const char *archive, pkg_err *);
#endif
