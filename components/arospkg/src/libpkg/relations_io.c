/* The AROS half of relations: the registry's view of what is installed,
 * the checks install, upgrade and remove make, and the run of an install
 * plan with its replacement record. The decisions are relations.c's. */
#include "internal.h"
#include "relations.h"
#include "entries.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rel_fragment(const char *json, const js_tok *t, int ntok, int obj, char *out, size_t n,
                 char *why, size_t wn)
{
    rel_decl *d = (rel_decl *)malloc(sizeof *d);
    js_out o;
    out[0] = 0;
    if (!d) { snprintf(why, wn, "out of memory"); return -1; }
    if (rel_read(json, t, ntok, obj, d, why, wn) != 0) { free(d); return -1; }
    if (d->unknown_feature[0]) {
        snprintf(why, wn, "it needs a newer apkg (it uses \"%s\"); update apkg with apkg self-update",
                 d->unknown_feature);
        free(d);
        return -1;
    }
    js_out_init(&o, out, n);
    /* The registry keeps what removal and conflict checks need; the
       recommendations are the catalogue's to show. */
    rel_emit(&o, d, 0);
    free(d);
    if (o.fail) { out[0] = 0; snprintf(why, wn, "too many relations to record"); return -1; }
    return 0;
}

static int same_id(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

static void member(const char *js, const js_tok *t, int n, int obj, const char *name, char *out, size_t len)
{
    out[0] = 0;
    if (js_str(js, t, js_member(js, t, n, obj, name), out, len) != 0) out[0] = 0;
}

/* Every registry entry of the root, with its relations: recorded ones, or
   for an entry from before 0.6 the catalogue's for that build (`idx`, may
   be NULL). Caller frees *out. */
static int load_installed(struct pkg_ctx *c, const char *idx, size_t idxlen, rel_inst **out, int *nout)
{
    char **names = NULL; int nnames = 0, i, n = 0;
    rel_inst *v;
    js_tok *it = NULL; int intok = -1, iarr = -1;
    *out = NULL; *nout = 0;
    if (u_listdir(c->installed, &names, &nnames) != 0 || nnames == 0) { if (names) u_freelist(names, nnames); return 0; }
    v = (rel_inst *)calloc((size_t)nnames, sizeof *v);
    if (!v) { u_freelist(names, nnames); return -1; }
    if (idx && (intok = js_parse_alloc(idx, idxlen, &it, PKG_JSON_MAXTOK)) > 0) iarr = js_member(idx, it, intok, 0, "packages");
    for (i = 0; i < nnames; i++) {
        char p[PKG_MAXPATH], *txt, why[200]; size_t len, l = strlen(names[i]); js_tok *t; int nt;
        rel_inst *r = &v[n];
        if (l < 6 || strcmp(names[i] + l - 5, ".json") != 0) continue;
        snprintf(p, sizeof p, "%s/%s", c->installed, names[i]);
        if (!(txt = u_read_all(p, &len))) continue;
        nt = js_parse_alloc(txt, len, &t, PKG_JSON_MAXTOK);
        if (nt > 0 && t[0].type == JS_OBJ) {
            member(txt, t, nt, 0, "name", r->id, sizeof r->id);
            member(txt, t, nt, 0, "version", r->version, sizeof r->version);
            member(txt, t, nt, 0, "arch", r->arch, sizeof r->arch);
            member(txt, t, nt, 0, "abi", r->abi, sizeof r->abi);
            member(txt, t, nt, 0, "reason", r->reason, sizeof r->reason);
            r->revision = js_long(txt, t, js_member(txt, t, nt, 0, "revision"), 0);
            if (!r->abi[0]) snprintf(r->abi, sizeof r->abi, "%s", pkg_abi());
            r->rel_known = js_member(txt, t, nt, 0, "relations_schema") >= 0;
            if (r->rel_known && rel_read(txt, t, nt, 0, &r->rel, why, sizeof why) != 0) r->rel_known = 0;
            if (!r->rel_known && iarr >= 0) {
                int how, same, el = index_select_variant(idx, it, intok, iarr, r->id, r->arch, r->abi, &how, &same);
                if (el >= 0 && how == VARIANT_ONE && rel_read(idx, it, intok, el, &r->rel, why, sizeof why) != 0)
                    memset(&r->rel, 0, sizeof r->rel);
            }
            if (r->id[0]) n++;
        }
        if (nt > 0) free(t);
        free(txt);
    }
    if (intok > 0) free(it);
    u_freelist(names, nnames);
    *out = v; *nout = n;
    return 0;
}

static const char *my_abi(void)
{
    const char *o = pkg_abi_override();
    return o && *o ? o : pkg_abi();
}

/* The plan from the catalogue file and the registry. */
static pkg_status plan_for(struct pkg_ctx *c, const char *index_path, const char *id, int upgrade,
                           const char *arch, const char *abi,
                           rel_plan *rp, rel_inst **inst, int *ninst, pkg_err *e)
{
    size_t len; char *idx = u_read_all(index_path, &len);
    int rc;
    if (!idx) return pkg_fail(e, PKG_E_NOT_FOUND, "no catalogue -- run apkg update first", "", index_path);
    if (load_installed(c, idx, len, inst, ninst) != 0) { free(idx); return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id); }
    rc = rel_plan_install(idx, len, *inst, *ninst, id, arch && *arch ? arch : pkg_arch(),
                          abi && *abi ? abi : my_abi(), upgrade, rp);
    free(idx);
    if (rc != 0 && rp->ok) return index_unreadable(e, rc, index_path);
    return PKG_OK;
}

pkg_status rel_check_alone(struct pkg_ctx *c, const char *index_path, const char *id, int upgrade,
                           const char *arch, const char *abi, pkg_err *e)
{
    rel_plan *rp = (rel_plan *)malloc(sizeof *rp);
    rel_inst *inst = NULL; int ninst = 0, i;
    pkg_status st;
    if (!rp) return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id);
    st = plan_for(c, index_path, id, upgrade, arch, abi, rp, &inst, &ninst, e);
    if (st == PKG_OK && !rp->ok)
        st = pkg_fail(e, rp->status ? rp->status : PKG_E_REQUIRES,
                      rp->status == PKG_E_CONFLICT ? "a conflict with another package" : "a requirement is not met",
                      rp->why, id);
    else if (st == PKG_OK && rp->nops > 1) {
        char what[400] = ""; size_t k = 0;
        for (i = 0; i < rp->nops; i++)
            if (rp->ops[i].kind != RELOP_TARGET && k + 80 < sizeof what)
                k += (size_t)snprintf(what + k, sizeof what - k, "%s%s %s", k ? ", " : "",
                                      rp->ops[i].kind == RELOP_DEP ? "install" : "remove", rp->ops[i].id);
        st = pkg_fail(e, PKG_E_REQUIRES, upgrade ? "this upgrade would need other packages changed"
                                                 : "this package needs other packages installed or removed",
                      upgrade ? what : "apkg install shows the plan and asks before doing it", what);
    }
    free(rp); free(inst);
    return st;
}

pkg_status rel_check_remove(struct pkg_ctx *c, const char *id, pkg_err *e)
{
    rel_inst *inst = NULL; int ninst = 0, n, rc;
    char who[400], why[500], ip[PKG_MAXPATH], *idx; size_t len = 0;
    /* Entries from before 0.6 recorded no relations: the catalogue on this
       machine says what they need. */
    snprintf(ip, sizeof ip, "%s/index.json", c->db);
    idx = u_read_all(ip, &len);
    rc = load_installed(c, idx, len, &inst, &ninst);
    free(idx);
    if (rc != 0) return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id);
    n = rel_required_by(inst, ninst, id, NULL, who, sizeof who);
    free(inst);
    if (!n) return PKG_OK;
    snprintf(why, sizeof why, "Installed package%s that need%s it: %s. Remove %s first; nothing was changed.",
             n == 1 ? "" : "s", n == 1 ? "s" : "", who, n == 1 ? "that one" : "those");
    return pkg_fail(e, PKG_E_REQUIRED, "other installed packages need it", why, id);
}

/* ------------------------------------------------------------- the plan */

pkg_status pkg_plan_install(pkg_ctx *c, const char *index_path, const char *id,
                            pkg_install_plan *out, pkg_err *e)
{
    rel_plan *rp;
    rel_inst *inst = NULL; int ninst = 0, i;
    pkg_status st;
    char regpath[PKG_MAXPATH];

    memset(out, 0, sizeof *out);
    registry_path(c, id, regpath, sizeof regpath);
    if (u_exists(regpath)) {
        /* Asked for by name, it is the user's from now on: removing what
           depended on it must not take it along. */
        if (load_installed(c, NULL, 0, &inst, &ninst) != 0) return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id);
        for (i = 0; i < ninst; i++)
            if (same_id(inst[i].id, id) && !strcmp(inst[i].reason, "dependency")) {
                out->ok = 1; out->nops = 1;
                out->ops[0].kind = PKG_OP_MARK_EXPLICIT;
                snprintf(out->ops[0].id, sizeof out->ops[0].id, "%s", id);
                snprintf(out->ops[0].version, sizeof out->ops[0].version, "%s", inst[i].version);
                snprintf(out->ops[0].note, sizeof out->ops[0].note, "installed as a dependency; recorded as installed by you");
                free(inst);
                return PKG_OK;
            }
        free(inst);
        return pkg_fail(e, PKG_E_EXISTS, "already installed", "Remove it first if you want to install it again.", id);
    }
    if (!(rp = (rel_plan *)malloc(sizeof *rp))) return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id);
    st = plan_for(c, index_path, id, 0, NULL, NULL, rp, &inst, &ninst, e);
    if (st == PKG_OK) {
        out->ok = rp->ok;
        out->status = rp->status;
        snprintf(out->why, sizeof out->why, "%s", rp->why);
        for (i = 0; rp->ok && i < rp->nops && i < PKG_PLAN_OPS; i++) {
            pkg_plan_op *o = &out->ops[out->nops++];
            o->kind = rp->ops[i].kind == RELOP_DEP ? PKG_OP_DEPENDENCY :
                      rp->ops[i].kind == RELOP_TARGET ? PKG_OP_INSTALL : PKG_OP_REMOVE_REPLACED;
            snprintf(o->id, sizeof o->id, "%s", rp->ops[i].id);
            snprintf(o->version, sizeof o->version, "%s", rp->ops[i].version);
            o->revision = rp->ops[i].revision;
            snprintf(o->note, sizeof o->note, "%s", rp->ops[i].note);
        }
        out->extra = out->nops ? out->nops - 1 : 0;
    }
    free(rp); free(inst);
    return st;
}

/* db/replace.json: the replacement under way, written before the new
   package is installed and deleted when the old ones are gone. */
static void replace_path(struct pkg_ctx *c, char *out, size_t n) { snprintf(out, n, "%s/replace.json", c->db); }

static int write_replace(struct pkg_ctx *c, const pkg_install_plan *pl)
{
    char path[PKG_MAXPATH], buf[2048]; js_out o; int i, first = 1;
    js_out_init(&o, buf, sizeof buf);
    js_raw(&o, "{ "); js_key(&o, "schema"); js_vlong(&o, 1);
    for (i = 0; i < pl->nops; i++)
        if (pl->ops[i].kind == PKG_OP_INSTALL) { js_raw(&o, ", "); js_key(&o, "new"); js_vstr(&o, pl->ops[i].id); }
    js_raw(&o, ", "); js_key(&o, "old"); js_raw(&o, "[");
    for (i = 0; i < pl->nops; i++)
        if (pl->ops[i].kind == PKG_OP_REMOVE_REPLACED) { js_raw(&o, first ? "" : ", "); js_vstr(&o, pl->ops[i].id); first = 0; }
    js_raw(&o, "] }\n");
    if (o.fail) return -1;
    replace_path(c, path, sizeof path);
    return u_write_atomicish(path, buf);
}

static int mark_explicit(struct pkg_ctx *c, const char *id)
{
    /* The registry's "reason" member, found through the reader, its value
       replaced in place: the rest of the entry stays byte for byte. */
    char p[PKG_MAXPATH], *txt, *n2; size_t len; js_tok *t; int nt, v, rc = -1;
    registry_path(c, id, p, sizeof p);
    if (!(txt = u_read_all(p, &len))) return -1;
    nt = js_parse_alloc(txt, len, &t, PKG_JSON_MAXTOK);
    v = nt > 0 ? js_member(txt, t, nt, 0, "reason") : -1;
    if (v >= 0 && t[v].type == JS_STR && t[v].end - t[v].start == 10 && !strncmp(txt + t[v].start, "dependency", 10)
        && (n2 = (char *)malloc(len + 1)) != NULL) {
        memcpy(n2, txt, (size_t)t[v].start);
        memcpy(n2 + t[v].start, "explicit", 8);
        memcpy(n2 + t[v].start + 8, txt + t[v].end, len - (size_t)t[v].end);
        n2[len - 2] = 0;
        rc = u_write_atomicish(p, n2);
        free(n2);
    }
    if (nt > 0) free(t);
    free(txt);
    return rc;
}

pkg_status pkg_install_plan_run(pkg_ctx *c, const char *index_path, pkg_install_plan *pl,
                                pkg_stop stop, pkg_err *e)
{
    int i, replacing = 0;
    pkg_status st = PKG_OK;
    char path[PKG_MAXPATH];

    if (!pl->ok) return pkg_fail(e, pl->status ? pl->status : PKG_E_REQUIRES, "the plan was refused", pl->why, "");
    for (i = 0; i < pl->nops; i++) if (pl->ops[i].kind == PKG_OP_REMOVE_REPLACED) replacing = 1;
    replace_path(c, path, sizeof path);
    if (replacing && u_exists(path))
        return pkg_fail(e, PKG_E_CONFLICT, "an earlier replacement is not finished",
                        "Its old package could not be removed. Look at it with apkg doctor; nothing was changed.", path);
    if (replacing && write_replace(c, pl) != 0)
        return pkg_fail(e, PKG_E_IO, "cannot record the replacement", "Nothing was changed.", c->db);
    c->in_plan = 1;
    for (i = 0; i < pl->nops; i++) {
        pkg_plan_op *o = &pl->ops[i];
        pkg_err oe; memset(&oe, 0, sizeof oe);
        if (o->kind == PKG_OP_MARK_EXPLICIT) {
            o->status = mark_explicit(c, o->id) == 0 ? PKG_OK : PKG_E_IO;
        } else if (o->kind == PKG_OP_REMOVE_REPLACED) {
            if (stop == PKG_STOP_BEFORE_REPLACED) {
                c->in_plan = 0;
                return pkg_fail(e, PKG_E_INTERRUPTED, "stopped on purpose", "PKG_STOP_BEFORE_REPLACED", o->id);
            }
            o->status = pkg_remove(c, o->id, PKG_STOP_NONE, &oe);
        } else {
            snprintf(c->install_reason, sizeof c->install_reason, "%s",
                     o->kind == PKG_OP_DEPENDENCY ? "dependency" : "explicit");
            o->status = pkg_install(c, index_path, o->id, o->kind == PKG_OP_INSTALL ? stop : PKG_STOP_NONE, &oe);
            c->install_reason[0] = 0;
        }
        o->done = 1;
        if (o->status != PKG_OK) {
            snprintf(o->error, sizeof o->error, "%s%s%s", oe.summary, oe.detail[0] ? ": " : "", oe.detail);
            *e = oe;
            st = o->status;
            break;
        }
    }
    c->in_plan = 0;
    /* The record goes once the old packages are gone, or when the new one
       never got installed: then there is nothing to finish. */
    if (replacing) {
        int new_done = 0, old_left = 0;
        for (i = 0; i < pl->nops; i++) {
            if (pl->ops[i].kind == PKG_OP_INSTALL && pl->ops[i].done && pl->ops[i].status == PKG_OK) new_done = 1;
            if (pl->ops[i].kind == PKG_OP_REMOVE_REPLACED && !(pl->ops[i].done && pl->ops[i].status == PKG_OK)) old_left = 1;
        }
        if (!new_done || !old_left) { replace_path(c, path, sizeof path); u_delete(path); }
    }
    return st;
}

/* At open, for writing: a replacement the last run did not finish. */
void rel_finish_replacements(struct pkg_ctx *c)
{
    char path[PKG_MAXPATH], *txt, newid[PKG_MAXID] = "", reg[PKG_MAXPATH];
    size_t len, used = 0; js_tok *t; int n, arr, k, el, left = 0;
    c->replaced_note[0] = 0;
    replace_path(c, path, sizeof path);
    if (!(txt = u_read_all(path, &len))) return;
    n = js_parse_alloc(txt, len, &t, PKG_JSON_MAXTOK);
    if (n > 0) {
        member(txt, t, n, 0, "new", newid, sizeof newid);
        registry_path(c, newid, reg, sizeof reg);
        arr = js_member(txt, t, n, 0, "old");
        if (newid[0] && u_exists(reg))
            for (k = 0; arr >= 0 && (el = js_elem(t, n, arr, k)) >= 0; k++) {
                char old[PKG_MAXID], oreg[PKG_MAXPATH]; pkg_err e;
                if (js_str(txt, t, el, old, sizeof old) != 0) continue;
                registry_path(c, old, oreg, sizeof oreg);
                if (!u_exists(oreg)) continue;
                memset(&e, 0, sizeof e);
                if (pkg_remove(c, old, PKG_STOP_NONE, &e) == PKG_OK) {
                    if (used + strlen(old) + strlen(newid) + 8 < sizeof c->replaced_note)
                        used += (size_t)snprintf(c->replaced_note + used, sizeof c->replaced_note - used,
                                                 "%s%s by %s", used ? ", " : "", old, newid);
                } else {
                    left = 1;
                    if (used + 60 < sizeof c->replaced_note)
                        used += (size_t)snprintf(c->replaced_note + used, sizeof c->replaced_note - used,
                                                 "%s%s by %s NOT finished: %.80s", used ? ", " : "", old, newid, e.summary);
                }
            }
        free(t);
    }
    free(txt);
    if (!left) u_delete(path);
}

const char *pkg_replacements_finished(const pkg_ctx *c) { return c->replaced_note; }

char *pkg_install_plan_json(const pkg_install_plan *pl)
{
    size_t cap = 2048 + (size_t)pl->nops * 700;
    char *buf = (char *)malloc(cap);
    js_out o; int i;
    static const char *const kinds[] = { "install-dependency", "install", "remove-replaced", "mark-explicit" };
    if (!buf) return NULL;
    js_out_init(&o, buf, cap);
    js_raw(&o, "{ "); js_key(&o, "schema"); js_vlong(&o, 1);
    js_raw(&o, ", "); js_key(&o, "ok"); js_raw(&o, pl->ok ? "true" : "false");
    js_raw(&o, ", "); js_key(&o, "refusal"); js_vstr(&o, pl->ok ? "" : pl->why);
    js_raw(&o, ",\n  "); js_key(&o, "steps"); js_raw(&o, "[");
    for (i = 0; i < pl->nops; i++) {
        const pkg_plan_op *s = &pl->ops[i];
        js_raw(&o, i ? ",\n    { " : "\n    { ");
        js_key(&o, "action"); js_vstr(&o, kinds[s->kind]);
        js_raw(&o, ", "); js_key(&o, "id"); js_vstr(&o, s->id);
        js_raw(&o, ", "); js_key(&o, "version"); js_vstr(&o, s->version);
        js_raw(&o, ", "); js_key(&o, "revision"); js_vlong(&o, s->revision);
        js_raw(&o, ", "); js_key(&o, "note"); js_vstr(&o, s->note);
        js_raw(&o, ", "); js_key(&o, "done"); js_raw(&o, s->done ? "true" : "false");
        js_raw(&o, ", "); js_key(&o, "result"); js_vstr(&o, s->done ? pkg_strstatus(s->status) : "");
        js_raw(&o, ", "); js_key(&o, "error"); js_vstr(&o, s->error);
        js_raw(&o, " }");
    }
    js_raw(&o, pl->nops ? "\n  ]\n}\n" : "]\n}\n");
    if (o.fail) { free(buf); return NULL; }
    return buf;
}

/* ------------------------------------------------------------- describing */

pkg_status pkg_relations_get(pkg_ctx *c, const char *index_path, const char *id,
                             pkg_relation *rows, int max, int *nrows, pkg_err *e)
{
    size_t len = 0; char *idx = u_read_all(index_path, &len);
    rel_inst *inst = NULL; int ninst = 0, i;
    rel_decl *d = (rel_decl *)calloc(1, sizeof *d);
    *nrows = 0;
    if (!d) { free(idx); return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id); }
    if (load_installed(c, idx, len, &inst, &ninst) != 0) { free(idx); free(d); return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id); }
    {
        /* The catalogue's build for this system; else what the registry recorded. */
        int found = 0;
        if (idx) {
            js_tok *t; int nt = js_parse_alloc(idx, len, &t, PKG_JSON_MAXTOK);
            if (nt > 0) {
                int how, same, el = index_select_variant(idx, t, nt, js_member(idx, t, nt, 0, "packages"),
                                                        id, pkg_arch(), my_abi(), &how, &same);
                char why[200];
                if (el >= 0 && rel_read(idx, t, nt, el, d, why, sizeof why) == 0) found = 1;
                free(t);
            }
        }
        for (i = 0; !found && i < ninst; i++) if (same_id(inst[i].id, id)) { *d = inst[i].rel; found = 1; }
    }
    *nrows = rel_rows(idx, len, d, inst, ninst, pkg_arch(), my_abi(), rows, max);
    /* And the other way: what installed here needs this one. */
    for (i = 0; i < ninst && *nrows < max; i++) {
        int j;
        for (j = 0; j < inst[i].rel.ndepends; j++)
            if (same_id(inst[i].rel.depends[j].id, id)) {
                pkg_relation *r = &rows[(*nrows)++];
                memset(r, 0, sizeof *r);
                r->kind = PKG_REL_NEEDED_BY;
                snprintf(r->id, sizeof r->id, "%s", inst[i].id);
                snprintf(r->min_version, sizeof r->min_version, "%s", inst[i].rel.depends[j].min_version);
                r->installed = r->satisfied = 1;
                snprintf(r->state, sizeof r->state, "installed; it cannot be removed while %s is", inst[i].id);
                break;
            }
    }
    free(idx); free(inst); free(d);
    return PKG_OK;
}

pkg_status rel_check_rollback(struct pkg_ctx *c, const char *id, const char *version, const char *rel_json,
                              const char *arch, const char *abi, pkg_err *e)
{
    char ip[PKG_MAXPATH], *idx, txt[REL_JSON_MAX + 16], why[400]; size_t len = 0;
    rel_inst *inst = NULL; int ninst = 0, rc;
    rel_decl *d = (rel_decl *)calloc(1, sizeof *d);
    pkg_status st = PKG_OK;
    if (!d) return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id);
    snprintf(txt, sizeof txt, "{ \"x\": 0%s }", rel_json ? rel_json : "");
    {
        js_tok *t; int nt = js_parse_alloc(txt, strlen(txt), &t, PKG_JSON_MAXTOK);
        if (nt > 0) { rel_read(txt, t, nt, 0, d, why, sizeof why); free(t); }
    }
    snprintf(ip, sizeof ip, "%s/index.json", c->db);
    idx = u_read_all(ip, &len);
    rc = load_installed(c, idx, len, &inst, &ninst);
    free(idx);
    if (rc != 0) st = pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id);
    else if (rel_check_decl(d, id, version, inst, ninst, arch, abi, why, sizeof why) != 0)
        st = pkg_fail(e, PKG_E_REQUIRES, "the earlier revision does not fit what is installed", why, id);
    free(inst); free(d);
    return st;
}
