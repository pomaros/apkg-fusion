/* The pure half of selfup.h: what to update and how to settle an
 * interruption, decided from facts the AROS half reads. Nothing here
 * touches the disk, so tests/test_selfup.c runs all of it on the host. */
#include "selfup.h"
#include "json.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *su_member(su_role r)
{
    switch (r) {
    case SU_APKG:       return "apkg";
    case SU_PKGMANAGER: return "PkgManager";
    case SU_BUILD:      return "BUILD.txt";
    case SU_APPJSON:    return "application.json";
    }
    return "?";
}

const char *su_role_name(su_role r) { return su_member(r); }

int su_jstr(const char *js, const js_tok *t, int i, char *buf, size_t n)
{
    char *r, *w;
    if (js_str(js, t, i, buf, n) != 0) return -1;
    /* A value cut to the buffer would be another path: refused, empty. */
    if ((size_t)(t[i].end - t[i].start) >= n) { buf[0] = 0; return -1; }
    /* The escapes js_vstr writes; js_str leaves them as they are. */
    for (r = w = buf; *r; r++) {
        if (*r == '\\' && r[1]) { r++; *w++ = *r == 'n' ? '\n' : *r; }
        else *w++ = *r;
    }
    *w = 0;
    return 0;
}

static int version_char(char c)
{ return isalnum((unsigned char)c) || c == '.' || c == '-'; }

/* A version is digits first: "0.7.0", "0.7.0-test1". */
static int take_version(const char *p, const char *end, char *out, size_t n)
{
    size_t k = 0;
    if (p >= end || !isdigit((unsigned char)*p)) return -1;
    while (p < end && version_char(*p) && k + 1 < n) out[k++] = *p++;
    out[k] = 0;
    return k ? 0 : -1;
}

int su_version_in(const char *data, size_t len, const char *name, char *out, size_t n)
{
    char key[80];
    size_t kl, i;
    const char *end = data + len;
    out[0] = 0;
    /* Built at run time: a literal "$VER: " here would be the first one
       C:Version finds in the program. */
    snprintf(key, sizeof key, "%cVER: %s ", '$', name);
    kl = strlen(key);
    for (i = 0; i + kl < len; i++)
        if (data[i] == '$' && !memcmp(data + i, key, kl) && take_version(data + i + kl, end, out, n) == 0)
            return 0;
    /* Before 0.7 apkg carried no $VER, only the string it prints first:
       "apkg 0.6.0", a string of its own, with a zero on both sides. */
    if (strcmp(name, "apkg") != 0) return -1;
    snprintf(key, sizeof key, "%s ", name);
    kl = strlen(key);
    for (i = 1; i + kl < len; i++) {
        const char *p;
        if (data[i - 1] != 0 || memcmp(data + i, key, kl)) continue;
        p = data + i + kl;
        if (take_version(p, end, out, n) == 0 && p + strlen(out) < end && p[strlen(out)] == 0)
            return 0;
    }
    out[0] = 0;
    return -1;
}

/* ------------------------------------------------------------- where */

static void where_home(su_where *w, su_where_kind k, int managed, const char *drawer, const char *root)
{
    w->kind = k;
    w->home = managed ? SU_HOME_MANAGED : SU_HOME_DRAWER;
    snprintf(w->drawer, sizeof w->drawer, "%s", drawer);
    snprintf(w->root, sizeof w->root, "%s", managed ? root : "");
}

void su_decide_where(const su_facts *f, su_where *w)
{
    memset(w, 0, sizeof *w);
    if (f->dir_managed) {
        where_home(w, SU_WHERE_HOME, 1, f->dir, f->dir_root);
        snprintf(w->why, sizeof w->why, "installed by apkg as the package arospkg in %s", f->dir_root);
        return;
    }
    if (f->dir_is_home) {
        where_home(w, SU_WHERE_HOME, 0, f->dir, "");
        snprintf(w->why, sizeof w->why, "the drawer %s was started from", f->started_is_apkg ? "apkg" : "PkgManager");
        return;
    }
    /* A copy: tied to a home only by what the user said, now or before. */
    if (f->chose) {
        w->record = 1;
        if (!f->chosen[0]) {
            w->kind = SU_WHERE_ALONE;
            snprintf(w->why, sizeof w->why, "used on its own, as you said (--apkg-only)");
        } else if (f->chosen_ok) {
            where_home(w, SU_WHERE_COPY, f->chosen_managed, f->chosen, f->chosen_root);
            snprintf(w->why, sizeof w->why, "a copy of the installation in %s, as you said (--drawer)", f->chosen);
        } else {
            w->kind = SU_WHERE_BROKEN; w->record = 0;
            snprintf(w->why, sizeof w->why, "%s is not an arospkg drawer: it has no PkgManager and no "
                     "arospkg BUILD.txt", f->chosen);
        }
        return;
    }
    if (f->recorded) {
        if (!f->recorded_drawer[0]) {
            w->kind = SU_WHERE_ALONE;
            snprintf(w->why, sizeof w->why, "used on its own, as recorded when you chose it");
        } else if (f->recorded_ok) {
            where_home(w, SU_WHERE_COPY, f->recorded_managed, f->recorded_drawer, f->recorded_root);
            snprintf(w->why, sizeof w->why, "a copy of the installation in %s, as recorded when you chose it",
                     f->recorded_drawer);
        } else {
            w->kind = SU_WHERE_BROKEN;
            snprintf(w->why, sizeof w->why, "it was recorded as a copy of the installation in %s, which is "
                     "no longer an arospkg drawer. Name the drawer again with --drawer, or use --apkg-only",
                     f->recorded_drawer);
        }
        return;
    }
    if (f->ncand == 0) {
        w->kind = SU_WHERE_ALONE;
        snprintf(w->why, sizeof w->why, "no PkgManager is known to go with it. If you have one, name its "
                 "drawer with --drawer, or update from PkgManager's About window");
        return;
    }
    {
        size_t k = 0; int i;
        w->kind = SU_WHERE_CHOOSE;
        k += (size_t)snprintf(w->why, sizeof w->why, "it is a copy, and %s installation%s could be its own:",
                              f->ncand == 1 ? "this" : "these", f->ncand == 1 ? "" : "s");
        for (i = 0; i < f->ncand && k < sizeof w->why; i++)
            k += (size_t)snprintf(w->why + k, sizeof w->why - k, " %s (%s)%s", f->cand[i], f->cand_how[i],
                                  i + 1 < f->ncand ? "," : "");
    }
}

/* ------------------------------------------------------------- plan */

const char *su_state_word(const su_file *f, const char *release)
{
    int c;
    if (f->want_sha[0] && f->sha[0] && !strcmp(f->sha, f->want_sha)) return "current";
    if (!f->version[0]) return "unknown";
    c = pkg_release_vercmp(f->version, release);
    return c < 0 ? "older" : c == 0 ? "current" : "newer";
}

int su_all_current(const su_install *in, const char *release)
{
    int i, programs = 0;
    if (su_managed_behind(in, release)) return 0;
    for (i = 0; i < in->n; i++) {
        if (in->f[i].role != SU_APKG && in->f[i].role != SU_PKGMANAGER) continue;
        programs++;
        if (!in->f[i].version[0] || pkg_release_vercmp(in->f[i].version, release) != 0) return 0;
    }
    return programs > 0;
}

int su_plan(su_install *in, const char *release, char *why, size_t n)
{
    int i, count = 0;
    if (in->home == SU_HOME_MANAGED && in->managed_version[0]
        && pkg_release_vercmp(in->managed_version, release) > 0) {
        snprintf(why, n, "the package arospkg in %s is %s, newer than the release (%s); nothing is "
                 "changed", in->root, in->managed_version, release);
        return -1;
    }
    for (i = 0; i < in->n; i++) {
        su_file *f = &in->f[i];
        f->act = SU_KEEP;
        if (!f->want_sha[0] || !strcmp(f->sha, f->want_sha)) continue;
        if (!f->sha[0]) {
            /* Without its checksum it could never be put back. */
            snprintf(why, n, "%s cannot be read; nothing is changed", f->path);
            return -1;
        }
        /* A managed home at the release already: its files are the
           package's business (apkg verify), not an update's. */
        if (in->home == SU_HOME_MANAGED && !f->copy && in->managed_version[0]
            && pkg_release_vercmp(in->managed_version, release) >= 0) continue;
        if ((f->role == SU_APKG || f->role == SU_PKGMANAGER) && f->version[0]
            && pkg_release_vercmp(f->version, release) > 0) {
            snprintf(why, n, "%s is %s, newer than the release (%s); nothing is changed", f->path,
                     f->version, release);
            return -1;
        }
        f->act = SU_REPLACE;
        count++;
    }
    return count;
}

int su_managed_behind(const su_install *in, const char *release)
{
    return in->home == SU_HOME_MANAGED
           && (!in->managed_version[0] || pkg_release_vercmp(in->managed_version, release) < 0);
}

/* ------------------------------------------------------------- record */

/* The record is small: a list of {path, drawer}. Parsed into this. */
#define SU_MAXREC 32
typedef struct { char path[PKG_MAXPATH], drawer[PKG_MAXPATH]; } rec_ent;

static int rec_parse(const char *json, rec_ent *r, int max)
{
    js_tok *t = NULL;
    int n = 0, ntok, arr, el;
    if (!json || !*json) return 0;
    ntok = js_parse_alloc(json, strlen(json), &t, 4096);
    if (ntok <= 0) { free(t); return 0; }
    arr = js_member(json, t, ntok, 0, "copies");
    for (el = arr >= 0 ? js_child(t, ntok, arr) : -1; el >= 0 && n < max; el = js_sibling(t, ntok, el)) {
        r[n].path[0] = r[n].drawer[0] = 0;
        su_jstr(json, t, js_member(json, t, ntok, el, "path"), r[n].path, sizeof r[n].path);
        su_jstr(json, t, js_member(json, t, ntok, el, "drawer"), r[n].drawer, sizeof r[n].drawer);
        if (r[n].path[0]) n++;
    }
    free(t);
    return n;
}

int su_copies_lookup(const char *json, const char *path, char *drawer, size_t n)
{
    rec_ent r[SU_MAXREC];
    int k = rec_parse(json, r, SU_MAXREC), i;
    for (i = 0; i < k; i++)
        if (!strcasecmp(r[i].path, path)) { snprintf(drawer, n, "%s", r[i].drawer); return 1; }
    return 0;
}

char *su_copies_set(const char *json, const char *path, const char *drawer)
{
    rec_ent r[SU_MAXREC];
    int k = rec_parse(json, r, SU_MAXREC), i, found = 0;
    size_t cap = 256 + (size_t)(k + 1) * (2 * PKG_MAXPATH + 64);
    char *buf = (char *)malloc(cap);
    js_out o;
    if (!buf) return NULL;
    for (i = 0; i < k; i++)
        if (!strcasecmp(r[i].path, path)) { snprintf(r[i].drawer, sizeof r[i].drawer, "%s", drawer); found = 1; }
    if (!found && k < SU_MAXREC) {
        snprintf(r[k].path, sizeof r[k].path, "%s", path);
        snprintf(r[k].drawer, sizeof r[k].drawer, "%s", drawer);
        k++;
    }
    js_out_init(&o, buf, cap);
    js_raw(&o, "{\n  "); js_key(&o, "schema"); js_vlong(&o, 1);
    js_raw(&o, ",\n  "); js_key(&o, "copies"); js_raw(&o, " [");
    for (i = 0; i < k; i++) {
        js_raw(&o, i ? ",\n    { " : "\n    { ");
        js_key(&o, "path");   js_vstr(&o, r[i].path); js_raw(&o, ", ");
        js_key(&o, "drawer"); js_vstr(&o, r[i].drawer); js_raw(&o, " }");
    }
    js_raw(&o, "\n  ]\n}\n");
    if (o.fail) { free(buf); return NULL; }
    return buf;
}

/* ------------------------------------------------------------- journal */

char *su_journal_text(const su_journal *j)
{
    size_t cap = 1024 + (size_t)j->n * (PKG_MAXPATH + 200);
    char *buf = (char *)malloc(cap);
    js_out o;
    int i;
    if (!buf) return NULL;
    js_out_init(&o, buf, cap);
    js_raw(&o, "{\n  "); js_key(&o, "format"); js_vlong(&o, 1);
    js_raw(&o, ",\n  "); js_key(&o, "from"); js_vstr(&o, j->from_version);
    js_raw(&o, ",\n  "); js_key(&o, "to");   js_vstr(&o, j->to_version);
    js_raw(&o, ",\n  "); js_key(&o, "root"); js_vstr(&o, j->root);
    js_raw(&o, ",\n  "); js_key(&o, "managed_to"); js_vstr(&o, j->managed_to);
    js_raw(&o, ",\n  "); js_key(&o, "files"); js_raw(&o, " [");
    for (i = 0; i < j->n; i++) {
        js_raw(&o, i ? ",\n    { " : "\n    { ");
        js_key(&o, "path"); js_vstr(&o, j->f[i].path);     js_raw(&o, ", ");
        js_key(&o, "from"); js_vstr(&o, j->f[i].from_sha); js_raw(&o, ", ");
        js_key(&o, "to");   js_vstr(&o, j->f[i].to_sha);   js_raw(&o, " }");
    }
    js_raw(&o, "\n  ]\n}\n");
    if (o.fail) { free(buf); return NULL; }
    return buf;
}

int su_journal_parse(const char *text, su_journal *j)
{
    js_tok *t = NULL;
    int ntok, arr, el;
    memset(j, 0, sizeof *j);
    if (!text) return -1;
    ntok = js_parse_alloc(text, strlen(text), &t, 4096);
    if (ntok <= 0 || js_long(text, t, js_member(text, t, ntok, 0, "format"), 0) != 1) { free(t); return -1; }
    su_jstr(text, t, js_member(text, t, ntok, 0, "from"), j->from_version, sizeof j->from_version);
    su_jstr(text, t, js_member(text, t, ntok, 0, "to"),   j->to_version,   sizeof j->to_version);
    su_jstr(text, t, js_member(text, t, ntok, 0, "root"), j->root,         sizeof j->root);
    su_jstr(text, t, js_member(text, t, ntok, 0, "managed_to"), j->managed_to, sizeof j->managed_to);
    arr = js_member(text, t, ntok, 0, "files");
    for (el = arr >= 0 ? js_child(t, ntok, arr) : -1; el >= 0 && j->n < SU_MAXFILES; el = js_sibling(t, ntok, el)) {
        su_jfile *f = &j->f[j->n];
        su_jstr(text, t, js_member(text, t, ntok, el, "path"), f->path,     sizeof f->path);
        su_jstr(text, t, js_member(text, t, ntok, el, "from"), f->from_sha, sizeof f->from_sha);
        su_jstr(text, t, js_member(text, t, ntok, el, "to"),   f->to_sha,   sizeof f->to_sha);
        if (!f->path[0] || strlen(f->to_sha) != 64) { free(t); return -1; }
        j->n++;
    }
    free(t);
    return 0;
}

/* Can this file be taken forward (to the new version) or back? */
static int can_forward(const su_jfile *f) { return f->now == SU_J_TO || (f->now != SU_J_OTHER && f->have_new); }
static int can_back(const su_jfile *f)
{
    /* A file that never had a previous version (from "") is put back by
       removing it; there was nothing there before. */
    if (!f->from_sha[0]) return f->now != SU_J_OTHER;
    return f->now == SU_J_FROM || (f->now != SU_J_OTHER && f->have_old);
}

static void manual(const su_journal *j, char *steps, size_t n)
{
    size_t k = 0; int i;
    steps[0] = 0;
    for (i = 0; i < j->n && k < n; i++) {
        const su_jfile *f = &j->f[i];
        if (f->now == SU_J_FROM || f->now == SU_J_TO) continue;
        if (f->now == SU_J_OTHER)
            k += (size_t)snprintf(steps + k, n - k, "%s is neither the old nor the new version; "
                                  "keep it or replace it from the release archive.\n", f->path);
        else if (f->have_old)
            k += (size_t)snprintf(steps + k, n - k, "%s is missing: Rename \"%s.old\" \"%s\"\n", f->path, f->path, f->path);
        else if (f->have_new)
            k += (size_t)snprintf(steps + k, n - k, "%s is missing: Rename \"%s.new\" \"%s\"\n", f->path, f->path, f->path);
        else
            k += (size_t)snprintf(steps + k, n - k, "%s is missing: copy it from the release archive.\n", f->path);
    }
}

void su_manual_steps(const su_journal *j, char *steps, size_t n)
{
    manual(j, steps, n);
    if (!steps[0]) snprintf(steps, n, "Every file is either the old or the new version; compare them with "
                            "apkg --version and the release archive.\n");
}

int su_recover_decide(const su_journal *j, int managed, char *steps, size_t n)
{
    int i, all_to = 1, all_from = 1, fwd = 1, back = 1;
    for (i = 0; i < j->n; i++) {
        const su_jfile *f = &j->f[i];
        if (f->now != SU_J_TO) all_to = 0;
        if (f->now != SU_J_FROM && !(f->now == SU_J_ABSENT && !f->from_sha[0])) all_from = 0;
        if (!can_forward(f)) fwd = 0;
        if (!can_back(f)) back = 0;
    }
    steps[0] = 0;
    /* A managed home's own transaction has already been settled by its
       root's recovery when this runs; it decides the direction. */
    if (managed == 1) {
        if (all_to) return SU_REC_DONE;
        if (fwd) return SU_REC_FORWARD;
    } else if (managed == 0) {
        if (all_from) return SU_REC_DONE;
        if (back) return SU_REC_BACKWARD;
    } else {
        if (all_to || all_from) return SU_REC_DONE;
        /* The update was asked for and every new file is still there and
           checked: finish it. Otherwise go back. */
        if (fwd) return SU_REC_FORWARD;
        if (back) return SU_REC_BACKWARD;
    }
    manual(j, steps, n);
    return SU_REC_STUCK;
}
