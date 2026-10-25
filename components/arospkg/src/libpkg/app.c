/* The application description (RFC 0002): reading, validating and writing
 * it. Pure logic; see app.h.
 *
 * One source of truth. The author writes [launch] in the package manifest;
 * apkg-pack checks it against the drawer and puts application.json into the
 * archive; the catalogue carries the same table; apkg writes the file into
 * the drawer at installation when the archive has none (a third-party
 * archive) and records the table in the registry so a rollback can write it
 * again. The runtime reads only the drawer's file, so an application unpacked
 * by hand starts the same way and losing the registry does not stop it. */
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int app_path_ok(const char *rel, char *why, size_t n)
{
    const char *p, *seg;
    if (!rel || !*rel) { snprintf(why, n, "the path is empty"); return -1; }
    if (strlen(rel) >= 256) { snprintf(why, n, "the path is longer than 255 characters"); return -1; }
    if (rel[0] == '/') { snprintf(why, n, "\"%s\" is absolute; give it relative to the drawer", rel); return -1; }
    for (p = rel; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == ':') { snprintf(why, n, "\"%s\" names a volume (':'); give it relative to the drawer", rel); return -1; }
        if (ch == '\\') { snprintf(why, n, "\"%s\" uses '\\'; AROS paths use '/'", rel); return -1; }
        if (ch < 0x20 || ch >= 0x7f || ch == '"') {
            snprintf(why, n, "the path has a character the description cannot carry (control, '\"' or non-ASCII)");
            return -1;
        }
    }
    /* Component by component: ".." and an empty component ("a//b", a
       trailing '/') are both ways of leaving or blurring the drawer. "."
       alone is the drawer itself and is allowed only as the whole path. */
    for (seg = rel; ; ) {
        size_t k = strcspn(seg, "/");
        if (k == 0) { snprintf(why, n, "\"%s\" has an empty component", rel); return -1; }
        if (k == 2 && seg[0] == '.' && seg[1] == '.') {
            snprintf(why, n, "\"%s\" leads out of the drawer ('..')", rel); return -1;
        }
        if (k == 1 && seg[0] == '.' && strcmp(rel, ".") != 0) {
            snprintf(why, n, "\"%s\" has a '.' component; write the path without it", rel); return -1;
        }
        if (!seg[k]) break;
        seg += k + 1;
    }
    return 0;
}

/* The directory part of a relative path: "bin" for "bin/Prog", "." for
   "Prog". */
static void rel_dirname(const char *rel, char *out, size_t n)
{
    const char *slash = strrchr(rel, '/');
    if (!slash) snprintf(out, n, ".");
    else snprintf(out, n, "%.*s", (int)(slash - rel), rel);
}

int launch_valid(const pkg_launch *l, char *why, size_t n)
{
    char w[200], dir[256];
    if (l->mode != PKG_LAUNCH_WORKBENCH && l->mode != PKG_LAUNCH_SHELL) {
        snprintf(why, n, "launch: mode must be \"workbench\" or \"shell\""); return -1;
    }
    if (app_path_ok(l->executable, w, sizeof w) != 0) {
        snprintf(why, n, "launch.executable: %s", w); return -1;
    }
    if (strcmp(l->executable, ".") == 0) {
        snprintf(why, n, "launch.executable: names the drawer, not a program"); return -1;
    }
    if (strlen(l->executable) > 5 && strcmp(l->executable + strlen(l->executable) - 5, ".info") == 0) {
        snprintf(why, n, "launch.executable: \"%s\" is an icon; name the program beside it", l->executable);
        return -1;
    }
    if (app_path_ok(l->workdir, w, sizeof w) != 0) {
        snprintf(why, n, "launch.working_directory: %s", w); return -1;
    }
    /* Workbench starts a program in the drawer that holds it, and nothing
       can change that without bypassing the icon, which is the point of
       the mode. A description saying otherwise could not be honoured. */
    rel_dirname(l->executable, dir, sizeof dir);
    if (l->mode == PKG_LAUNCH_WORKBENCH && strcmp(l->workdir, dir) != 0) {
        snprintf(why, n, "launch.working_directory: a Workbench start runs in the program's own "
                 "drawer (\"%s\"); leave working_directory out or use shell mode", dir);
        return -1;
    }
    return 0;
}

static int member_string(const char *js, const js_tok *t, int ntok, int obj,
                         const char *name, char *out, size_t n, int *present)
{
    int v = js_member(js, t, ntok, obj, name);
    *present = v >= 0;
    if (v < 0) { out[0] = 0; return 0; }
    if (t[v].type != JS_STR) return -1;
    if ((size_t)(t[v].end - t[v].start) >= n) return -1;
    return js_str(js, t, v, out, n);
}

int launch_from_object(const char *js, const js_tok *t, int ntok, int obj,
                       pkg_launch *l, char *why, size_t n)
{
    char mode[16];
    int have, c;
    memset(l, 0, sizeof *l);
    if (obj < 0) return 0;
    if (t[obj].type != JS_OBJ) { snprintf(why, n, "launch is not a table"); return -1; }
    /* Unknown keys are refused, not skipped: a later field this client
       ignored could change what "start it" means (an argument, another
       program), and starting it anyway would be guessing. Duplicates too:
       the reader would take the first and the author may have meant the
       second. */
    for (c = js_child(t, ntok, obj); c >= 0; ) {
        int val = js_sibling(t, ntok, c), d;
        char key[32] = "";
        js_str(js, t, c, key, sizeof key);
        if (strcmp(key, "executable") && strcmp(key, "mode") && strcmp(key, "working_directory")) {
            snprintf(why, n, "launch has a key this client does not know: \"%s\"", key);
            return -1;
        }
        /* Keys and values are siblings: key, value, key, value. */
        for (d = val >= 0 ? js_sibling(t, ntok, val) : -1; d >= 0; ) {
            char k2[32] = "";
            int dv = js_sibling(t, ntok, d);
            js_str(js, t, d, k2, sizeof k2);
            if (strcmp(key, k2) == 0) { snprintf(why, n, "launch has \"%s\" twice", key); return -1; }
            d = dv >= 0 ? js_sibling(t, ntok, dv) : -1;
        }
        c = val >= 0 ? js_sibling(t, ntok, val) : -1;
    }
    if (member_string(js, t, ntok, obj, "executable", l->executable, sizeof l->executable, &have) != 0 || !have) {
        snprintf(why, n, "launch.executable is missing or not a string"); return -1;
    }
    if (member_string(js, t, ntok, obj, "mode", mode, sizeof mode, &have) != 0 || !have) {
        snprintf(why, n, "launch.mode is missing or not a string"); return -1;
    }
    if (strcmp(mode, "workbench") == 0) l->mode = PKG_LAUNCH_WORKBENCH;
    else if (strcmp(mode, "shell") == 0) l->mode = PKG_LAUNCH_SHELL;
    else { snprintf(why, n, "launch.mode \"%s\" is not \"workbench\" or \"shell\"", mode); return -1; }
    if (member_string(js, t, ntok, obj, "working_directory", l->workdir, sizeof l->workdir, &have) != 0) {
        snprintf(why, n, "launch.working_directory is not a string"); return -1;
    }
    if (!have) {
        /* The default is where Workbench would put it: the program's
           drawer. For a program at the top of the drawer that is ".". */
        rel_dirname(l->executable, l->workdir, sizeof l->workdir);
    }
    if (launch_valid(l, why, n) != 0) { l->mode = PKG_LAUNCH_NONE; return -1; }
    return 1;
}

void launch_emit(js_out *o, const pkg_launch *l)
{
    js_key(o, "launch"); js_raw(o, " { ");
    js_key(o, "executable"); js_vstr(o, l->executable); js_raw(o, ", ");
    js_key(o, "mode"); js_vstr(o, l->mode == PKG_LAUNCH_SHELL ? "shell" : "workbench"); js_raw(o, ", ");
    js_key(o, "working_directory"); js_vstr(o, l->workdir);
    js_raw(o, " }");
}

char *app_text(const char *id, const char *version, long revision,
               const char *arch, const char *abi,
               const req_ent *reqs, int nreq, const pkg_launch *l)
{
    size_t cap = 2048 + (size_t)nreq * 160;
    char *buf = (char *)malloc(cap);
    js_out o;
    int i;
    if (!buf) return NULL;
    js_out_init(&o, buf, cap);
    js_raw(&o, "{\n  "); js_key(&o, "schema"); js_vlong(&o, PKG_APP_SCHEMA);
    js_raw(&o, ",\n  "); js_key(&o, "id"); js_vstr(&o, id);
    js_raw(&o, ",\n  "); js_key(&o, "version"); js_vstr(&o, version);
    if (revision > 0) { js_raw(&o, ",\n  "); js_key(&o, "revision"); js_vlong(&o, revision); }
    js_raw(&o, ",\n  "); js_key(&o, "targets"); js_raw(&o, " [ { ");
    js_key(&o, "os"); js_vstr(&o, "aros"); js_raw(&o, ", ");
    js_key(&o, "arch"); js_vstr(&o, arch); js_raw(&o, ", ");
    js_key(&o, "abi"); js_vstr(&o, abi ? abi : ""); js_raw(&o, " } ]");
    js_raw(&o, ",\n  "); js_key(&o, "requires_system"); js_raw(&o, " [");
    for (i = 0; i < nreq; i++) {
        js_raw(&o, i ? ", { " : " { ");
        js_key(&o, "type"); js_vstr(&o, reqs[i].type);
        js_raw(&o, ", "); js_key(&o, "id"); js_vstr(&o, reqs[i].id);
        if (reqs[i].min_version > 0) { js_raw(&o, ", "); js_key(&o, "min_version"); js_vlong(&o, reqs[i].min_version); }
        js_raw(&o, " }");
    }
    js_raw(&o, nreq ? " ]" : "]");
    js_raw(&o, ",\n  "); launch_emit(&o, l);
    js_raw(&o, "\n}\n");
    if (o.fail) { free(buf); return NULL; }
    return buf;
}

const char *app_binary_arch(const unsigned char *h, size_t n)
{
    if (n >= 4 && h[0] == 0 && h[1] == 0 && h[2] == 0x03 && h[3] == 0xf3) return "m68k";
    if (n >= 20 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') {
        /* e_machine sits at 18 in both ELF classes, in the file's byte
           order (EI_DATA: 1 little, 2 big). */
        unsigned m = h[5] == 2 ? (unsigned)(h[18] << 8 | h[19]) : (unsigned)(h[19] << 8 | h[18]);
        switch (m) {
        case 3:   return "i386";
        case 62:  return "x86_64";
        case 183: return "aarch64";
        case 40:  return "arm";
        case 20:  return "ppc";
        }
    }
    return "";
}

int app_parse(const char *text, size_t len, app_desc *a,
              req_ent *reqs, int maxreq, int *nreq, char *why, size_t n)
{
    js_tok *t = NULL;
    int ntok, v, tg, have;
    long schema;
    char w[200];
    memset(a, 0, sizeof *a);
    *nreq = 0;
    if (len > (size_t)PKG_APP_MAXSIZE) { snprintf(why, n, "%s is larger than 64 KiB", PKG_APP_FILE); return -1; }
    ntok = js_parse_alloc(text, len, &t, 4096);
    if (ntok <= 0 || t[0].type != JS_OBJ) { free(t); snprintf(why, n, "%s is not valid JSON", PKG_APP_FILE); return -1; }
    v = js_member(text, t, ntok, 0, "schema");
    schema = v >= 0 && t[v].type == JS_PRIM ? js_long(text, t, v, -1) : -1;
    if (schema != PKG_APP_SCHEMA) {
        free(t);
        snprintf(why, n, "%s has schema %ld; this client reads schema %d", PKG_APP_FILE, schema, PKG_APP_SCHEMA);
        return -1;
    }
    if (member_string(text, t, ntok, 0, "id", a->id, sizeof a->id, &have) != 0 || !have || !a->id[0]) {
        free(t); snprintf(why, n, "%s: id is missing", PKG_APP_FILE); return -1;
    }
    if (member_string(text, t, ntok, 0, "name", a->name, sizeof a->name, &have) != 0) {
        free(t); snprintf(why, n, "%s: name is not a string", PKG_APP_FILE); return -1;
    }
    if (!a->name[0]) snprintf(a->name, sizeof a->name, "%s", a->id);
    if (member_string(text, t, ntok, 0, "version", a->version, sizeof a->version, &have) != 0 || !have) {
        free(t); snprintf(why, n, "%s: version is missing", PKG_APP_FILE); return -1;
    }
    a->revision = js_long(text, t, js_member(text, t, ntok, 0, "revision"), 0);
    tg = js_member(text, t, ntok, 0, "targets");
    if (tg < 0 || t[tg].type != JS_ARR || t[tg].size < 1) {
        free(t); snprintf(why, n, "%s: targets is missing or empty", PKG_APP_FILE); return -1;
    }
    a->ntargets = t[tg].size;
    {
        int el = js_elem(t, ntok, tg, 0);
        if (el < 0 || t[el].type != JS_OBJ
            || member_string(text, t, ntok, el, "arch", a->arch, sizeof a->arch, &have) != 0 || !have
            || member_string(text, t, ntok, el, "abi", a->abi, sizeof a->abi, &have) != 0) {
            free(t); snprintf(why, n, "%s: targets[0] needs arch and abi", PKG_APP_FILE); return -1;
        }
    }
    /* One drawer, one program: per-target executables are deferred in the
       RFC, so a description naming several targets cannot say which binary
       is which. */
    if (a->ntargets > 1) {
        free(t); snprintf(why, n, "%s names %d targets; one is supported", PKG_APP_FILE, a->ntargets); return -1;
    }
    if (js_member(text, t, ntok, 0, "requires_system") >= 0
        && req_parse(text, t, ntok, 0, reqs, maxreq, nreq, w, sizeof w) != 0) {
        free(t); snprintf(why, n, "%s: %s", PKG_APP_FILE, w); return -1;
    }
    v = js_member(text, t, ntok, 0, "launch");
    if (v < 0) { free(t); snprintf(why, n, "%s has no launch table", PKG_APP_FILE); return -1; }
    if (launch_from_object(text, t, ntok, v, &a->launch, w, sizeof w) != 1) {
        free(t); snprintf(why, n, "%s: %s", PKG_APP_FILE, w); return -1;
    }
    free(t);
    return 0;
}
