/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include <inttypes.h>
#include <string.h>

static int array(const ak_json *j) {
    int body = ak_member(j, 0, "body");
    return ak_member(j, body < 0 ? 0 : body, "files");
}
static int find(const ak_json *j, int list, const char *path) {
    if (list < 0)
        return -1;
    for (size_t i = (size_t)list + 1; i < j->tokens[list].next;
         i = j->tokens[i].next)
        if (ak_equal(j, ak_member(j, (int)i, "path"), path))
            return (int)i;
    return -1;
}
static int same(const ak_json *expected, int x, const ak_json *live, int y) {
    char type[16], hash[65];
    uint64_t a = 0700, b = 0700, asize, bsize;
    if (ak_string(expected, ak_member(expected, x, "type"), type,
                  sizeof type) ||
        !ak_equal(live, ak_member(live, y, "type"), type))
        return 0;
    if (ak_member(expected, x, "mode") >= 0 &&
        ak_u64(expected, ak_member(expected, x, "mode"), &a))
        return 0;
    if (ak_u64(live, ak_member(live, y, "mode"), &b) || a != b)
        return 0;
    int attributes = ak_attributes_equal(expected, x, live, y);
    if (attributes <= 0)
        return attributes;
    if (!strcmp(type, "directory"))
        return 1;
    return !ak_string(expected, ak_member(expected, x, "sha256"), hash,
                      sizeof hash) &&
           ak_equal(live, ak_member(live, y, "sha256"), hash) &&
           !ak_u64(expected, ak_member(expected, x, "size"), &asize) &&
           !ak_u64(live, ak_member(live, y, "size"), &bsize) && asize == bsize;
}
apkg_status apkg_client_verify(const char *root, const char *name,
                               const apkg_observer *observer, char **out,
                               apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!root || !name || !ak_valid_name(name))
        return APKG_INVALID;
    apkg_open_options options = {0};
    options.contract_version = APKG_CONTRACT_VERSION;
    options.struct_size = sizeof options;
    options.root = (apkg_text){root, strlen(root)};
    apkg_ctx *ctx = NULL;
    apkg_snapshot *snapshot = NULL, *after = NULL;
    apkg_inspection *inspection = NULL;
    ak_json expected = {0}, live = {0};
    ak_buf rows = {0}, report = {0};
    rows.max = report.max = 65535;
    char *tree = NULL, path[AK_PATH];
    struct stat identity;
    uint64_t ok = 0, changed = 0, missing = 0, unknown = 0;
    apkg_status code = ak_cancelled(observer)
                           ? APKG_CANCELLED
                           : apkg_open(&options, &ctx, error);
    if (!code)
        code = apkg_snapshot_read(ctx, &snapshot, error);
    if (!code && snapshot->view.state != APKG_ROOT_READY)
        code = snapshot->view.state == APKG_ROOT_PENDING
                   ? APKG_RECOVERY_REQUIRED
                   : APKG_POLICY;
    size_t matched = SIZE_MAX;
    for (size_t i = 0; !code && i < snapshot->nrows; i++) {
        if (strcmp(name, snapshot->rows[i].name.value.data))
            continue;
        if (matched != SIZE_MAX) {
            code = APKG_CONFLICT;
            break;
        }
        matched = i;
    }
    if (!code && matched == SIZE_MAX)
        code = APKG_NOT_FOUND;
    if (!code)
        code = apkg_snapshot_inspect(snapshot, matched, &inspection, error);
    const apkg_inspection_view *view =
        inspection ? apkg_inspection_get(inspection) : NULL;
    if (!code && view->signature != APKG_SIGNATURE_VALID)
        code = APKG_SIGNATURE;
    if (!code) {
        int parsed = ak_json_parse(view->descriptor.data, view->descriptor.size,
                                   &expected);
        if (parsed)
            code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    }
    if (!code && ak_path(path, ctx->root, name))
        code = APKG_LIMIT;
    if (!code)
        code = ak_tree_read(path,
                            ctx->object_max < ctx->memory_max / 32
                                ? ctx->object_max
                                : ctx->memory_max / 32,
                            &tree, &identity);
    if (!code) {
        int parsed = ak_json_parse(tree, strlen(tree), &live);
        if (parsed)
            code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    }
    int declarations = code ? -1 : array(&expected),
        actual = code ? -1 : array(&live);
    for (size_t i = declarations < 0 ? 0 : (size_t)declarations + 1;
         !code && declarations >= 0 && i < expected.tokens[declarations].next;
         i = expected.tokens[i].next) {
        char relative[AK_PATH];
        if (ak_cancelled(observer)) {
            code = APKG_CANCELLED;
            break;
        }
        if (ak_string(&expected, ak_member(&expected, (int)i, "path"), relative,
                      sizeof relative)) {
            code = APKG_INVALID;
            break;
        }
        int at = find(&live, actual, relative),
            equal = at < 0 ? 0 : same(&expected, (int)i, &live, at);
        if (equal < 0) {
            code = APKG_NOMEM;
            break;
        }
        const char *state = at < 0 ? "missing" : equal ? "ok" : "changed";
        if (at < 0)
            missing++;
        else if (equal)
            ok++;
        else
            changed++;
        ak_buf_add(&rows, "%s %s%s\n", state, relative,
                   ak_equal(&expected, ak_member(&expected, (int)i, "config"),
                            "preserve")
                       ? " [configuration]"
                       : "");
    }
    for (size_t i = actual < 0 ? 0 : (size_t)actual + 1;
         !code && actual >= 0 && i < live.tokens[actual].next;
         i = live.tokens[i].next) {
        char relative[AK_PATH];
        if (ak_string(&live, ak_member(&live, (int)i, "path"), relative,
                      sizeof relative)) {
            code = APKG_INVALID;
            break;
        }
        if (find(&expected, declarations, relative) < 0) {
            unknown++;
            ak_buf_add(&rows, "unknown %s\n", relative);
        }
    }
    if (!code && rows.bad)
        code = (apkg_status)rows.bad;
    if (!code)
        code = apkg_snapshot_read(ctx, &after, error);
    if (!code && (after->view.state != APKG_ROOT_READY ||
                  memcmp(after->view.snapshot_digest.bytes,
                         snapshot->view.snapshot_digest.bytes, 32)))
        code = APKG_STALE;
    if (!code && ak_cancelled(observer))
        code = APKG_CANCELLED;
    if (!code) {
        ak_buf_add(&report,
                   "Verification of %s/%s %s (revision %" PRIu64
                   ")\nSignature: verified\nAuthor trust: %s\nok %" PRIu64
                   "\nchanged %" PRIu64 "\nmissing %" PRIu64
                   "\nunknown %" PRIu64 "\n%s",
                   view->authority.data, view->name.data, view->version.data,
                   view->revision,
                   view->author_trust == APKG_TRUST_ACCEPTED ? "accepted"
                                                             : "rejected",
                   ok, changed, missing, unknown, rows.data ? rows.data : "");
        code = report.bad ? (apkg_status)report.bad : APKG_OK;
    }
    if (!code) {
        *out = report.data;
        report.data = NULL;
    } else if (error && !*error)
        ak_fail(error, code, "installed verification refused", name);
    free(report.data);
    free(rows.data);
    free(tree);
    ak_json_free(&expected);
    ak_json_free(&live);
    apkg_inspection_free(inspection);
    apkg_snapshot_free(after);
    apkg_snapshot_free(snapshot);
    apkg_close(ctx);
    return code;
}
