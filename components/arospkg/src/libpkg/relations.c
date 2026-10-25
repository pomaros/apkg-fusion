/* Relations between packages: reading them, and the plan of an install.
 * See relations.h for the meaning of each relation; the rules here are
 * the same as tools/mkindex.py check_relations(), so a published entry is
 * never refused here for its form. */
#include "relations.h"
#include "entries.h"
#include "upgrade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Client features this client implements (mkindex.py CLIENT_FEATURES). */
static const char *const FEATURES[] = { "relations", "large-members" };

static int lc(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch; }

static int same_id(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (lc(*a) != lc(*b)) return 0;
    return *a == *b;
}

static int id_ok(const char *s)
{
    size_t i, n = strlen(s);
    if (n == 0 || n >= PKG_MAXID || !(s[0] >= 'a' && s[0] <= 'z')) return 0;
    for (i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '_' || s[i] == '-'))
            return 0;
    return 1;
}

static void mstr(const char *json, const js_tok *t, int n, int obj, const char *name, char *buf, size_t len)
{
    buf[0] = 0;
    if (js_str(json, t, js_member(json, t, n, obj, name), buf, len) != 0) buf[0] = 0;
}

/* "id" or "id >= 1.2" */
static int parse_depend(const char *s, rel_item *out)
{
    const char *sp = strstr(s, " >= ");
    memset(out, 0, sizeof *out);
    if (!sp) {
        if (strchr(s, ' ')) return -1;
        snprintf(out->id, sizeof out->id, "%s", s);
    } else {
        size_t k = (size_t)(sp - s);
        if (k >= sizeof out->id) return -1;
        memcpy(out->id, s, k); out->id[k] = 0;
        snprintf(out->min_version, sizeof out->min_version, "%s", sp + 4);
        if (version_compare(out->min_version, out->min_version) != 0) return -1;
    }
    return id_ok(out->id) ? 0 : -1;
}

static int read_list(const char *json, const js_tok *t, int ntok, int obj, const char *name,
                     rel_item *items, int *n, int depends, char *why, size_t wn)
{
    int a = js_member(json, t, ntok, obj, name), k, el;
    *n = 0;
    if (a < 0) return 0;
    if (t[a].type != JS_ARR) { snprintf(why, wn, "%s is not a list", name); return -1; }
    for (k = 0; (el = js_elem(t, ntok, a, k)) >= 0; k++) {
        char s[200];
        if (*n >= REL_MAX) { snprintf(why, wn, "%s has more than %d entries", name, REL_MAX); return -1; }
        if (t[el].type != JS_STR || js_str(json, t, el, s, sizeof s) != 0) {
            snprintf(why, wn, "%s[%d] is not a string", name, k); return -1;
        }
        if (depends ? parse_depend(s, &items[*n]) != 0
                    : (memset(&items[*n], 0, sizeof items[*n]), snprintf(items[*n].id, sizeof items[*n].id, "%s", s),
                       !id_ok(s) || strlen(s) >= PKG_MAXID)) {
            snprintf(why, wn, "%s[%d] (%.60s) is not one this apkg can read", name, k, s); return -1;
        }
        (*n)++;
    }
    return 0;
}

int rel_read(const char *json, const js_tok *t, int ntok, int obj, rel_decl *d, char *why, size_t wn)
{
    int a, k, el;
    memset(d, 0, sizeof *d);
    why[0] = 0;
    if (read_list(json, t, ntok, obj, "depends", d->depends, &d->ndepends, 1, why, wn) ||
        read_list(json, t, ntok, obj, "conflicts", d->conflicts, &d->nconflicts, 0, why, wn) ||
        read_list(json, t, ntok, obj, "replaces", d->replaces, &d->nreplaces, 0, why, wn))
        return -1;
    a = js_member(json, t, ntok, obj, "recommends");
    for (k = 0; a >= 0 && (el = js_elem(t, ntok, a, k)) >= 0; k++) {
        rel_item *r;
        if (d->nrecommends >= REL_MAX) break;     /* a hint; extra ones are not lost requirements */
        if (t[el].type != JS_OBJ) continue;
        r = &d->recommends[d->nrecommends];
        mstr(json, t, ntok, el, "id", r->id, sizeof r->id);
        mstr(json, t, ntok, el, "min_version", r->min_version, sizeof r->min_version);
        mstr(json, t, ntok, el, "reason", r->reason, sizeof r->reason);
        if (!id_ok(r->id)) continue;
        if (r->min_version[0] && version_compare(r->min_version, r->min_version) != 0) r->min_version[0] = 0;
        d->nrecommends++;
    }
    a = js_member(json, t, ntok, obj, "client_features");
    for (k = 0; a >= 0 && (el = js_elem(t, ntok, a, k)) >= 0; k++) {
        char f[32]; size_t i; int known = 0;
        if (js_str(json, t, el, f, sizeof f) != 0) f[0] = 0;
        for (i = 0; i < sizeof FEATURES / sizeof FEATURES[0]; i++) if (!strcmp(f, FEATURES[i])) known = 1;
        if (!known && !d->unknown_feature[0]) snprintf(d->unknown_feature, sizeof d->unknown_feature, "%s", f[0] ? f : "?");
    }
    return 0;
}

int rel_any(const rel_decl *d)
{
    return d->ndepends || d->nrecommends || d->nconflicts || d->nreplaces;
}

static void emit_list(js_out *o, const char *name, const rel_item *it, int n, int depends)
{
    int i;
    if (!n) return;
    js_raw(o, ",\n  "); js_key(o, name); js_raw(o, " [");
    for (i = 0; i < n; i++) {
        char s[200];
        if (depends && it[i].min_version[0]) snprintf(s, sizeof s, "%s >= %s", it[i].id, it[i].min_version);
        else snprintf(s, sizeof s, "%s", it[i].id);
        js_raw(o, i ? ", " : ""); js_vstr(o, s);
    }
    js_raw(o, "]");
}

void rel_emit(js_out *o, const rel_decl *d, int with_recommends)
{
    int i;
    emit_list(o, "depends", d->depends, d->ndepends, 1);
    emit_list(o, "conflicts", d->conflicts, d->nconflicts, 0);
    emit_list(o, "replaces", d->replaces, d->nreplaces, 0);
    if (with_recommends && d->nrecommends) {
        js_raw(o, ",\n  "); js_key(o, "recommends"); js_raw(o, " [");
        for (i = 0; i < d->nrecommends; i++) {
            js_raw(o, i ? ", { " : "{ ");
            js_key(o, "id"); js_vstr(o, d->recommends[i].id);
            if (d->recommends[i].min_version[0]) { js_raw(o, ", "); js_key(o, "min_version"); js_vstr(o, d->recommends[i].min_version); }
            js_raw(o, ", "); js_key(o, "reason"); js_vstr(o, d->recommends[i].reason);
            js_raw(o, " }");
        }
        js_raw(o, "]");
    }
}

const char *pkg_rel_word(int k) { return rel_kind_word(k); }

const char *rel_kind_word(int k)
{
    return k == REL_DEPENDS ? "depends" : k == REL_RECOMMENDS ? "recommends" :
           k == REL_CONFLICTS ? "conflicts" : k == REL_REPLACES ? "replaces" : "needed-by";
}

/* --------------------------------------------------------------- planning */

typedef struct {
    const char *json; const js_tok *t; int ntok; int arr;
    const rel_inst *inst; int ninst;
    const char *arch, *abi;
    rel_plan *out;
    char stack[REL_PLAN_MAX][PKG_MAXID]; int depth;
} planner;

static const rel_inst *installed(const planner *p, const char *id)
{
    int i;
    for (i = 0; i < p->ninst; i++) if (same_id(p->inst[i].id, id)) return &p->inst[i];
    return NULL;
}

static const rel_op *planned(const planner *p, const char *id)
{
    int i;
    for (i = 0; i < p->out->nops; i++) if (p->out->ops[i].kind != RELOP_REMOVE_REPLACED && same_id(p->out->ops[i].id, id)) return &p->out->ops[i];
    return NULL;
}

static int refuse(planner *p, pkg_status st, const char *fmt, const char *a, const char *b, const char *c)
{
    p->out->ok = 0;
    p->out->status = st;
    snprintf(p->out->why, sizeof p->out->why, fmt, a ? a : "", b ? b : "", c ? c : "");
    return -1;
}

static int native(const char *arch, const char *abi, const char *my_arch, const char *my_abi)
{
    char why[8];
    return pkg_compat_of(arch, abi, my_arch, my_abi, why, sizeof why) == PKG_COMPAT_NATIVE;
}

/* version >= min, when min says anything. */
static int meets(const char *version, const char *min)
{
    int c;
    if (!min || !*min) return 1;
    c = version_compare(version, min);
    return c == 0 || c == 1;
}

/* The catalogue element for `id` on this machine, or -1 with the refusal. */
static int choose(planner *p, const char *id, const char *wanted_by)
{
    int how, same, el = index_select_variant(p->json, p->t, p->ntok, p->arr, id, p->arch, p->abi, &how, &same);
    char arch[16], abi[8];
    if (el < 0)
        return wanted_by ? refuse(p, PKG_E_NOT_FOUND, "%s needs %s, which the catalogue does not list.%s", wanted_by, id, "")
                         : refuse(p, PKG_E_NOT_FOUND, "%s is not in the catalogue.%s%s", id, "", "");
    if (how == VARIANT_SEVERAL)
        return refuse(p, PKG_E_CONFLICT, "The catalogue lists more than one build of %s for this system; "
                      "nothing is picked for it.%s%s", id, "", "");
    mstr(p->json, p->t, p->ntok, el, "arch", arch, sizeof arch);
    mstr(p->json, p->t, p->ntok, el, "abi", abi, sizeof abi);
    if (!native(arch, abi, p->arch, p->abi)) {
        char target[32];
        snprintf(target, sizeof target, "%s/%s", arch, abi[0] ? abi : "?");
        return wanted_by ? refuse(p, PKG_E_NOT_FOUND, "%s needs %s, which the catalogue has only for other "
                                  "systems (%s).", wanted_by, id, target)
                         : -2;   /* the target: the install's own target check explains it */
    }
    return el;
}

static int visit(planner *p, const char *id, const char *min, const char *wanted_by, int is_target)
{
    const rel_inst *in = installed(p, id);
    const rel_op *op;
    char version[64], why[200];
    rel_decl *d;
    int el, i, rc = 0;

    if (in && !is_target) {
        if (!native(in->arch, in->abi, p->arch, p->abi))
            return refuse(p, PKG_E_REQUIRES, "%s needs %s; the installed one is for another system.%s", wanted_by, id, "");
        if (!meets(in->version, min)) {
            snprintf(why, sizeof why, "%s >= %s; %s %s is installed. Upgrade it first: apkg upgrade %s",
                     id, min, id, in->version, id);
            return refuse(p, PKG_E_REQUIRES, "%s needs %s%s", wanted_by, why, "");
        }
        return 0;
    }
    if ((op = planned(p, id)) != NULL) {
        if (!meets(op->version, min)) {
            snprintf(why, sizeof why, "%s >= %s, and the catalogue has %s", id, min, op->version);
            return refuse(p, PKG_E_REQUIRES, "%s needs %s.%s", wanted_by, why, "");
        }
        return 0;
    }
    for (i = 0; i < p->depth; i++)
        if (same_id(p->stack[i], id))
            return refuse(p, PKG_E_REQUIRES, "The dependencies of %s lead back to %s (a cycle); "
                          "nothing can be installed first.%s", wanted_by, id, "");
    if ((el = choose(p, id, wanted_by)) < 0) return el == -2 ? 0 : -1;
    mstr(p->json, p->t, p->ntok, el, "version", version, sizeof version);
    if (!meets(version, min)) {
        snprintf(why, sizeof why, "%s >= %s, and the catalogue has %s", id, min, version);
        return refuse(p, PKG_E_REQUIRES, "%s needs %s.%s", wanted_by, why, "");
    }
    /* On the heap: this recurses, and an AROS Shell stack is small. */
    if (!(d = (rel_decl *)malloc(sizeof *d))) return refuse(p, PKG_E_NOMEM, "out of memory%s%s%s", "", "", "");
    if (rel_read(p->json, p->t, p->ntok, el, d, why, sizeof why) != 0)
        rc = refuse(p, PKG_E_REQUIRES, "The relations of %s cannot be read: %s%s", id, why, "");
    else if (d->unknown_feature[0])
        rc = refuse(p, PKG_E_REQUIRES, "%s needs a newer apkg (it uses \"%s\"). Update apkg: apkg self-update%s",
                    id, d->unknown_feature, "");
    else if (p->depth >= REL_PLAN_MAX)
        rc = refuse(p, PKG_E_REQUIRES, "%s has too many dependencies.%s%s", id, "", "");
    else {
        snprintf(p->stack[p->depth++], PKG_MAXID, "%s", id);
        for (i = 0; i < d->ndepends && rc == 0; i++)
            rc = visit(p, d->depends[i].id, d->depends[i].min_version, id, 0);
        p->depth--;
    }
    free(d);
    if (rc) return -1;
    if (p->out->nops >= REL_PLAN_MAX) return refuse(p, PKG_E_REQUIRES, "%s has too many dependencies.%s%s", id, "", "");
    {
        rel_op *o = &p->out->ops[p->out->nops++];
        memset(o, 0, sizeof *o);
        o->kind = is_target ? RELOP_TARGET : RELOP_DEP;
        snprintf(o->id, sizeof o->id, "%s", id);
        snprintf(o->version, sizeof o->version, "%s", version);
        o->revision = js_long(p->json, p->t, js_member(p->json, p->t, p->ntok, el, "revision"), 0);
        if (!is_target) snprintf(o->note, sizeof o->note, "needed by %s%s%s", wanted_by, min && *min ? ", at least " : "", min ? min : "");
    }
    return 0;
}

/* The relations of an installed package: its registry's, else the
   catalogue's entry for that build. */
static const rel_decl *inst_rel(planner *p, const rel_inst *in, rel_decl *buf)
{
    char why[200];
    int how, same, el;
    if (in->rel_known) return &in->rel;
    el = index_select_variant(p->json, p->t, p->ntok, p->arr, in->id, in->arch, in->abi, &how, &same);
    if (el >= 0 && how == VARIANT_ONE && rel_read(p->json, p->t, p->ntok, el, buf, why, sizeof why) == 0) return buf;
    memset(buf, 0, sizeof *buf);
    return buf;
}

static int names(const rel_item *it, int n, const char *id)
{
    int i;
    for (i = 0; i < n; i++) if (same_id(it[i].id, id)) return 1;
    return 0;
}

static int planned_rel(planner *p, const char *id, rel_decl *d)
{
    char why[200];
    int how, same, el = index_select_variant(p->json, p->t, p->ntok, p->arr, id, p->arch, p->abi, &how, &same);
    memset(d, 0, sizeof *d);
    return el >= 0 && rel_read(p->json, p->t, p->ntok, el, d, why, sizeof why) == 0;
}

int rel_plan_install(const char *json, size_t len, const rel_inst *inst, int ninst,
                     const char *id, const char *my_arch, const char *my_abi,
                     int upgrade, rel_plan *out)
{
    planner p;
    js_tok *t = NULL;
    int ntok, i, j, k, ntarget;
    rel_decl *dd = (rel_decl *)malloc(2 * sizeof *dd);
    char why[300];
#define d  (dd[0])
#define dq (dd[1])

    memset(out, 0, sizeof *out);
    memset(&p, 0, sizeof p);
    out->ok = 1;
    if (!dd) { out->ok = 0; out->status = PKG_E_NOMEM; snprintf(out->why, sizeof out->why, "out of memory"); return JS_ENOMEM; }
    ntok = js_parse_alloc(json, len, &t, PKG_JSON_MAXTOK);
    if (ntok < 0) { free(dd); out->ok = 0; out->status = PKG_E_VERIFY; snprintf(out->why, sizeof out->why, "the catalogue cannot be read"); return ntok; }
    p.json = json; p.t = t; p.ntok = ntok; p.arr = js_member(json, t, ntok, 0, "packages");
    p.inst = inst; p.ninst = ninst; p.arch = my_arch; p.abi = my_abi; p.out = out;
    if (p.arr < 0) { free(t); free(dd); out->ok = 0; out->status = PKG_E_VERIFY; snprintf(out->why, sizeof out->why, "the catalogue cannot be read"); return JS_EINVAL; }

    if (visit(&p, id, NULL, NULL, 1) != 0) goto done;
    if (upgrade) {
        /* An upgrade installs nothing else: what the new build needs must
           already be here. */
        for (i = 0; i < out->nops; i++)
            if (out->ops[i].kind == RELOP_DEP) {
                snprintf(why, sizeof why, "The new build of %s needs %s, which is not installed. "
                         "Install it first: apkg install %s", id, out->ops[i].id, out->ops[i].id);
                out->ok = 0; out->status = PKG_E_REQUIRES;
                snprintf(out->why, sizeof out->why, "%s", why);
                goto done;
            }
    }
    ntarget = out->nops;

    /* Replacements: the package asked for may replace installed ones; a
       dependency that would replace something is installed on its own
       first, so the plan stays one replacement with one record. Nothing is
       replaced that a package staying installed, or one this plan
       installs, still depends on. */
    for (i = 0; i < ntarget; i++) {
        if (!planned_rel(&p, out->ops[i].id, &d)) continue;
        for (j = 0; j < d.nreplaces; j++) {
            const rel_inst *old = installed(&p, d.replaces[j].id);
            char who[300] = "";
            size_t wl = 0;
            int m, q;
            if (!old || (same_id(old->id, id) && out->ops[i].kind == RELOP_TARGET)) continue;
            if (out->ops[i].kind != RELOP_TARGET) {
                snprintf(why, sizeof why, "%s, which %s needs, would replace the installed %s. Install %s "
                         "by itself first, then %s.", out->ops[i].id, id, old->id, out->ops[i].id, id);
                out->ok = 0; out->status = PKG_E_CONFLICT; snprintf(out->why, sizeof out->why, "%s", why);
                goto done;
            }
            for (q = 0; q < ninst; q++) {
                if (same_id(inst[q].id, old->id) || names(d.replaces, d.nreplaces, inst[q].id)) continue;
                for (m = 0; m < inst[q].rel.ndepends; m++)
                    if (same_id(inst[q].rel.depends[m].id, old->id) && wl + 70 < sizeof who)
                        wl += (size_t)snprintf(who + wl, sizeof who - wl, "%s%s", wl ? ", " : "", inst[q].id);
            }
            for (q = 0; q < ntarget; q++) {
                if (q == i || !planned_rel(&p, out->ops[q].id, &dq)) continue;
                if (names(dq.depends, dq.ndepends, old->id) && wl + 70 < sizeof who)
                    wl += (size_t)snprintf(who + wl, sizeof who - wl, "%s%s", wl ? ", " : "", out->ops[q].id);
            }
            if (wl) {
                snprintf(why, sizeof why, "%s would replace %s, but %s still need%s it. Remove %s first, "
                         "or install it without replacing.", out->ops[i].id, old->id, who,
                         strchr(who, ',') ? "" : "s", who);
                out->ok = 0; out->status = PKG_E_CONFLICT;
                snprintf(out->why, sizeof out->why, "%s", why);
                goto done;
            }
            if (out->nops >= REL_PLAN_MAX) continue;
            {
                rel_op *o = &out->ops[out->nops++];
                memset(o, 0, sizeof *o);
                o->kind = RELOP_REMOVE_REPLACED;
                snprintf(o->id, sizeof o->id, "%s", old->id);
                snprintf(o->version, sizeof o->version, "%s", old->version);
                o->revision = old->revision;
                snprintf(o->note, sizeof o->note, "replaced by %s; files you changed in it are kept", out->ops[i].id);
            }
        }
    }

    /* The replaced packages go dependents first: removing one that another
       replaced package still depends on would be refused. */
    {
        int a, b, moved = 1, guard = 0;
        while (moved && guard++ < REL_PLAN_MAX) {
            moved = 0;
            for (a = ntarget; a < out->nops; a++)
                for (b = a + 1; b < out->nops; b++) {
                    const rel_inst *later = installed(&p, out->ops[b].id);
                    if (later && names(later->rel.depends, later->rel.ndepends, out->ops[a].id)) {
                        rel_op tmp = out->ops[a]; out->ops[a] = out->ops[b]; out->ops[b] = tmp;
                        moved = 1;
                    }
                }
        }
    }

    /* Conflicts, in both directions, against what stays installed. */
    for (i = 0; i < ntarget; i++) {
        const char *pid = out->ops[i].id;
        if (!planned_rel(&p, pid, &d)) continue;
        for (k = 0; k < ninst; k++) {
            const rel_inst *q = &inst[k];
            const rel_decl *qr;
            int replaced = 0;
            if (same_id(q->id, pid)) continue;
            for (j = ntarget; j < out->nops; j++) if (same_id(out->ops[j].id, q->id)) replaced = 1;
            if (replaced) continue;
            qr = inst_rel(&p, q, &dq);
            if (names(d.conflicts, d.nconflicts, q->id) || names(d.replaces, d.nreplaces, q->id)) {
                snprintf(why, sizeof why, "%s cannot be installed beside %s: %s says it conflicts with %s. "
                         "Remove %s first if you want %s.", pid, q->id, pid, q->id, q->id, pid);
                out->ok = 0; out->status = PKG_E_CONFLICT; snprintf(out->why, sizeof out->why, "%s", why);
                goto done;
            }
            if (names(qr->conflicts, qr->nconflicts, pid) || names(qr->replaces, qr->nreplaces, pid)) {
                snprintf(why, sizeof why, "%s cannot be installed beside %s: the installed %s says it conflicts "
                         "with %s. Remove %s first if you want %s.", pid, q->id, q->id, pid, q->id, pid);
                out->ok = 0; out->status = PKG_E_CONFLICT; snprintf(out->why, sizeof out->why, "%s", why);
                goto done;
            }
        }
        for (k = 0; k < ntarget; k++)
            if (k != i && names(d.conflicts, d.nconflicts, out->ops[k].id)) {
                snprintf(why, sizeof why, "%s and %s would both be installed, but %s conflicts with %s.",
                         pid, out->ops[k].id, pid, out->ops[k].id);
                out->ok = 0; out->status = PKG_E_CONFLICT; snprintf(out->why, sizeof out->why, "%s", why);
                goto done;
            }
    }
    out->extra = out->nops - 1;
done:
    free(t);
    free(dd);
    return 0;
#undef d
#undef dq
}

int rel_required_by(const rel_inst *inst, int ninst, const char *id, const char *except, char *out, size_t n)
{
    int i, j, count = 0;
    size_t used = 0;
    if (n) out[0] = 0;
    for (i = 0; i < ninst; i++) {
        if (same_id(inst[i].id, id) || (except && same_id(inst[i].id, except))) continue;
        for (j = 0; j < inst[i].rel.ndepends; j++)
            if (same_id(inst[i].rel.depends[j].id, id)) {
                char one[160];
                if (inst[i].rel.depends[j].min_version[0])
                    snprintf(one, sizeof one, "%s (needs %s >= %s)", inst[i].id, id, inst[i].rel.depends[j].min_version);
                else snprintf(one, sizeof one, "%s", inst[i].id);
                if (used + strlen(one) + 3 < n) used += (size_t)snprintf(out + used, n - used, "%s%s", count ? ", " : "", one);
                count++;
                break;
            }
    }
    return count;
}

/* ------------------------------------------------------------- describing */

int rel_rows(const char *json, size_t len, const rel_decl *d, const rel_inst *inst, int ninst,
             const char *my_arch, const char *my_abi, rel_row *rows, int max)
{
    js_tok *t = NULL;
    int ntok = json ? js_parse_alloc(json, len, &t, PKG_JSON_MAXTOK) : -1;
    int arr = ntok > 0 ? js_member(json, t, ntok, 0, "packages") : -1;
    int n = 0, kind, i;
    for (kind = REL_DEPENDS; kind <= REL_REPLACES; kind++) {
        const rel_item *it = kind == REL_DEPENDS ? d->depends : kind == REL_RECOMMENDS ? d->recommends :
                             kind == REL_CONFLICTS ? d->conflicts : d->replaces;
        int cnt = kind == REL_DEPENDS ? d->ndepends : kind == REL_RECOMMENDS ? d->nrecommends :
                  kind == REL_CONFLICTS ? d->nconflicts : d->nreplaces;
        for (i = 0; i < cnt && n < max; i++) {
            rel_row *r = &rows[n++];
            int j, how = VARIANT_NONE, same, el = -1;
            memset(r, 0, sizeof *r);
            r->kind = kind;
            snprintf(r->id, sizeof r->id, "%s", it[i].id);
            snprintf(r->min_version, sizeof r->min_version, "%s", it[i].min_version);
            snprintf(r->reason, sizeof r->reason, "%s", it[i].reason);
            if (arr >= 0) {
                int e2;
                for (j = 0; (e2 = js_elem(t, ntok, arr, j)) >= 0; j++) {
                    char pid[PKG_MAXID];
                    mstr(json, t, ntok, e2, "id", pid, sizeof pid);
                    if (same_id(pid, it[i].id)) { r->listed = 1; break; }
                }
                el = index_select_variant(json, t, ntok, arr, it[i].id, my_arch, my_abi, &how, &same);
                if (el >= 0 && how == VARIANT_ONE) {
                    char arch[16], abi[8];
                    mstr(json, t, ntok, el, "arch", arch, sizeof arch);
                    mstr(json, t, ntok, el, "abi", abi, sizeof abi);
                    if (native(arch, abi, my_arch, my_abi)) {
                        mstr(json, t, ntok, el, "version", r->available, sizeof r->available);
                        r->compatible = meets(r->available, it[i].min_version);
                    }
                }
            }
            for (j = 0; j < ninst; j++)
                if (same_id(inst[j].id, it[i].id)) {
                    r->installed = 1;
                    snprintf(r->installed_version, sizeof r->installed_version, "%s", inst[j].version);
                    r->satisfied = native(inst[j].arch, inst[j].abi, my_arch, my_abi) && meets(inst[j].version, it[i].min_version);
                }
            if (kind == REL_DEPENDS || kind == REL_RECOMMENDS) {
                if (r->satisfied) snprintf(r->state, sizeof r->state, "installed (%s)", r->installed_version);
                else if (r->installed) snprintf(r->state, sizeof r->state, "installed %s, but %s%s needed",
                                                r->installed_version, it[i].min_version[0] ? "at least " : "a build for this system is",
                                                it[i].min_version[0] ? it[i].min_version : "");
                else if (r->compatible) snprintf(r->state, sizeof r->state, "not installed; the catalogue has %s (apkg install %s)",
                                                 r->available, r->id);
                else if (r->available[0]) snprintf(r->state, sizeof r->state, "not installed; the catalogue has only %s, %s is needed",
                                                   r->available, it[i].min_version);
                else if (r->listed) snprintf(r->state, sizeof r->state, "not installed; the catalogue has it only for other systems");
                else snprintf(r->state, sizeof r->state, "not installed; not in the catalogue");
            } else if (kind == REL_CONFLICTS) {
                snprintf(r->state, sizeof r->state, r->installed ? "installed: this package cannot be installed beside it"
                                                                 : "not installed");
            } else {
                snprintf(r->state, sizeof r->state, r->installed ? "installed: installing this package removes it (files you changed are kept)"
                                                                 : "not installed");
            }
        }
    }
    free(t);
    return n;
}

int rel_check_decl(const rel_decl *d, const char *id, const char *version, const rel_inst *inst, int ninst,
                   const char *my_arch, const char *my_abi, char *why, size_t wn)
{
    int i, j;
    for (i = 0; i < d->ndepends; i++) {
        const rel_inst *in = NULL;
        for (j = 0; j < ninst; j++) if (same_id(inst[j].id, d->depends[i].id)) in = &inst[j];
        if (!in || !native(in->arch, in->abi, my_arch, my_abi) || !meets(in->version, d->depends[i].min_version)) {
            snprintf(why, wn, "%s %s needs %s%s%s, which is not installed here%s.", id, version, d->depends[i].id,
                     d->depends[i].min_version[0] ? " >= " : "", d->depends[i].min_version,
                     in ? " in that version" : "");
            return -1;
        }
    }
    for (j = 0; j < ninst; j++) {
        if (same_id(inst[j].id, id)) continue;
        if (names(d->conflicts, d->nconflicts, inst[j].id) || names(d->replaces, d->nreplaces, inst[j].id) ||
            names(inst[j].rel.conflicts, inst[j].rel.nconflicts, id) || names(inst[j].rel.replaces, inst[j].rel.nreplaces, id)) {
            snprintf(why, wn, "%s %s conflicts with the installed %s.", id, version, inst[j].id);
            return -1;
        }
        for (i = 0; i < inst[j].rel.ndepends; i++)
            if (same_id(inst[j].rel.depends[i].id, id) && !meets(version, inst[j].rel.depends[i].min_version)) {
                snprintf(why, wn, "%s needs %s >= %s; going back to %s would break it.", inst[j].id, id,
                         inst[j].rel.depends[i].min_version, version);
                return -1;
            }
    }
    return 0;
}
