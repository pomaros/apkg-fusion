/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#define _POSIX_C_SOURCE 200809L
#include "apkg_client.h"
#include "internal.h"
#include "pkg_fs.h"
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
static apkg_status open_root(const char *root, apkg_mode mode, apkg_ctx **c,
                             apkg_error **e) {
    apkg_open_options o = {0};
    if (!root)
        return APKG_INVALID;
    o.contract_version = 1;
    o.struct_size = sizeof o;
    o.mode = mode;
    o.root.data = root;
    o.root.size = strlen(root);
    return apkg_open(&o, c, e);
}
static apkg_status read_package(apkg_ctx *c, const char *dir,
                                const apkg_observer *observer,
                                apkg_package **out, apkg_error **e) {
    return ak_package_source(c, dir, observer, out, e);
}
static int compare_package(const void *a, const void *b) {
    const apkg_package *x = *(apkg_package *const *)a,
                       *y = *(apkg_package *const *)b;
    int cmp = strcmp(x->name, y->name);
    if (!cmp)
        cmp = strcmp(x->version, y->version);
    if (!cmp)
        cmp = memcmp(x->digest.bytes, y->digest.bytes, 32);
    return cmp;
}
apkg_status apkg_client_apply(apkg_client_action action, const char *root,
                              const char *arg, const char *pool,
                              apkg_confirm_fn confirm, void *user,
                              const apkg_observer *observer, apkg_result **out,
                              apkg_error **e) {
    apkg_ctx *c = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_package *package = NULL, *candidates[128] = {0};
    apkg_plan *p = NULL;
    apkg_approval *a = NULL;
    char *text = NULL;
    apkg_status code;
    size_t n = 0, i;
    DIR *dir = NULL;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (action == APKG_CLIENT_INSTALL)
        return apkg_client_install(root, arg, confirm, user, observer, out, e);
    if (action < APKG_CLIENT_REMOVE || action > APKG_CLIENT_RECOVER || !arg ||
        !confirm)
        return APKG_INVALID;
    code = open_root(root, APKG_WRITE, &c, e);
    if (code)
        goto done;
    if (action == APKG_CLIENT_RECOVER) {
        code = apkg_recover(c, observer, out, e);
        goto done;
    }
    code = apkg_snapshot_read(c, &snapshot, e);
    if (code)
        goto done;
    apkg_text argument = {arg, strlen(arg)};
    if (action == APKG_CLIENT_REMOVE)
        code = apkg_plan_remove(c, snapshot, argument, &p, e);
    else if (action == APKG_CLIENT_REPAIR)
        code = apkg_plan_repair(c, snapshot, argument, &p, e);
    else if (action == APKG_CLIENT_ROLLBACK)
        code = apkg_plan_rollback(c, snapshot, argument, &p, e);
    else {
        code = read_package(c, arg, observer, &package, e);
        if (code)
            goto done;
        if (action == APKG_CLIENT_UPGRADE)
            code = apkg_plan_upgrade(c, snapshot, package, &p, e);
        else {
            if (pool && *pool) {
                struct dirent *entry;
                dir = opendir(pool);
                if (!dir) {
                    code = APKG_IO;
                    goto done;
                }
                errno = 0;
                while ((entry = readdir(dir))) {
                    char child[AK_PATH];
                    struct stat st;
                    if (!strcmp(entry->d_name, ".") ||
                        !strcmp(entry->d_name, ".."))
                        continue;
                    if (n == 128) {
                        code = APKG_LIMIT;
                        goto done;
                    }
                    if (ak_path(child, pool, entry->d_name) ||
                        lstat(child, &st) ||
                        (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
                        code = APKG_UNSUPPORTED;
                        goto done;
                    }
                    code = read_package(c, child, observer, &candidates[n], e);
                    if (code)
                        goto done;
                    n++;
                    errno = 0;
                }
                if (errno) {
                    code = APKG_IO;
                    goto done;
                }
                closedir(dir);
                dir = NULL;
                if (n > 1)
                    qsort(candidates, n, sizeof *candidates, compare_package);
            }
            code = apkg_plan_resolve(c, snapshot, &package, 1, candidates, n, 0,
                                     &p, e);
        }
    }
    if (code)
        goto done;
    code = apkg_plan_format(p, &text);
    if (code)
        goto done;
    if (!confirm(user, p, text)) {
        code = APKG_CANCELLED;
        goto done;
    }
    code = apkg_plan_approve(c, p, &p->view.plan_digest, &a, e);
    if (!code)
        code = apkg_execute(c, p, a, observer, out, e);
done:
    if (dir)
        closedir(dir);
    for (i = 0; i < n; i++)
        apkg_package_free(candidates[i]);
    apkg_package_free(package);
    apkg_plan_free(p);
    apkg_approval_free(a);
    free(text);
    apkg_snapshot_free(snapshot);
    apkg_close(c);
    return code;
}
apkg_status apkg_client_query(const char *root, const char *match, char **out,
                              apkg_error **e) {
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    apkg_status code;
    ak_buf b = {0};
    size_t i;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    code = open_root(root, APKG_READ, &c, e);
    if (code)
        goto done;
    code = apkg_snapshot_read(c, &s, e);
    if (code)
        goto done;
    b.max = c->object_max;
    ak_buf_add(&b, "Root: %s\nState: %d\n", c->root, s->view.state);
    if (s->view.state != APKG_ROOT_READY) {
        ak_buf_add(&b, "%.*s\n", (int)s->view.diagnostic.size,
                   s->view.diagnostic.data ? s->view.diagnostic.data : "");
    }
    for (i = 0; i < s->nrows; i++) {
        const apkg_installed_view *r = &s->rows[i];
        if (match && *match && !strstr(r->name.value.data, match))
            continue;
        ak_buf_add(&b,
                   "\n%s %s revision %" PRIu64
                   "\nAuthority: %s\nTarget: %s\nReason: %s\nSignature/trust: "
                   "not reverified by this read-only query\nReview/runtime "
                   "qualification: unknown\n",
                   r->name.value.data, r->version.value.data, r->revision.value,
                   r->authority.value.data, r->target.value.data,
                   r->reason == APKG_REASON_DEPENDENCY       ? "dependency"
                   : r->reason == APKG_REASON_RECOMMENDATION ? "recommendation"
                                                             : "explicit");
    }
    code = b.bad;
    if (!code) {
        *out = b.data;
        b.data = NULL;
    }
done:
    free(b.data);
    apkg_snapshot_free(s);
    apkg_close(c);
    return code;
}
apkg_status ak_changes_format(const apkg_plan *p, char **out) {
    ak_json changes = {0}, state = {0};
    ak_buf b = {0};
    size_t i, k;
    char digest[65];
    int arr;
    *out = NULL;
    b.max = AK_MAX;
    if (ak_json_parse(p->changes, strlen(p->changes), &changes) ||
        ak_json_parse(p->root_record, strlen(p->root_record), &state)) {
        ak_json_free(&changes);
        ak_json_free(&state);
        return errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
    }
    const char *action = p->view.kind == APKG_PLAN_REMOVE    ? "Remove"
                         : p->view.kind == APKG_PLAN_REPAIR  ? "Repair"
                         : p->view.kind == APKG_PLAN_UPGRADE ? "Upgrade"
                         : p->view.kind == APKG_PLAN_ROLLBACK
                             ? "Rollback"
                             : "Resolve/install";
    ak_tohex(p->view.plan_digest.bytes, 32, digest);
    ak_buf_add(&b, "%s\nRoot folder: %s\nApproved plan: %s\n", action, p->root,
               digest);
    arr = ak_member(&state, ak_member(&state, 0, "body"), "installed");
    for (i = (size_t)arr + 1; i < state.tokens[arr].next;
         i = state.tokens[i].next) {
        char name[256], authority[33], key[65], version[256], target[32];
        int body = ak_member(&state, ak_member(&state, (int)i, "descriptor"),
                             "body"),
            sig = ak_member(&state, (int)i, "signature");
        uint64_t revision;
        ak_string(&state, ak_member(&state, body, "name"), name, sizeof name);
        ak_string(&state, ak_member(&state, body, "authority"), authority,
                  sizeof authority);
        ak_string(&state, ak_member(&state, sig, "public_key"), key,
                  sizeof key);
        ak_string(&state, ak_member(&state, body, "version"), version,
                  sizeof version);
        ak_string(&state, ak_member(&state, body, "target"), target,
                  sizeof target);
        ak_u64(&state, ak_member(&state, body, "revision"), &revision);
        ak_buf_add(&b,
                   "\nSelected: %s %s revision %" PRIu64
                   "\nPublisher: %s\nTarget: %s\nSignature: verified, "
                   "authorized key %s\n",
                   name, version, revision, authority, target, key);
    }
    ak_buf_add(&b, "\nReview/runtime qualification: no independent "
                   "qualification asserted\n");
    for (i = 1; i < changes.tokens[0].next; i = changes.tokens[i].next) {
        char name[256];
        ak_json before = {0}, next = {0};
        char *a = ak_slice(&changes, ak_member(&changes, (int)i, "before")),
             *n = ak_slice(&changes, ak_member(&changes, (int)i, "next"));
        ak_string(&changes, ak_member(&changes, (int)i, "name"), name,
                  sizeof name);
        if (!a || !n || ak_json_parse(a, strlen(a), &before) ||
            ak_json_parse(n, strlen(n), &next)) {
            free(a);
            free(n);
            ak_json_free(&before);
            ak_json_free(&next);
            b.bad = APKG_NOMEM;
            break;
        }
        ak_buf_add(&b, "\nPackage folder: %s\n", name);
        int passes;
        for (passes = 0; passes < 2; passes++) {
            const ak_json *j = passes ? &before : &next;
            int files = ak_member(j, 0, "files");
            if (files < 0)
                continue;
            for (k = (size_t)files + 1; k < j->tokens[files].next;
                 k = j->tokens[k].next) {
                char path[AK_PATH], sha[65], mode[24], size[24];
                ak_string(j, ak_member(j, (int)k, "path"), path, sizeof path);
                int other = ak_tree_find(passes ? &next : &before, path);
                if (passes && other >= 0)
                    continue;
                const char *effect = passes      ? "Remove"
                                     : other < 0 ? "Create"
                                                 : "Replace";
                if (!passes && other >= 0) {
                    int t = (int)k;
                    size_t len = j->tokens[t].end - j->tokens[t].start,
                           olen = before.tokens[other].end -
                                  before.tokens[other].start;
                    if (len == olen &&
                        !memcmp(j->text + j->tokens[t].start,
                                before.text + before.tokens[other].start, len))
                        effect = "Preserve";
                }
                ak_buf_add(&b, "%s %s/%s", effect, name, path);
                if (ak_equal(j, ak_member(j, (int)k, "type"), "file")) {
                    ak_string(j, ak_member(j, (int)k, "sha256"), sha,
                              sizeof sha);
                    ak_string(j, ak_member(j, (int)k, "mode"), mode,
                              sizeof mode);
                    ak_string(j, ak_member(j, (int)k, "size"), size,
                              sizeof size);
                    ak_buf_add(&b, "; %s bytes, mode %s, SHA-256 %s", size,
                               mode, sha);
                } else
                    ak_buf_add(&b, " (directory)");
                ak_buf_add(&b, "\n");
            }
        }
        free(a);
        free(n);
        ak_json_free(&before);
        ak_json_free(&next);
    }
    ak_buf_add(&b, "\nCommit .apkg/state at generation %" PRIu64 "\n",
               p->view.generation + 1);
    apkg_status code = b.bad;
    if (!code) {
        *out = b.data;
        b.data = NULL;
    }
    free(b.data);
    ak_json_free(&changes);
    ak_json_free(&state);
    return code;
}
apkg_status apkg_client_folder(const char *root, const char *name, char **out,
                               apkg_error **e) {
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    apkg_status code;
    char path[AK_PATH];
    size_t i;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!name || !ak_valid_name(name))
        return APKG_INVALID;
    code = open_root(root, APKG_READ, &c, e);
    if (code)
        goto done;
    code = apkg_snapshot_read(c, &s, e);
    if (code)
        goto done;
    if (s->view.state != APKG_ROOT_READY) {
        code = s->view.state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED
                                                  : APKG_POLICY;
        goto done;
    }
    for (i = 0; i < s->nrows; i++)
        if (!strcmp(name, s->rows[i].name.value.data))
            break;
    if (i == s->nrows) {
        code = APKG_NOT_FOUND;
        goto done;
    }
    if (ak_path(path, c->root, name) || pkg_fs_path_is_link(path) != 0) {
        code = APKG_INVALID;
        goto done;
    }
    {
        char canonical[AK_PATH];
        if (pkg_fs_canonical_dir(path, canonical, sizeof canonical) ||
            strcmp(canonical, path)) {
            code = APKG_IO;
            goto done;
        }
    }
    *out = ak_copy(path, strlen(path));
    code = *out ? APKG_OK : APKG_NOMEM;
done:
    apkg_snapshot_free(s);
    apkg_close(c);
    return code;
}
apkg_status apkg_client_run(const char *root, const char *name,
                            apkg_error **e) {
    char *folder = NULL;
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    apkg_status code = apkg_client_folder(root, name, &folder, e);
    if (code)
        return code;
    code = open_root(root, APKG_READ, &c, e);
    if (!code)
        code = apkg_snapshot_read(c, &s, e);
    if (!code && s->view.state != APKG_ROOT_READY)
        code = APKG_RECOVERY_REQUIRED;
    if (!code)
        code = apkg_bundle_run(folder, s->target, e);
    apkg_snapshot_free(s);
    apkg_close(c);
    free(folder);
    return code;
}
