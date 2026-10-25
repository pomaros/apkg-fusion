/* The demonstration storefront file. See demo.h for what it is and is not.
 *
 * The file is local and is read once at startup, never fetched: pictures in
 * a future catalogue would have to be downloaded off the window's task, and
 * this demo does not pretend to have solved that. */
#include "demo.h"
#include "../libpkg/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static demo_media media[DEMO_MAXMEDIA];
static int nmedia;
static demo_offer offers[DEMO_MAXOFFERS];
static int noffers;
static int loaded;
static char banner[80] = "Demo -- purchasing is not available yet";

void demo_clear(void) { nmedia = noffers = loaded = 0; }
int  demo_loaded(void) { return loaded; }
const char *demo_banner(void) { return banner; }
int  demo_noffers(void) { return noffers; }
demo_offer *demo_offer_at(int i) { return i >= 0 && i < noffers ? &offers[i] : NULL; }

/* By identity, never by a field: a catalogue entry could carry any kind,
   and must not be taken for an offer (or an offer for a package). */
int demo_is_offer(const pkg_entry *e)
{
    int i;
    for (i = 0; e && i < noffers; i++) if (&offers[i].e == e) return 1;
    return 0;
}

demo_offer *demo_offer_of(const pkg_entry *e)
{
    int i;
    for (i = 0; i < noffers; i++) if (&offers[i].e == e) return &offers[i];
    return NULL;
}

const demo_media *demo_media_for(const char *id)
{
    int i;
    for (i = 0; i < nmedia; i++) if (strcmp(media[i].id, id) == 0) return &media[i];
    return NULL;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || n > 256 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); return NULL;
    }
    if (!(buf = (char *)malloc((size_t)n + 1))) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    buf[n] = 0;
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int g_ntok;    /* the token count of the file being read */

/* libpkg's JSON reader hands strings over as written (json.h); the texts a
   person reads here may break lines with "\n", which is turned into a line
   break. Nothing else is unescaped. */
static void line_breaks(char *s)
{
    char *w = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') { *w++ = '\n'; s++; }
        else *w++ = *s;
    }
    *w = 0;
}

static void str(const char *js, js_tok *t, int obj, const char *key, char *out, size_t n)
{
    out[0] = 0;
    js_str(js, t, js_member(js, t, g_ntok, obj, key), out, n);
}

/* A path from the file, relative to its drawer: no volume, no parent, as
   for application.json (a demo file is no reason to read elsewhere). */
static int rel_path(const char *dir, const char *rel, char *out, size_t n)
{
    if (!rel[0]) { out[0] = 0; return 0; }
    if (strchr(rel, ':') || strstr(rel, "..") || rel[0] == '/') return -1;
    snprintf(out, n, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != ':' ? "/" : "", rel);
    return 0;
}

static int read_media(const char *js, js_tok *t, int ntok, int obj, const char *dir,
                      demo_media *m, char *why, size_t n)
{
    char rel[PKG_MAXPATH];
    int arr, i;
    memset(m, 0, sizeof *m);
    (void)ntok;
    str(js, t, obj, "id", m->id, sizeof m->id);
    if (!m->id[0]) { snprintf(why, n, "a media entry has no id"); return -1; }
    str(js, t, obj, "icon", rel, sizeof rel);
    if (rel_path(dir, rel, m->icon, sizeof m->icon) != 0) { snprintf(why, n, "%s: icon path leaves the drawer", m->id); return -1; }
    str(js, t, obj, "provenance", m->provenance, sizeof m->provenance);
    line_breaks(m->provenance);
    if (!m->provenance[0]) {
        /* Every picture says where it comes from; one that does not is
           refused rather than shown as if it were a real screenshot. */
        snprintf(why, n, "%s: no provenance for its pictures", m->id); return -1;
    }
    {
        char kind[16];
        str(js, t, obj, "kind", kind, sizeof kind);
        if (strcmp(kind, "screenshot") && strcmp(kind, "mockup")) {
            snprintf(why, n, "%s: kind must be \"screenshot\" or \"mockup\"", m->id); return -1;
        }
        m->mockup = strcmp(kind, "mockup") == 0;
    }
    arr = js_member(js, t, g_ntok, obj, "screenshots");
    for (i = 0; arr >= 0 && i < DEMO_MAXSHOTS; i++) {
        int el = js_elem(t, g_ntok, arr, i);
        demo_pic *p = &m->shots[m->nshots];
        if (el < 0) break;
        str(js, t, el, "file", rel, sizeof rel);
        if (!rel[0] || rel_path(dir, rel, p->file, sizeof p->file) != 0) { snprintf(why, n, "%s: screenshot %d has no usable file", m->id, i); return -1; }
        str(js, t, el, "thumb", rel, sizeof rel);
        if (!rel[0] || rel_path(dir, rel, p->thumb, sizeof p->thumb) != 0) { snprintf(why, n, "%s: screenshot %d has no usable thumb", m->id, i); return -1; }
        str(js, t, el, "caption", p->caption, sizeof p->caption);
        m->nshots++;
    }
    return 0;
}

int demo_load(const char *path, char *why, size_t n)
{
    size_t len;
    char *js, dir[PKG_MAXPATH];
    js_tok *t = NULL;
    int ntok, arr, i;
    const char *slash;

    demo_clear();
    if (!(js = read_file(path, &len))) { snprintf(why, n, "no storefront file at %s", path); return -1; }
    ntok = js_parse_alloc(js, len, &t, 1 << 16);
    if (ntok <= 0 || t[0].type != JS_OBJ) { free(t); free(js); snprintf(why, n, "%s is not valid JSON", path); return -1; }
    g_ntok = ntok;
    if (js_long(js, t, js_member(js, t, ntok, 0, "schema"), 0) != 1
        || js_member(js, t, ntok, 0, "demo") < 0) {
        free(t); free(js); snprintf(why, n, "%s is not a demo storefront of schema 1", path); return -1;
    }
    slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, ':');
    if (slash) snprintf(dir, sizeof dir, "%.*s", (int)(slash - path + (*slash == ':')), path);
    else dir[0] = 0;
    str(js, t, 0, "banner", banner, sizeof banner);
    if (!banner[0]) snprintf(banner, sizeof banner, "Demo -- purchasing is not available yet");

    arr = js_member(js, t, ntok, 0, "media");
    for (i = 0; arr >= 0 && nmedia < DEMO_MAXMEDIA; i++) {
        int el = js_elem(t, ntok, arr, i);
        if (el < 0) break;
        if (read_media(js, t, ntok, el, dir, &media[nmedia], why, n) != 0) { free(t); free(js); demo_clear(); return -1; }
        nmedia++;
    }
    arr = js_member(js, t, ntok, 0, "offers");
    for (i = 0; arr >= 0 && noffers < DEMO_MAXOFFERS; i++) {
        int el = js_elem(t, ntok, arr, i);
        demo_offer *o = &offers[noffers];
        if (el < 0) break;
        memset(o, 0, sizeof *o);
        str(js, t, el, "id", o->e.id, sizeof o->e.id);
        str(js, t, el, "name", o->name, sizeof o->name);
        str(js, t, el, "price", o->price, sizeof o->price);
        str(js, t, el, "currency", o->currency, sizeof o->currency);
        str(js, t, el, "seller", o->seller, sizeof o->seller);
        str(js, t, el, "summary", o->e.summary, sizeof o->e.summary);
        str(js, t, el, "category", o->e.category, sizeof o->e.category);
        str(js, t, el, "description", o->description, sizeof o->description);
        line_breaks(o->description);
        if (!o->e.id[0] || !o->name[0] || !o->price[0] || !o->currency[0]) {
            free(t); free(js); demo_clear();
            snprintf(why, n, "offer %d needs id, name, price and currency", i);
            return -1;
        }
        /* An offer's id must never be a real package's: it is shown beside
           the catalogue, and the prefix keeps the two apart for anyone who
           reads a log or a screenshot. */
        if (strncmp(o->e.id, "demo-", 5) != 0) {
            free(t); free(js); demo_clear();
            snprintf(why, n, "offer \"%s\": an offer id must start with \"demo-\"", o->e.id);
            return -1;
        }
        {
            char v[sizeof o->e.version];
            snprintf(v, sizeof v, "%s %s", o->price, o->currency);
            memcpy(o->e.version, v, sizeof v);
        }
        snprintf(o->e.kind, sizeof o->e.kind, "demo-offer");
        o->e.size = -1;
        noffers++;
    }
    free(t); free(js);
    loaded = 1;
    return 0;
}

int demo_png_check(const unsigned char *h, size_t n, long file_size,
                   int max_w, int max_h, int *w, int *hh, char *why, size_t whyn)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    *w = *hh = 0;
    if (file_size < 0) { snprintf(why, whyn, "the picture file is missing"); return -1; }
    if (file_size > DEMO_PIC_MAXBYTES) { snprintf(why, whyn, "the picture is larger than %ld KB", DEMO_PIC_MAXBYTES / 1024); return -1; }
    if (n < 24 || memcmp(h, sig, 8) != 0 || memcmp(h + 12, "IHDR", 4) != 0) {
        snprintf(why, whyn, "the picture is not a PNG file"); return -1;
    }
    *w = (int)((unsigned long)h[16] << 24 | (unsigned long)h[17] << 16 | (unsigned long)h[18] << 8 | h[19]);
    *hh = (int)((unsigned long)h[20] << 24 | (unsigned long)h[21] << 16 | (unsigned long)h[22] << 8 | h[23]);
    /* Truecolour only (PNG colour type 2 or 6). A palette PNG did not load
       through the picture datatype on AROS One 1.3 (2026-10-12), and Dtpic
       then left the previous picture on screen, so a mockup showed as a
       real screenshot of another program. Refused here, it shows as text. */
    if (n < 26 || (h[25] != 2 && h[25] != 6)) {
        snprintf(why, whyn, "the picture is not a truecolour PNG"); return -1;
    }
    if (*w <= 0 || *hh <= 0 || *w > max_w || *hh > max_h) {
        snprintf(why, whyn, "the picture is %dx%d; this panel shows at most %dx%d", *w, *hh, max_w, max_h);
        return -1;
    }
    return 0;
}
