/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * TEST DOUBLE ONLY: no filesystem, signatures, planning or mutation. */
#include "apkg.h"
#include <stdlib.h>
#include <string.h>
struct apkg_ctx { int unused; };
struct apkg_snapshot { apkg_root_view root; };
struct apkg_error { apkg_error_view view; };
struct apkg_event_copy { apkg_event view; char *subject, *message, *unit, *evidence; };

apkg_status apkg_open(const apkg_open_options *o, apkg_ctx **out, apkg_error **err)
{
    if (err) *err = NULL;
    if (!out) return APKG_INVALID;
    *out = NULL;
    if (!o || o->contract_version != APKG_CONTRACT_VERSION || o->struct_size != sizeof *o)
        return APKG_UNSUPPORTED;
    *out = calloc(1, sizeof **out);
    return *out ? APKG_OK : APKG_NOMEM;
}
void apkg_close(apkg_ctx *c) { free(c); }
apkg_status apkg_snapshot_read(apkg_ctx *c, apkg_snapshot **out, apkg_error **err)
{
    if (err) *err = NULL;
    if (!out) return APKG_INVALID;
    *out = NULL;
    if (!c) return APKG_INVALID;
    *out = calloc(1, sizeof **out); /* ABSENT and UNKNOWN are zero */
    return *out ? APKG_OK : APKG_NOMEM;
}
const apkg_root_view *apkg_snapshot_root(const apkg_snapshot *s) { return &s->root; }
void apkg_snapshot_free(apkg_snapshot *s) { free(s); }
uint64_t apkg_snapshot_installed_count(const apkg_snapshot *s) { (void)s; return 0; }
apkg_status apkg_snapshot_installed_at(const apkg_snapshot *s, uint64_t i,
                                      const apkg_installed_view **out, apkg_error **err)
{
    (void)s; (void)i;
    if (err) *err = NULL;
    if (!out) return APKG_INVALID;
    *out = NULL;
    return APKG_NOT_FOUND;
}
const apkg_error_view *apkg_error_get(const apkg_error *e) { return &e->view; }
void apkg_error_free(apkg_error *e) { free(e); }
static char *copy_text(apkg_text src)
{
    char *p = malloc(src.size + 1);
    if (!p) return NULL;
    if (src.size) memcpy(p, src.data, src.size);
    p[src.size] = 0;
    return p;
}
apkg_status apkg_event_clone(const apkg_event *e, apkg_event_copy **out)
{
    apkg_event_copy *c;
    if (!out) return APKG_INVALID;
    *out = NULL;
    if (!e) return APKG_INVALID;
    c = calloc(1, sizeof *c);
    if (!c) return APKG_NOMEM;
    c->view = *e;
    c->subject = copy_text(e->subject); c->message = copy_text(e->message);
    c->unit = copy_text(e->unit); c->evidence = copy_text(e->total.evidence);
    if (!c->subject || !c->message || !c->unit || !c->evidence) {
        apkg_event_free(c); return APKG_NOMEM;
    }
    c->view.subject.data = c->subject; c->view.message.data = c->message;
    c->view.unit.data = c->unit; c->view.total.evidence.data = c->evidence;
    *out = c;
    return APKG_OK;
}
const apkg_event *apkg_event_get(const apkg_event_copy *c) { return &c->view; }
void apkg_event_free(apkg_event_copy *c)
{
    if (!c) return;
    free(c->subject); free(c->message); free(c->unit); free(c->evidence); free(c);
}
