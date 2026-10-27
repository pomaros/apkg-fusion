/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
/* Self-update of a client installed as a managed package.
 *
 * The client is a package like any other, so its update is the shared
 * channel plan, confirmation, journal and recovery; there is no second
 * update engine. This module only decides which package and which candidate:
 * the installed package that owns the running executable, whose bytes must
 * still be the recorded ones, and the newest trusted candidate of the same
 * authority, name and target that still carries the executable at the same
 * path. Copies outside the root and an unsettled legacy self-update are
 * refused, not guessed. */
#include "apkg_client.h"
#include "internal.h"
#include "pkg_fs.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* Where an arospkg self-update kept its state. Hosts never had one; the test
 * suite names its own files through the environment. */
static const char *legacy_path(const char *variable, const char *aros) {
#ifdef __AROS__
    (void)variable;
    return aros;
#else
    (void)aros;
    const char *p = getenv(variable);
    return p && *p ? p : NULL;
#endif
}
static int text_is(apkg_text t, const char *s) {
    return t.size == strlen(s) && !memcmp(t.data, s, t.size);
}
static int copy_text(apkg_text t, char *out, size_t size) {
    if (t.size >= size)
        return -1;
    memcpy(out, t.data, t.size);
    out[t.size] = 0;
    return 0;
}
/* The canonical path of a file: its directory resolved, its name kept. */
static int canonical_file(const char *path, char *out) {
    char dir[AK_PATH], resolved[AK_PATH];
    const char *slash = strrchr(path, '/'), *colon = strrchr(path, ':');
    const char *cut = slash > colon ? slash : colon;
    if (strlen(path) >= AK_PATH)
        return -1;
    if (!cut) {
        strcpy(dir, ".");
        cut = path - 1;
    } else {
        size_t n = (size_t)(cut - path) + (cut == colon || cut == path);
        memcpy(dir, path, n);
        dir[n] = 0;
    }
    if (!cut[1] || pkg_fs_canonical_dir(dir, resolved, sizeof resolved))
        return -1;
    return ak_path(out, resolved, cut + 1);
}
/* The descriptor entry at `path`: its sha256, or -1 when absent/not a file. */
static int file_sha(apkg_bytes descriptor, const char *path, char sha[65]) {
    ak_json j = {0};
    int found = -1;
    if (ak_json_parse(descriptor.data, descriptor.size, &j))
        return -1;
    int files = ak_member(&j, ak_member(&j, 0, "body"), "files");
    if (files >= 0 && j.tokens[files].type == '[')
        for (size_t i = (size_t)files + 1; i < j.tokens[files].next;
             i = j.tokens[i].next)
            if (ak_equal(&j, ak_member(&j, (int)i, "path"), path)) {
                if (ak_equal(&j, ak_member(&j, (int)i, "type"), "file") &&
                    !ak_string(&j, ak_member(&j, (int)i, "sha256"), sha, 65))
                    found = 0;
                break;
            }
    ak_json_free(&j);
    return found;
}
static int candidate_order(const apkg_channel_entry_view *a,
                           const apkg_channel_entry_view *b) {
    char x[256], y[256];
    if (copy_text(a->version, x, sizeof x) || copy_text(b->version, y, sizeof y))
        return 2;
    int order = ak_version_compare(x, y);
    if (order == 0)
        order = a->revision > b->revision ? 1 : a->revision < b->revision ? -1 : 2;
    return order;
}
apkg_status apkg_client_self_update(const char *root, const char *endpoint,
                                    const char *identity,
                                    const char *executable, int check_only,
                                    apkg_confirm_fn confirm, void *user,
                                    const apkg_observer *observer,
                                    char **report, apkg_result **out,
                                    apkg_error **e) {
    apkg_snapshot *s = NULL;
    apkg_channel *ch = NULL;
    apkg_inspection *owner = NULL;
    char *selection = NULL;
    char root_path[AK_PATH], exe[AK_PATH], rel[AK_PATH], inner[AK_PATH],
        name[256], installed_version[256], candidate_version[256];
    char recorded[65], live[65], wanted[65];
    const apkg_inspection_view *view = NULL;
    const apkg_channel_entry_view *best = NULL;
    uint64_t best_index = 0, size;
    unsigned char head[64];
    size_t head_size, n;
    ak_buf b = {0};
    apkg_status code;
    if (e)
        *e = NULL;
    if (out)
        *out = NULL;
    if (report)
        *report = NULL;
    if (!root || !endpoint || !identity || !executable || !report ||
        (!check_only && (!confirm || !out)))
        return APKG_INVALID;
    {
        const char *journal = legacy_path("APKG_LEGACY_SELF_UPDATE_JOURNAL",
                                          "ENVARC:arospkg/self-update.json");
        int present = journal ? ak_exists(journal) : 0;
        if (present)
            return ak_fail(e, present < 0 ? APKG_IO : APKG_RECOVERY_REQUIRED,
                           "an earlier arospkg self-update is unsettled; "
                           "settle it with that arospkg first",
                           journal);
    }
    code = apkg_client_channel_refresh(root, endpoint, identity, 0, observer,
                                       &s, &ch, e);
    if (code)
        return code;
    if (apkg_snapshot_root(s)->state != APKG_ROOT_READY) {
        code = apkg_snapshot_root(s)->state == APKG_ROOT_PENDING
                   ? APKG_RECOVERY_REQUIRED
                   : APKG_POLICY;
        ak_fail(e, code, "the root is not ready", root);
        goto done;
    }
    /* Which installed package owns the running client. */
    if (pkg_fs_canonical_dir(root, root_path, sizeof root_path) ||
        canonical_file(executable, exe)) {
        code = ak_fail(e, APKG_NOT_FOUND, "cannot resolve the running client",
                       executable);
        goto done;
    }
    n = strlen(root_path);
    if (strncmp(exe, root_path, n) ||
        (root_path[n - 1] != '/' && root_path[n - 1] != ':' && exe[n] != '/')) {
        code = ak_fail(e, APKG_NOT_FOUND,
                       "the running client is outside this root", exe);
        goto done;
    }
    strcpy(rel, exe + n + (root_path[n - 1] != '/' && root_path[n - 1] != ':'));
    for (uint64_t i = 0; i < apkg_snapshot_installed_count(s) && !view; i++) {
        const apkg_installed_view *row;
        code = apkg_snapshot_installed_at(s, i, &row, e);
        if (code)
            goto done;
        if (copy_text(row->name.value, name, sizeof name))
            continue;
        size_t k = strlen(name);
        if (strncmp(rel, name, k) || rel[k] != '/')
            continue;
        strcpy(inner, rel + k + 1);
        code = apkg_snapshot_inspect(s, i, &owner, e);
        if (code)
            goto done;
        if (!file_sha(apkg_inspection_get(owner)->descriptor, inner, recorded))
            view = apkg_inspection_get(owner);
        else {
            apkg_inspection_free(owner);
            owner = NULL;
        }
    }
    if (!view) {
        code = ak_fail(e, APKG_NOT_FOUND,
                       "the running client is not a file of an installed "
                       "package; adopt it first",
                       rel);
        goto done;
    }
    {
        const char *copies = legacy_path("APKG_LEGACY_SELF_UPDATE_COPIES",
                                         "ENVARC:arospkg/copies.json");
        unsigned char *data = NULL;
        size_t length = 0;
        int present = copies ? ak_exists(copies) : 0;
        if (present < 0 ||
            (present && ak_read(copies, &data, &length, AK_MAX / 64))) {
            code = ak_fail(e, APKG_IO, "cannot read recorded copies", copies);
            goto done;
        }
        if (present && length > 2) { /* anything beyond {} or [] */
            free(data);
            code = ak_fail(e, APKG_UNSUPPORTED,
                           "copies of the client are recorded; updating "
                           "copies needs the multi-root journal",
                           copies);
            goto done;
        }
        free(data);
    }
    code = ak_stream_hash(exe, &size, live, head, &head_size);
    if (code) {
        ak_fail(e, code, "cannot read the running client", exe);
        goto done;
    }
    if (strcmp(live, recorded)) {
        code = ak_fail(e, APKG_CONFLICT,
                       "the running client differs from its installed record; "
                       "repair it first",
                       exe);
        goto done;
    }
    /* The newest trusted candidate of the same identity. */
    for (uint64_t i = 0; i < apkg_channel_get(ch)->entry_count; i++) {
        const apkg_channel_entry_view *row;
        apkg_candidate_view state;
        code = apkg_channel_entry_at(ch, i, &row, e);
        if (!code)
            code = apkg_channel_compare(s, ch, i, &state, e);
        if (code)
            goto done;
        if (state.state != APKG_CANDIDATE_NEWER ||
            row->author_trust != APKG_TRUST_ACCEPTED || !row->target_matches ||
            row->authority.size != view->authority.size ||
            memcmp(row->authority.data, view->authority.data,
                   row->authority.size) ||
            copy_text(row->name, name, sizeof name) ||
            !text_is(view->name, name) ||
            row->target.size != view->target.size ||
            memcmp(row->target.data, view->target.data, row->target.size))
            continue;
        if (best) {
            int order = candidate_order(row, best);
            if (order == 2) {
                code = ak_fail(e, APKG_CONFLICT,
                               "newer client releases have no defined order",
                               name);
                goto done;
            }
            if (order < 0)
                continue;
        }
        best = row;
        best_index = i;
    }
    copy_text(view->name, name, sizeof name);
    copy_text(view->version, installed_version, sizeof installed_version);
    b.max = AK_MAX / 64;
    if (!best) {
        ak_buf_add(&b, "self-update: %s %s is current\nexecutable=%s\n", name,
                   installed_version, exe);
        code = b.bad ? (apkg_status)b.bad : APKG_OK;
        goto done;
    }
    if (file_sha(best->descriptor, inner, wanted)) {
        code = ak_fail(e, APKG_UNSUPPORTED,
                       "the newer release does not carry the client at the "
                       "same path",
                       inner);
        goto done;
    }
    copy_text(best->version, candidate_version, sizeof candidate_version);
    ak_buf_add(&b,
               "self-update: %s %s -> %s revision %" PRIu64
               "\nexecutable=%s\nrestart=required\n",
               name, installed_version, candidate_version, best->revision, exe);
    if (b.bad) {
        code = (apkg_status)b.bad;
        goto done;
    }
    if (check_only)
        goto done;
    code = apkg_client_channel_selection(ch, best_index, &selection, e);
    if (code)
        goto done;
    /* Bound to the observed snapshot: a changed channel or root refuses. */
    apkg_inspection_free(owner);
    owner = NULL;
    apkg_channel_free(ch);
    ch = NULL;
    apkg_snapshot_free(s);
    s = NULL;
    code = apkg_client_channel_apply(root, endpoint, selection, 0, confirm,
                                     user, observer, out, e);
    if (code)
        goto done;
    /* An upgrade keeps an edited file. If the client changed between the check
     * and the transaction, it is kept and the update did not reach it. */
    code = ak_stream_hash(exe, &size, live, head, &head_size);
    if (!code && strcmp(live, wanted))
        code = ak_fail(e, APKG_CONFLICT,
                       "the package was updated but the running client was "
                       "kept as an edited file",
                       exe);
done:
    apkg_client_text_free(selection);
    apkg_inspection_free(owner);
    apkg_channel_free(ch);
    apkg_snapshot_free(s);
    if (b.data && (code == APKG_OK || code == APKG_CONFLICT))
        *report = b.data;
    else
        free(b.data);
    return code;
}
