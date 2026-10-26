/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include <stdlib.h>
#include <string.h>
apkg_status apkg_cancel_create(apkg_cancel **out) {
    if (!out)
        return APKG_INVALID;
    *out = calloc(1, sizeof **out);
    return *out ? APKG_OK : APKG_NOMEM;
}
void apkg_cancel_request(apkg_cancel *c) {
    if (c)
        __atomic_store_n(&c->requested, 1u, __ATOMIC_RELAXED);
}
void apkg_cancel_free(apkg_cancel *c) { free(c); }
int ak_cancelled(const apkg_observer *o) {
    return o && o->cancel && __atomic_load_n(&o->cancel->requested, __ATOMIC_RELAXED);
}
apkg_status apkg_event_clone(const apkg_event *e, apkg_event_copy **out) {
    apkg_event_copy *c;
    apkg_text *dst[4];
    const apkg_text *src[4];
    size_t i;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!e)
        return APKG_INVALID;
    c = calloc(1, sizeof *c);
    if (!c)
        return APKG_NOMEM;
    c->view = *e;
    dst[0] = &c->view.subject;
    dst[1] = &c->view.message;
    dst[2] = &c->view.unit;
    dst[3] = &c->view.total.evidence;
    src[0] = &e->subject;
    src[1] = &e->message;
    src[2] = &e->unit;
    src[3] = &e->total.evidence;
    for (i = 0; i < 4; i++) {
        if (src[i]->size > AK_MAX || (!src[i]->data && src[i]->size)) {
            apkg_event_free(c);
            return APKG_INVALID;
        }
        c->strings[i] = ak_copy(src[i]->data, src[i]->size);
        if (!c->strings[i]) {
            apkg_event_free(c);
            return APKG_NOMEM;
        }
        dst[i]->data = c->strings[i];
    }
    *out = c;
    return APKG_OK;
}
const apkg_event *apkg_event_get(const apkg_event_copy *c) { return &c->view; }
void apkg_event_free(apkg_event_copy *c) {
    size_t i;
    if (c) {
        for (i = 0; i < 4; i++)
            free(c->strings[i]);
        free(c);
    }
}
