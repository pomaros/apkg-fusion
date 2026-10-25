/* apkg self-update; see selfupdate.c and docs/self-update.md. */
#ifndef APKG_SELFUPDATE_H
#define APKG_SELFUPDATE_H
typedef struct {
    int check;              /* --check: report only */
    int recover;            /* --recover: settle an unfinished update only */
    int protocol;           /* --protocol: say which hand-off this apkg reads */
    const char *drawer;     /* --drawer <d>; "" for --apkg-only; NULL for neither */
    const char *apply;      /* --apply <file>: finish what PkgManager staged */
} su_opts;
/* current: this apkg's version ("0.7.0"); root: the package root in use.
   Returns a Shell return code. */
int apkg_self_update(const char *current, const char *root, const su_opts *o);
int apkg_self_update_apply(const char *handoff);
/* Testing only: another release base URL (https, ending in "/"), and a
   local SHA256SUMS instead of the release's. Failure injection is
   su_test_fail in libpkg/selfup.h. */
void su_test_from(const char *url);
void su_test_sums(const char *file);
#endif
