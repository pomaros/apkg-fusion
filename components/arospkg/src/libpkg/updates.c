/* Which installed packages the catalogue has something newer for. See
 * pkg_updates in pkg.h for what the answer promises and what it does not.
 *
 * Nothing here decides anything of its own: the entry is the one
 * index_select_variant() picks for the INSTALLED target (the one `apkg
 * upgrade` would take), and "newer" is upgrade_allowed(). A second reading
 * of versions would be a second opinion, and the day the two disagreed the
 * list would offer an upgrade that `apkg upgrade` then refuses. */
#include "updates.h"
#include "entries.h"
#include "json.h"
#include "req.h"
#include "upgrade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *pkg_upd_word(pkg_upd_state s)
{
    switch (s) {
    case PKG_UPD_AVAILABLE:       return "available";
    case PKG_UPD_BLOCKED:         return "blocked";
    case PKG_UPD_CURRENT:         return "current";
    case PKG_UPD_NEWER_INSTALLED: return "newer-installed";
    case PKG_UPD_INCOMPARABLE:    return "incomparable";
    case PKG_UPD_AMBIGUOUS:       return "ambiguous";
    case PKG_UPD_NOT_IN_INDEX:    return "not-in-catalogue";
    case PKG_UPD_OTHER_TARGET:    return "other-target";
    case PKG_UPD_NO_CATALOGUE:    return "no-catalogue";
    }
    return "unknown";
}

const char *pkg_cat_word(pkg_cat_state s)
{
    return s == PKG_CAT_OK ? "ok" : s == PKG_CAT_MISSING ? "missing" : "unreadable";
}

void pkg_updates_free(pkg_updates *u)
{
    if (!u) return;
    free(u->rows);
    u->rows = NULL; u->n = u->cap = 0;
}

const pkg_update_row *pkg_updates_find(const pkg_updates *u, const char *id)
{
    int i;
    for (i = 0; u && i < u->n; i++) if (strcmp(u->rows[i].id, id) == 0) return &u->rows[i];
    return NULL;
}

static int push(pkg_updates *u, const pkg_update_row *r)
{
    if (u->n == u->cap) {
        int cap = u->cap ? u->cap * 2 : 16;
        pkg_update_row *v = (pkg_update_row *)realloc(u->rows, (size_t)cap * sizeof *v);
        if (!v) return -1;
        u->rows = v; u->cap = cap;
    }
    u->rows[u->n++] = *r;
    return 0;
}

static void str_member(const char *js, const js_tok *t, int n, int el, const char *name,
                       char *out, size_t len)
{
    out[0] = 0;
    if (js_str(js, t, js_member(js, t, n, el, name), out, len) != 0) out[0] = 0;
}

/* "x86_64/v11, i386/v0": what the catalogue does have for an id. */
static void variants_of(const char *js, const js_tok *t, int n, int arr, const char *id,
                        char *out, size_t len)
{
    int i, el; size_t k = 0;
    out[0] = 0;
    for (i = 0; (el = js_elem(t, n, arr, i)) >= 0; i++) {
        char eid[PKG_MAXID], a[16], b[8];
        str_member(js, t, n, el, "id", eid, sizeof eid);
        if (strcmp(eid, id) != 0) continue;
        str_member(js, t, n, el, "arch", a, sizeof a);
        str_member(js, t, n, el, "abi", b, sizeof b);
        if (k + strlen(a) + strlen(b) + 4 >= len) break;
        k += (size_t)snprintf(out + k, len - k, "%s%s/%s", k ? ", " : "", a[0] ? a : "?", b[0] ? b : "?");
    }
}

/* A newer entry this machine could not take. The same gates `apkg upgrade`
 * applies before it downloads anything, in the same order: the target, the
 * requirements, the hash. Returns 1 with the reason when blocked. */
static int blocked(const char *js, js_tok *t, int n, int el,
                   const char *my_arch, const char *my_abi, const char *abi_force,
                   int probe, pkg_update_row *r)
{
    char arch[16], abi[8], sha[80], why[200];
    req_ent reqs[MAXREQ]; int nreq = 0, i, missing = 0, undet = 0;
    size_t k;

    str_member(js, t, n, el, "arch", arch, sizeof arch);
    str_member(js, t, n, el, "abi", abi, sizeof abi);
    if (pkg_compat_of(arch, abi, my_arch,
                      abi_force && *abi_force ? abi_force : my_abi, why, sizeof why) != PKG_COMPAT_NATIVE) {
        snprintf(r->why, sizeof r->why, "the newer build cannot run here: %s", why);
        return 1;
    }
    if (req_parse(js, t, n, el, reqs, MAXREQ, &nreq, why, sizeof why) != 0) {
        snprintf(r->why, sizeof r->why, "its system requirements are not readable: %s", why);
        snprintf(r->requirements, sizeof r->requirements, "undetermined");
        return 1;
    }
    if (!nreq) snprintf(r->requirements, sizeof r->requirements, "none");
    else if (!probe) snprintf(r->requirements, sizeof r->requirements, "not checked");
    else {
        req_probe_all(reqs, nreq);
        for (i = 0; i < nreq; i++) {
            if (reqs[i].state == REQ_MISSING) missing++;
            else if (reqs[i].state == REQ_UNDETERMINED) undet++;
        }
        snprintf(r->requirements, sizeof r->requirements, "%s",
                 missing ? "missing" : undet ? "undetermined" : "satisfied");
        if (missing) {
            k = (size_t)snprintf(r->why, sizeof r->why, "this system does not provide what the newer version needs:");
            for (i = 0; i < nreq && k + 2 < sizeof r->why; i++)
                if (reqs[i].state == REQ_MISSING)
                    k += (size_t)snprintf(r->why + k, sizeof r->why - k, " %s", reqs[i].id);
            return 1;
        }
    }
    str_member(js, t, n, el, "sha256", sha, sizeof sha);
    if (strlen(sha) != 64) {
        snprintf(r->why, sizeof r->why, "the catalogue has no hash for the newer version");
        return 1;
    }
    return 0;
}

static void one(const char *js, js_tok *t, int n, int arr, const upd_installed *in,
                const char *my_arch, const char *my_abi, const char *abi_force,
                int probe, pkg_update_row *r)
{
    /* The installed target: what `apkg upgrade` stays on. Entries written
       before the target was recorded are this client's own. */
    const char *want_arch = in->arch[0] ? in->arch : my_arch;
    const char *want_abi  = abi_force && *abi_force ? abi_force : (in->abi[0] ? in->abi : my_abi);
    int how = VARIANT_NONE, same = 0, el;
    char why[240];

    el = index_select_variant(js, t, n, arr, in->id, want_arch, want_abi, &how, &same);
    if (el < 0) {
        r->state = PKG_UPD_NOT_IN_INDEX;
        snprintf(r->why, sizeof r->why, "the catalogue does not list it");
        return;
    }
    if (how == VARIANT_NONE) {
        char v[160];
        variants_of(js, t, n, arr, in->id, v, sizeof v);
        r->state = PKG_UPD_OTHER_TARGET;
        snprintf(r->why, sizeof r->why, "the catalogue lists it only for %s, not for the installed %s/%s",
                 v, want_arch, want_abi);
        return;
    }
    if (how == VARIANT_SEVERAL) {
        r->state = PKG_UPD_AMBIGUOUS;
        snprintf(r->why, sizeof r->why, "the catalogue lists %d builds of it for %s/%s; "
                 "apkg refuses rather than picks one", same, want_arch, want_abi);
        return;
    }
    str_member(js, t, n, el, "version", r->available_version, sizeof r->available_version);
    r->available_revision = js_long(js, t, js_member(js, t, n, el, "revision"), 0);
    r->size = js_long(js, t, js_member(js, t, n, el, "size"), -1);

    if (upgrade_allowed(in->version, in->revision, r->available_version, r->available_revision,
                        why, sizeof why) == 0) {
        r->newer = 1;
        if (blocked(js, t, n, el, my_arch, my_abi, abi_force, probe, r)) r->state = PKG_UPD_BLOCKED;
        else r->state = PKG_UPD_AVAILABLE;
        return;
    }
    /* Refused: say which refusal, because "up to date" and "the catalogue is
       behind what you have" ask different things of the user. */
    if (strcmp(in->version, r->available_version) == 0)
        r->state = in->revision == r->available_revision ? PKG_UPD_CURRENT : PKG_UPD_NEWER_INSTALLED;
    else switch (version_compare(in->version, r->available_version)) {
        case 1:  r->state = PKG_UPD_NEWER_INSTALLED; break;
        default: r->state = PKG_UPD_INCOMPARABLE; break;
    }
    if (r->state == PKG_UPD_CURRENT) snprintf(r->why, sizeof r->why, "up to date");
    else snprintf(r->why, sizeof r->why, "%s", why);
}

int updates_compute(const char *json, size_t len,
                    const upd_installed *inst, int ninst,
                    const char *my_arch, const char *my_abi, const char *abi_force,
                    int probe, pkg_updates *out)
{
    js_tok *t = NULL; int n = -1, arr = -1, i;

    out->installed = ninst;
    out->newer = out->ready = out->blocked = 0;
    if (!json) out->catalogue = PKG_CAT_MISSING;
    else {
        n = js_parse_alloc(json, len, &t, PKG_JSON_MAXTOK);
        if (n > 0) arr = js_member(json, t, n, 0, "packages");
        if (n <= 0 || arr < 0 || t[arr].type != JS_ARR) {
            out->catalogue = PKG_CAT_UNREADABLE;
            if (!out->catalogue_error[0])
                snprintf(out->catalogue_error, sizeof out->catalogue_error, "%s",
                         n == JS_ENOMEM ? "not enough memory to read the catalogue" :
                         n == JS_ELIMIT ? "the catalogue is larger than this client reads" :
                         "the catalogue is not a valid index");
        } else out->catalogue = PKG_CAT_OK;
    }
    for (i = 0; i < ninst; i++) {
        pkg_update_row r;
        memset(&r, 0, sizeof r);
        snprintf(r.id, sizeof r.id, "%s", inst[i].id);
        snprintf(r.installed_version, sizeof r.installed_version, "%s", inst[i].version);
        r.installed_revision = inst[i].revision;
        snprintf(r.arch, sizeof r.arch, "%s", inst[i].arch[0] ? inst[i].arch : my_arch);
        snprintf(r.abi, sizeof r.abi, "%s", inst[i].abi[0] ? inst[i].abi : my_abi);
        r.size = -1;
        snprintf(r.requirements, sizeof r.requirements, "not checked");
        if (out->catalogue != PKG_CAT_OK) {
            r.state = PKG_UPD_NO_CATALOGUE;
            snprintf(r.why, sizeof r.why, "%s", out->catalogue == PKG_CAT_MISSING
                     ? "there is no catalogue on this machine; apkg update fetches it"
                     : "the catalogue cannot be read");
        } else one(json, t, n, arr, &inst[i], my_arch, my_abi, abi_force, probe, &r);
        if (r.state == PKG_UPD_AVAILABLE) { out->newer++; out->ready++; }
        else if (r.state == PKG_UPD_BLOCKED) { out->newer++; out->blocked++; }
        if (push(out, &r) != 0) { free(t); return -1; }
    }
    free(t);
    return 0;
}

/* ------------------------------------------------------------------ JSON */

/* "key": with a space, as the rest of apkg --json prints. */
static void jk(js_out *o, const char *k) { js_raw(o, "\""); js_raw(o, k); js_raw(o, "\": "); }

char *pkg_updates_json(const pkg_updates *u)
{
    size_t cap = 1024 + (size_t)u->n * 1024;
    char *buf = (char *)malloc(cap);
    js_out o; int i;
    if (!buf) return NULL;
    js_out_init(&o, buf, cap);
    js_raw(&o, "{ "); jk(&o, "schema"); js_vlong(&o, 1);
    js_raw(&o, ",\n  "); jk(&o, "catalogue"); js_raw(&o, "{ ");
    jk(&o, "state"); js_vstr(&o, pkg_cat_word(u->catalogue));
    js_raw(&o, ", "); jk(&o, "error"); js_vstr(&o, u->catalogue_error);
    js_raw(&o, ", "); jk(&o, "updated"); js_vstr(&o, u->catalogue_when);
    js_raw(&o, ", "); jk(&o, "updated_time"); js_vlong(&o, u->catalogue_time);
    js_raw(&o, ", "); jk(&o, "stale"); js_raw(&o, u->catalogue_stale ? "true" : "false");
    js_raw(&o, " }");
    js_raw(&o, ",\n  "); jk(&o, "installed"); js_vlong(&o, u->installed);
    js_raw(&o, ", "); jk(&o, "newer"); js_vlong(&o, u->newer);
    js_raw(&o, ", "); jk(&o, "ready"); js_vlong(&o, u->ready);
    js_raw(&o, ", "); jk(&o, "blocked"); js_vlong(&o, u->blocked);
    js_raw(&o, ", "); jk(&o, "pending_transactions"); js_vlong(&o, u->pending);
    js_raw(&o, ",\n  "); jk(&o, "packages"); js_raw(&o, "[");
    for (i = 0; i < u->n; i++) {
        const pkg_update_row *r = &u->rows[i];
        js_raw(&o, i ? ",\n    { " : "\n    { ");
        jk(&o, "id"); js_vstr(&o, r->id);
        js_raw(&o, ", "); jk(&o, "installed_version"); js_vstr(&o, r->installed_version);
        js_raw(&o, ", "); jk(&o, "installed_revision"); js_vlong(&o, r->installed_revision);
        js_raw(&o, ", "); jk(&o, "available_version"); js_vstr(&o, r->available_version);
        js_raw(&o, ", "); jk(&o, "available_revision"); js_vlong(&o, r->available_revision);
        js_raw(&o, ", "); jk(&o, "arch"); js_vstr(&o, r->arch);
        js_raw(&o, ", "); jk(&o, "abi"); js_vstr(&o, r->abi);
        js_raw(&o, ", "); jk(&o, "state"); js_vstr(&o, pkg_upd_word(r->state));
        js_raw(&o, ", "); jk(&o, "newer"); js_raw(&o, r->newer ? "true" : "false");
        js_raw(&o, ", "); jk(&o, "size"); js_vlong(&o, r->size);
        js_raw(&o, ", "); jk(&o, "requirements"); js_vstr(&o, r->requirements);
        js_raw(&o, ", "); jk(&o, "reason"); js_vstr(&o, r->why);
        js_raw(&o, " }");
    }
    js_raw(&o, u->n ? "\n  ]\n}\n" : "]\n}\n");
    if (o.fail) { free(buf); return NULL; }
    return buf;
}
