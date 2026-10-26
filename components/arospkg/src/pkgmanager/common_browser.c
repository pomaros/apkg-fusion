/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#ifdef APKG_FUSION
#include "apkg.h"
#include "job.h"
#include "pkg_target.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* UI rows are a display projection. Refuse an unrepresentable field rather
 * than truncate an identity and use that truncated name in an operation. */
pkg_status pm_common_query(const char *root, const char *term, pkg_entries *out,
                           pkg_err *error) {
    apkg_open_options o = {0};
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    apkg_status code;
    size_t i;
    pkg_entries fresh = {0};
    memset(out, 0, sizeof *out);
    o.contract_version = 1;
    o.struct_size = sizeof o;
    o.root.data = root;
    o.root.size = strlen(root);
    code = apkg_open(&o, &c, NULL);
    if (code)
        goto done;
    code = apkg_snapshot_read(c, &s, NULL);
    if (code)
        goto done;
    if (apkg_snapshot_root(s)->state != APKG_ROOT_READY) {
        code = APKG_POLICY;
        goto done;
    }
    uint64_t count = apkg_snapshot_installed_count(s);
    if (count > INT_MAX) {
        code = APKG_LIMIT;
        goto done;
    }
    fresh.v = calloc(count ? count : 1, sizeof *fresh.v);
    if (!fresh.v) {
        code = APKG_NOMEM;
        goto done;
    }
    fresh.cap = (int)count;
    for (i = 0; i < count; i++) {
        const apkg_installed_view *v;
        struct pkg_target target;
        pkg_entry *r = &fresh.v[fresh.n];
        code = apkg_snapshot_installed_at(s, i, &v, NULL);
        if (code)
            goto done;
        if (term && *term && !strstr(v->name.value.data, term))
            continue;
        if (v->name.value.size >= sizeof r->id ||
            v->version.value.size >= sizeof r->version ||
            v->revision.value > LONG_MAX ||
            pkg_target_parse(v->target.value.data, &target, NULL, 0)) {
            code = APKG_LIMIT;
            goto done;
        }
        memcpy(r->id, v->name.value.data, v->name.value.size + 1);
        memcpy(r->version, v->version.value.data, v->version.value.size + 1);
        memcpy(r->installed_version, r->version, strlen(r->version) + 1);
        r->revision = r->installed_revision = (long)v->revision.value;
        snprintf(r->arch, sizeof r->arch, "%s", target.cpu);
        snprintf(r->abi, sizeof r->abi, "%s", target.abi);
        snprintf(r->kind, sizeof r->kind, "package");
        snprintf(r->summary, sizeof r->summary,
                 "Installed; qualification not asserted");
        r->installed = 1;
        r->size = -1;
        fresh.n++;
    }
    *out = fresh;
    memset(&fresh, 0, sizeof fresh);
done:
    free(fresh.v);
    apkg_snapshot_free(s);
    apkg_close(c);
    if (error) {
        memset(error, 0, sizeof *error);
        error->status = code ? PKG_E_IO : PKG_OK;
        if (code)
            snprintf(error->summary, sizeof error->summary,
                     "common root query refused (code %d)", (int)code);
    }
    return code ? PKG_E_IO : PKG_OK;
}
#endif
