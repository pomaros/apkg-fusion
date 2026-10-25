/* See rexxcmd.h. The argument syntax is AmigaDOS ReadArgs's, reimplemented
 * because ReadArgs cannot run on the host where these tests do, and because
 * Zune's own ARexx dispatcher calls the command even when ReadArgs has
 * refused the arguments. */
#include "rexxcmd.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One argument of a template: name and kind, as ReadArgs spells them. */
typedef enum { A_POS_PKG, A_POS_ID, A_KEY_FIELD, A_KEY_MATCH, A_SWITCH_INSTALLED, A_SWITCH_SHOW } argkind;

typedef struct {
    rx_cmd      cmd;
    const char *name;
    int         nargs;
    argkind     args[3];
    int         required;       /* the positional must be given */
} cmddef;

static const cmddef cmds[] = {
    { RXC_VERSION,      "VERSION",      0, { 0 }, 0 },
    { RXC_HELP,         "HELP",         0, { 0 }, 0 },
    { RXC_LIST,         "LIST",         2, { A_SWITCH_INSTALLED, A_KEY_MATCH }, 0 },
    { RXC_INFO,         "INFO",         2, { A_POS_PKG, A_KEY_FIELD }, 1 },
    { RXC_REQUIREMENTS, "REQUIREMENTS", 1, { A_POS_PKG }, 1 },
    { RXC_SHOW,         "SHOW",         1, { A_POS_PKG }, 1 },
    { RXC_UPDATE,       "UPDATE",       0, { 0 }, 0 },
    { RXC_INSTALL,      "INSTALL",      1, { A_POS_PKG }, 1 },
    { RXC_REMOVE,       "REMOVE",       1, { A_POS_PKG }, 1 },
    { RXC_UPGRADE,      "UPGRADE",      1, { A_POS_PKG }, 1 },
    { RXC_ROLLBACK,     "ROLLBACK",     1, { A_POS_PKG }, 1 },
    { RXC_JOB,          "JOB",          2, { A_POS_ID, A_KEY_FIELD }, 0 },
    { RXC_WAIT,         "WAIT",         2, { A_POS_ID, A_KEY_FIELD }, 0 },
    { RXC_CANCEL,       "CANCEL",       1, { A_POS_ID }, 0 },
    { RXC_QUIT,         "QUIT",         0, { 0 }, 0 },
    { RXC_LASTERROR,    "LASTERROR",    0, { 0 }, 0 },
    { RXC_OUTDATED,     "OUTDATED",     1, { A_SWITCH_SHOW }, 0 },
    { RXC_RUN,          "RUN",          1, { A_POS_PKG }, 1 },
    { RXC_VERIFY,       "VERIFY",       1, { A_POS_PKG }, 1 },
};
#define NCMDS (int)(sizeof cmds / sizeof cmds[0])

static const char *argname(argkind k)
{
    switch (k) {
    case A_POS_PKG: return "PACKAGE";
    case A_POS_ID: return "ID";
    case A_KEY_FIELD: return "FIELD";
    case A_KEY_MATCH: return "MATCH";
    case A_SWITCH_INSTALLED: return "INSTALLED";
    case A_SWITCH_SHOW: return "SHOW";
    }
    return "?";
}

const char *rx_cmdname(rx_cmd c)
{
    int i;
    for (i = 0; i < NCMDS; i++) if (cmds[i].cmd == c) return cmds[i].name;
    return "?";
}

const char *rx_help(void)
{
    static char buf[256];
    size_t k = 0; int i;
    if (buf[0]) return buf;
    for (i = 0; i < NCMDS; i++)
        k += (size_t)snprintf(buf + k, sizeof buf - k, "%s%s", i ? " " : "", cmds[i].name);
    return buf;
}

/* ------------------------------------------------------------- tokens */

#define MAXTOK 8
typedef struct { char text[160]; int quoted; char key[16]; } token;   /* key: "FIELD" of FIELD=x */

static int ieq(const char *a, const char *b)
{
    while (*a && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b)) a++, b++;
    return !*a && !*b;
}

/* Split the argument part. Quotes group, and inside them AmigaDOS's escapes
 * hold: *" is a quote, ** an asterisk, *N a newline -- refused later with
 * every other control character. KEY=value and KEY="a b" are one token with
 * the key split off. Returns the count, or -1 with `err` set. */
static int tokenize(const char *s, token *t, int max, char *err, size_t errn)
{
    int n = 0;
    for (;;) {
        size_t k = 0; int inq = 0, started = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) return n;
        if (n == max) { snprintf(err, errn, "BADARGS too many arguments"); return -1; }
        memset(&t[n], 0, sizeof t[n]);
        for (;;) {
            char c = *s;
            if (!inq && (c == ' ' || c == '\t' || !c)) break;
            if (inq && !c) { snprintf(err, errn, "BADARGS unterminated quote"); return -1; }
            s++;
            if (!inq && c == '"' && (!started || (k > 0 && t[n].text[k - 1] == '=' && !t[n].key[0]))) {
                /* An opening quote: at the start of the token, or right after
                   KEY= -- in which case the key is split off here. */
                if (started) {
                    t[n].text[k - 1] = 0;
                    if (k - 1 >= sizeof t[n].key) { snprintf(err, errn, "BADARGS unknown keyword"); return -1; }
                    memcpy(t[n].key, t[n].text, k); k = 0;
                } else t[n].quoted = 1;
                inq = 1; started = 1; continue;
            }
            if (inq && c == '"') {
                inq = 0;
                if (*s && *s != ' ' && *s != '\t') {
                    snprintf(err, errn, "BADARGS text after a closing quote"); return -1;
                }
                continue;
            }
            if (inq && c == '*') {
                char e = *s;
                if (e == '"' || e == '*') { c = e; s++; }
                else if (e == 'n' || e == 'N') { c = '\n'; s++; }
                else if (e == 'e' || e == 'E') { c = 0x1b; s++; }
            }
            started = 1;
            if (k + 1 >= sizeof t[n].text) { snprintf(err, errn, "BADARGS argument longer than %d characters", (int)sizeof t[n].text - 1); return -1; }
            t[n].text[k++] = c;
        }
        t[n].text[k] = 0;
        /* Unquoted KEY=value: split at the first '='. Whether KEY is one of
           this command's keywords is decided by the caller. */
        if (!t[n].quoted && !t[n].key[0]) {
            char *eq = strchr(t[n].text, '=');
            if (eq && eq != t[n].text && (size_t)(eq - t[n].text) < sizeof t[n].key) {
                *eq = 0;
                snprintf(t[n].key, sizeof t[n].key, "%s", t[n].text);
                memmove(t[n].text, eq + 1, strlen(eq + 1) + 1);
            }
        }
        n++;
    }
}

static int has_control(const char *s)
{
    for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == 0x7f) return 1;
    return 0;
}

static int put(char *dst, size_t n, const char *what, const char *v, char *err, size_t errn)
{
    if (!*v) { snprintf(err, errn, "BADARGS %s is empty", what); return -1; }
    if (strlen(v) >= n) { snprintf(err, errn, "BADARGS %s is longer than %d characters", what, (int)n - 1); return -1; }
    if (has_control(v)) { snprintf(err, errn, "BADARGS %s contains a control character", what); return -1; }
    snprintf(dst, n, "%s", v);
    return 0;
}

static int set_arg(rx_req *r, argkind k, const char *v, char *err, size_t errn)
{
    switch (k) {
    case A_POS_PKG:   return put(r->package, sizeof r->package, "PACKAGE", v, err, errn);
    case A_KEY_FIELD: return put(r->field, sizeof r->field, "FIELD", v, err, errn);
    case A_KEY_MATCH: return put(r->match, sizeof r->match, "MATCH", v, err, errn);
    case A_POS_ID: {
        char *end; long id;
        if (!*v || !isdigit((unsigned char)*v)) { snprintf(err, errn, "BADARGS ID must be a job number"); return -1; }
        id = strtol(v, &end, 10);
        if (*end || id <= 0) { snprintf(err, errn, "BADARGS ID must be a job number"); return -1; }
        r->job = id; return 0;
    }
    case A_SWITCH_INSTALLED: case A_SWITCH_SHOW: return 0;
    }
    return -1;
}

int rx_parse(const char *line, rx_req *r, char *err, size_t errn)
{
    char word[24]; size_t k = 0;
    const cmddef *d = NULL;
    token t[MAXTOK]; int nt, i, j, given[3] = { 0, 0, 0 };

    memset(r, 0, sizeof *r);
    err[0] = 0;
    while (*line == ' ' || *line == '\t') line++;
    while (*line && *line != ' ' && *line != '\t' && k + 1 < sizeof word) word[k++] = *line++;
    word[k] = 0;
    if (!k) { snprintf(err, errn, "BADCOMMAND empty command; HELP lists the commands"); return RX_RC_FAIL; }
    if (*line && *line != ' ' && *line != '\t') { snprintf(err, errn, "BADCOMMAND unknown command; HELP lists the commands"); return RX_RC_FAIL; }
    /* Whole names only: Zune's dispatcher matches prefixes, so "IN" there
       would have been INSTALL. */
    for (i = 0; i < NCMDS; i++) if (ieq(word, cmds[i].name)) { d = &cmds[i]; break; }
    if (!d) {
        char shown[24]; snprintf(shown, sizeof shown, "%s", word);
        for (i = 0; shown[i]; i++) if ((unsigned char)shown[i] < 0x20) shown[i] = '?';
        snprintf(err, errn, "BADCOMMAND unknown command \"%s\"; HELP lists the commands", shown);
        return RX_RC_FAIL;
    }
    r->cmd = d->cmd;

    nt = tokenize(line, t, MAXTOK, err, errn);
    if (nt < 0) return RX_RC_FAIL;
    for (i = 0; i < nt; i++) {
        int slot = -1;
        const char *key = t[i].key[0] ? t[i].key : (t[i].quoted ? NULL : t[i].text);
        /* A keyword: KEY=value, or the bare word followed by its value. */
        if (key)
            for (j = 0; j < d->nargs; j++) if (ieq(key, argname(d->args[j]))) { slot = j; break; }
        if (slot >= 0) {
            argkind a = d->args[slot];
            if (given[slot]) { snprintf(err, errn, "BADARGS %s given twice", argname(a)); return RX_RC_FAIL; }
            if (a == A_SWITCH_INSTALLED || a == A_SWITCH_SHOW) {
                if (t[i].key[0]) { snprintf(err, errn, "BADARGS %s takes no value", argname(a)); return RX_RC_FAIL; }
                if (a == A_SWITCH_SHOW) r->show = 1; else r->installed = 1;
                given[slot] = 1; continue;
            }
            if (t[i].key[0]) {
                if (set_arg(r, a, t[i].text, err, errn)) return RX_RC_FAIL;
            } else {
                if (i + 1 >= nt) { snprintf(err, errn, "BADARGS %s needs a value", argname(a)); return RX_RC_FAIL; }
                if (set_arg(r, a, t[++i].text, err, errn)) return RX_RC_FAIL;
            }
            given[slot] = 1;
            continue;
        }
        if (t[i].key[0]) {
            snprintf(err, errn, "BADARGS %s does not take %s", d->name, t[i].key);
            return RX_RC_FAIL;
        }
        /* Otherwise the first positional not yet given. */
        for (j = 0; j < d->nargs; j++)
            if (!given[j] && (d->args[j] == A_POS_PKG || d->args[j] == A_POS_ID)) { slot = j; break; }
        if (slot < 0) {
            char shown[40]; int q;
            snprintf(shown, sizeof shown, "%s", t[i].text);
            for (q = 0; shown[q]; q++) if ((unsigned char)shown[q] < 0x20) shown[q] = '?';
            snprintf(err, errn, "BADARGS %s: unexpected argument \"%s\"", d->name, shown);
            return RX_RC_FAIL;
        }
        if (set_arg(r, d->args[slot], t[i].text, err, errn)) return RX_RC_FAIL;
        given[slot] = 1;
    }
    for (j = 0; j < d->nargs; j++)
        if (d->required && d->args[j] == A_POS_PKG && !given[j]) {
            snprintf(err, errn, "BADARGS %s needs PACKAGE", d->name);
            return RX_RC_FAIL;
        }
    return RX_RC_OK;
}

/* ------------------------------------------------------------- words */

const char *rx_statusword(pkg_status s)
{
    switch (s) {
    case PKG_OK:               return "OK";
    case PKG_E_LOCKED:         return "LOCKED";
    case PKG_E_STALE_LOCK:     return "STALELOCK";
    case PKG_E_UNRESOLVED_TXN: return "UNRESOLVED";
    case PKG_E_NOT_FOUND:      return "NOTFOUND";
    case PKG_E_EXISTS:         return "EXISTS";
    case PKG_E_VERIFY:         return "VERIFY";
    case PKG_E_CONFLICT:       return "CONFLICT";
    case PKG_E_ARCHIVE:        return "ARCHIVE";
    case PKG_E_IO:             return "IO";
    case PKG_E_NOMEM:          return "NOMEM";
    case PKG_E_INTERRUPTED:    return "INTERRUPTED";
    case PKG_E_NETWORK:        return "NETWORK";
    case PKG_E_REQUIRES:       return "REQUIRES";
    case PKG_E_CANCELLED:      return "CANCELLED";
    case PKG_E_LAUNCH:         return "CANNOTRUN";
    case PKG_E_REQUIRED:       return "REQUIRED";
    }
    return "FAILED";
}

const char *rx_statename(rx_jobstate s)
{
    static const char *n[] = { "preview", "confirm", "running", "done", "failed", "cancelled", "declined" };
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "?";
}

int rx_final(rx_jobstate s) { return s >= RXJ_DONE; }

/* ------------------------------------------------------------- replies */

typedef struct { char *out; size_t n, k; int over; } sink;

/* One "name value" line, with any control character in the value turned into
 * a space: a summary with a newline in it must not become two fields. */
static void line(sink *s, const char *name, const char *v)
{
    size_t need;
    if (s->over) return;
    need = strlen(name) + 1 + strlen(v) + (s->k ? 1 : 0);
    if (s->k + need >= s->n) { s->over = 1; return; }
    if (s->k) s->out[s->k++] = '\n';
    s->k += (size_t)sprintf(s->out + s->k, "%s ", name);
    for (; *v; v++) s->out[s->k++] = ((unsigned char)*v < 0x20 || *v == 0x7f) ? ' ' : *v;
    s->out[s->k] = 0;
}

static void value(sink *s, const char *v)
{
    size_t len = strlen(v), i;
    if (len >= s->n) { s->over = 1; return; }
    for (i = 0; i < len; i++) s->out[i] = ((unsigned char)v[i] < 0x20 || v[i] == 0x7f) ? ' ' : v[i];
    s->out[len] = 0;
}

static const char *entry_fields =
    "id version revision state installedversion installedrevision summary category "
    "arch abi kind size requires canupgrade canrollback compatibility requirements";
static const char *job_fields =
    "id op package origin state phase done total cancancel error message";

const char *rx_entry_fields(void) { return entry_fields; }
const char *rx_job_fields(void) { return job_fields; }

/* Walks a space separated field list, calling `get` for each name, and emits
 * either all of them as lines or the one asked for as a bare value. */
typedef const char *(*getter)(const void *obj, const char *name, char *tmp, size_t n);

static int format(const void *obj, getter get, const char *fields, const char *want,
                  char *out, size_t n)
{
    sink s = { out, n, 0, 0 };
    char name[24], tmp[512];
    const char *p = fields;
    int found = 0;
    out[0] = 0;
    while (*p) {
        size_t k = strcspn(p, " ");
        snprintf(name, sizeof name, "%.*s", (int)k, p);
        p += k; while (*p == ' ') p++;
        if (want && *want) {
            if (!ieq(want, name)) continue;
            value(&s, get(obj, name, tmp, sizeof tmp));
            found = 1; break;
        }
        line(&s, name, get(obj, name, tmp, sizeof tmp));
    }
    if (want && *want && !found) return -1;
    return s.over ? -2 : 0;
}

typedef struct { const pkg_entry *e; int up, back; const char *compat, *reqs; } entry_view;

static const char *entry_get(const void *obj, const char *f, char *tmp, size_t n)
{
    const entry_view *v = (const entry_view *)obj;
    const pkg_entry *e = v->e;
    if (!strcmp(f, "id")) return e->id;
    if (!strcmp(f, "version")) return e->version;
    if (!strcmp(f, "revision")) { snprintf(tmp, n, "%ld", e->revision); return tmp; }
    if (!strcmp(f, "state")) return e->installed ? "installed" : (e->ours ? "available" : "otherabi");
    if (!strcmp(f, "installedversion")) return e->installed ? e->installed_version : "";
    if (!strcmp(f, "installedrevision")) {
        if (!e->installed) return "";
        snprintf(tmp, n, "%ld", e->installed_revision); return tmp;
    }
    if (!strcmp(f, "summary")) return e->summary;
    if (!strcmp(f, "category")) return e->category;
    if (!strcmp(f, "arch")) return e->arch;
    if (!strcmp(f, "abi")) return e->abi;
    if (!strcmp(f, "kind")) return e->kind;
    if (!strcmp(f, "size")) { snprintf(tmp, n, "%ld", e->size); return tmp; }
    if (!strcmp(f, "requires")) return e->requires;
    if (!strcmp(f, "canupgrade")) return v->up ? "1" : "0";
    if (!strcmp(f, "canrollback")) return v->back ? "1" : "0";
    if (!strcmp(f, "compatibility")) return v->compat;
    if (!strcmp(f, "requirements")) return v->reqs;
    return "";
}

int rx_format_entry(const pkg_entry *e, int can_upgrade, int can_rollback,
                    const char *compatibility, const char *requirements,
                    const char *field, char *out, size_t n)
{
    entry_view v = { e, can_upgrade, can_rollback, compatibility, requirements };
    return format(&v, entry_get, entry_fields, field, out, n);
}

static const char *job_get(const void *obj, const char *f, char *tmp, size_t n)
{
    const rx_job *j = (const rx_job *)obj;
    if (!strcmp(f, "id")) { snprintf(tmp, n, "%ld", j->id); return tmp; }
    if (!strcmp(f, "op")) return j->op;
    if (!strcmp(f, "package")) return j->package;
    if (!strcmp(f, "origin")) return j->origin;
    if (!strcmp(f, "state")) return rx_statename(j->state);
    if (!strcmp(f, "phase")) return j->phase;
    if (!strcmp(f, "done")) { snprintf(tmp, n, "%lu", j->done); return tmp; }
    if (!strcmp(f, "total")) { snprintf(tmp, n, "%lu", j->total); return tmp; }
    if (!strcmp(f, "cancancel")) return j->can_cancel && !rx_final(j->state) ? "1" : "0";
    if (!strcmp(f, "error")) return j->error;
    if (!strcmp(f, "message")) return j->message;
    return "";
}

int rx_format_job(const rx_job *j, const char *field, char *out, size_t n)
{
    return format(j, job_get, job_fields, field, out, n);
}

int rx_format_updates(const pkg_updates *u, char *out, size_t n)
{
    size_t k; int i, over = 0;
    k = (size_t)snprintf(out, n, "catalogue %s\nupdated %s\nstale %d\ninstalled %d\nnewer %d\nready %d\nblocked %d\npackages ",
                         pkg_cat_word(u->catalogue), u->catalogue_when, u->catalogue_stale,
                         u->installed, u->newer, u->ready, u->blocked);
    if (k >= n) return -1;
    for (i = 0; i < u->n; i++) {
        size_t l;
        if (!u->rows[i].newer) continue;
        l = strlen(u->rows[i].id);
        if (k + l + 2 >= n) { over = 1; break; }
        if (out[k - 1] != ' ') out[k++] = ' ';
        memcpy(out + k, u->rows[i].id, l + 1); k += l;
    }
    while (k && out[k - 1] == ' ') out[--k] = 0;
    return over ? -1 : 0;
}
