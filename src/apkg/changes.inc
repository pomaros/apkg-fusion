/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Common journal participants for package-tree changes. Included by execute.c
 * to share locking, publication, durable markers, results and event ownership.
 */
static size_t change_limit(apkg_ctx *c) {
    return c->object_max < c->memory_max / 32 ? c->object_max
                                              : c->memory_max / 32;
}
static int change_path(char *out, const char *txn, const char *prefix,
                       size_t i) {
    char leaf[64];
    snprintf(leaf, sizeof leaf, "%s-%zu", prefix, i);
    return ak_path(out, txn, leaf);
}
static int tree_matches(apkg_ctx *c, const char *path, const char *expected,
                        uint64_t dev, uint64_t ino) {
    struct stat st = {0};
    char *got = NULL;
    apkg_status status = ak_tree_read(path, change_limit(c), &got, &st);
    int bad = status != APKG_OK;
    if (!bad)
        bad =
            strcmp(got, expected) ||
            (ino && ((uint64_t)st.st_ino != ino || (uint64_t)st.st_dev != dev));
    free(got);
    if (status == APKG_NOMEM)
        errno = ENOMEM;
    return bad;
}
static int new_identity(const char *txn, size_t i, const char *tree,
                        int write) {
    char p[AK_PATH], text[96];
    struct stat st;
    unsigned char *bytes = NULL;
    size_t n;
    int len, bad;
    if (change_path(p, txn, "new-id", i) || stat(tree, &st))
        return -1;
    len =
        snprintf(text, sizeof text, "%llu:%llu", (unsigned long long)st.st_dev,
                 (unsigned long long)st.st_ino);
    if (write)
        return ak_write(p, text, (size_t)len, 1);
    if (ak_read(p, &bytes, &n, sizeof text))
        return -1;
    bad = n != (size_t)len || memcmp(bytes, text, n);
    free(bytes);
    return bad;
}
static int inventory_stage(const char *tree, const char *raw) {
    ak_json j = {0};
    size_t i;
    int arr, bad = 0;
    uint64_t mode;
    if (ak_json_parse(raw, strlen(raw), &j))
        return -1;
    if (mkdir(tree, 0700) || stage_package(tree, &j)) {
        bad = 1;
        goto done;
    }
    arr = ak_member(&j, 0, "files");
    /* Set directory modes after all descendants have been written. */
    for (i = (size_t)arr + 1; i < j.tokens[arr].next; i = j.tokens[i].next) {
        char rel[AK_PATH], path[AK_PATH];
        if (!ak_equal(&j, ak_member(&j, (int)i, "type"), "directory"))
            continue;
        if (ak_string(&j, ak_member(&j, (int)i, "path"), rel, sizeof rel) ||
            !ak_valid_path(rel) || ak_path(path, tree, rel) ||
            ak_u64(&j, ak_member(&j, (int)i, "mode"), &mode) || mode > 0777 ||
            chmod(path, (mode_t)mode)) {
            bad = 1;
            goto done;
        }
    }
    if (ak_u64(&j, ak_member(&j, 0, "mode"), &mode) || mode > 0777 ||
        chmod(tree, (mode_t)mode) || sync_tree(tree))
        bad = 1;
done:
    ak_json_free(&j);
    return bad;
}
static int changes_validate(apkg_ctx *c, const ak_json *plan, int *array) {
    int b = ak_member(plan, 0, "body");
    uint64_t dev, ino;
    char hex[AK_PATH * 2 + 1], expected[AK_PATH * 2 + 1];
    size_t i, x;
    static const char *const fields[] = {"action", "anchor_dev", "anchor_ino",
                                         "before", "changes",    "generation",
                                         "kind",   "new_state",  "root_path"};
    char legacy[AK_PATH];
    if (ak_path(legacy, c->root, "db") || ak_exists(legacy) != 0 ||
        ak_path(legacy, c->root, ".pkg") || ak_exists(legacy) != 0)
        return -1;
    if (!ak_fields(plan, b, fields, 9) ||
        ak_u64(plan, ak_member(plan, b, "anchor_dev"), &dev) ||
        ak_u64(plan, ak_member(plan, b, "anchor_ino"), &ino) || !ino ||
        dev != (uint64_t)c->binding.st_dev ||
        ino != (uint64_t)c->binding.st_ino ||
        !ak_physical(c->anchor, &c->binding))
        return -1;
    ak_tohex((unsigned char *)c->root, strlen(c->root), expected);
    if (ak_string(plan, ak_member(plan, b, "root_path"), hex, sizeof hex) ||
        strcmp(hex, expected))
        return -1;
    *array = ak_member(plan, b, "changes");
    if (*array < 0 || plan->tokens[*array].type != '[')
        return -1;
    for (i = (size_t)*array + 1; i < plan->tokens[*array].next;
         i = plan->tokens[i].next) {
        char name[256];
        static const char *const f[] = {"before", "dev", "ino", "name", "next"};
        if (!ak_fields(plan, (int)i, f, 5) ||
            ak_string(plan, ak_member(plan, (int)i, "name"), name,
                      sizeof name) ||
            !ak_valid_name(name) ||
            ak_u64(plan, ak_member(plan, (int)i, "dev"), &dev) ||
            ak_u64(plan, ak_member(plan, (int)i, "ino"), &ino))
            return -1;
        for (x = (size_t)*array + 1; x < i; x = plan->tokens[x].next)
            if (ak_equal(plan, ak_member(plan, (int)x, "name"), name))
                return -1;
    }
    return 0;
}
static apkg_status recover_changes_locked(apkg_ctx *c, apkg_result *r,
                                          const char *txn,
                                          const ak_json *plan) {
    unsigned char *old = NULL, *next = NULL, *id = NULL;
    size_t on = 0, nn = 0, idn = 0, i, index = 0;
    int arr, decision, b = ak_member(plan, 0, "body");
    char state[AK_PATH], dummy[AK_PATH], hex[65], before[65];
    apkg_digest h;
    apkg_status code = APKG_RECOVERY_REQUIRED;
    if (changes_validate(c, plan, &arr) || paths(c, dummy, state) ||
        has(txn, "ready") != 1 ||
        read_in_limit(txn, "old-state", &old, &on, change_limit(c)) ||
        read_in_limit(txn, "new-state", &next, &nn, change_limit(c)) ||
        read_in_limit(txn, "archive-id", &id, &idn, 32) || idn != 32 ||
        ak_hex((char *)id, r->view.operation_id.bytes, 16))
        goto done;
    {
        int t = ak_member(plan, b, "new_state");
        if (t < 0 || plan->tokens[t].end - plan->tokens[t].start != nn ||
            memcmp(plan->text + plan->tokens[t].start, next, nn))
            goto done;
    }
    ak_hash(old, on, &h);
    ak_tohex(h.bytes, 32, hex);
    if (ak_string(plan, ak_member(plan, b, "before"), before, sizeof before) ||
        strcmp(before, hex))
        goto done;
    decision = has(txn, "decision");
    if (decision < 0 || fault(6))
        goto done;
    if (decision) {
        if (exact_file(state, old, on) && exact_file(state, next, nn))
            goto done;
    } else if (exact_file(state, old, on))
        goto done;
    /* First validate every participant before recovery touches any of them. */
    for (i = (size_t)arr + 1; i < plan->tokens[arr].next;
         i = plan->tokens[i].next, index++) {
        char name[256], live[AK_PATH], backup[AK_PATH], *was = NULL, *to = NULL;
        uint64_t dev, ino;
        int bad = 0, held;
        ak_string(plan, ak_member(plan, (int)i, "name"), name, sizeof name);
        ak_u64(plan, ak_member(plan, (int)i, "dev"), &dev);
        ak_u64(plan, ak_member(plan, (int)i, "ino"), &ino);
        was = ak_slice(plan, ak_member(plan, (int)i, "before"));
        to = ak_slice(plan, ak_member(plan, (int)i, "next"));
        if (!was || !to || ak_path(live, c->root, name) ||
            change_path(backup, txn, "old", index)) {
            free(was);
            free(to);
            goto done;
        }
        held = ak_exists(backup);
        if (decision)
            bad = tree_matches(c, live, to, 0, 0) ||
                  (strcmp(to, "null") && new_identity(txn, index, live, 0)) ||
                  (strcmp(was, "null") && held != 1) ||
                  (held == 1 && tree_matches(c, backup, was, dev, ino));
        else if (held == 1)
            bad = tree_matches(c, backup, was, dev, ino) ||
                  (ak_exists(live) != 0 && (tree_matches(c, live, to, 0, 0) ||
                                            new_identity(txn, index, live, 0)));
        else if (held == 0) {
            if (!strcmp(was, "null") && ak_exists(live) == 1)
                bad = tree_matches(c, live, to, 0, 0) ||
                      new_identity(txn, index, live, 0);
            else
                bad = tree_matches(c, live, was, dev, ino);
        } else
            bad = 1;
        free(was);
        free(to);
        if (bad)
            goto done;
    }
    if (!decision) {
        index = 0;
        for (i = (size_t)arr + 1; i < plan->tokens[arr].next;
             i = plan->tokens[i].next, index++) {
            char name[256], live[AK_PATH], backup[AK_PATH], restored[AK_PATH];
            int oldtok = ak_member(plan, (int)i, "before");
            int was_absent = plan->tokens[oldtok].type == 'n';
            ak_string(plan, ak_member(plan, (int)i, "name"), name, sizeof name);
            ak_path(live, c->root, name);
            change_path(backup, txn, "old", index);
            change_path(restored, txn, "restored", index);
            if (ak_exists(backup) == 1) {
                if (ak_exists(live) == 1 &&
                    (publish_dir(live, restored) || ak_sync_dir(c->root) ||
                     ak_sync_dir(txn)))
                    goto done;
                if (publish_dir(backup, live) || ak_sync_dir(c->root) ||
                    ak_sync_dir(txn))
                    goto done;
            } else if (was_absent && ak_exists(live) == 1) {
                if (publish_dir(live, restored) || ak_sync_dir(c->root) ||
                    ak_sync_dir(txn))
                    goto done;
            }
            if (fault(106))
                goto done;
        }
    } else if (exact_file(state, next, nn) &&
               state_publish(state, txn, change_limit(c)))
        goto done;
    if (ak_sync_dir(c->root) || fault(7))
        goto done;
    {
        char archived[AK_PATH], parent[AK_PATH];
        if (snprintf(archived, sizeof archived, "%s.apkg-history.%s", c->root,
                     id) >= (int)sizeof archived ||
            publish_dir(txn, archived) || parent_path(c->root, parent) ||
            ak_sync_dir(parent))
            goto done;
        strcpy(r->record, archived);
        r->view.record.data = (unsigned char *)r->record;
        r->view.record.size = strlen(archived);
    }
    result_set(r, APKG_OK, decision ? APKG_COMMITTED : APKG_RESTORED);
    code = APKG_OK;
done:
    free(old);
    free(next);
    free(id);
    if (code)
        result_set(r, code, APKG_NEEDS_RECOVERY);
    return code;
}
static int changes_preflight(apkg_ctx *c, const apkg_plan *p, const ak_json *j,
                             int arr, int after) {
    size_t i, index = 0;
    char txn[AK_PATH], state[AK_PATH];
    char legacy[AK_PATH];
    if (ak_path(legacy, c->root, "db") || ak_exists(legacy) != 0 ||
        ak_path(legacy, c->root, ".pkg") || ak_exists(legacy) != 0)
        return -1;
    errno = 0;
    if (!ak_physical(c->anchor, &c->binding) || paths(c, txn, state) ||
        exact_file(state, (const unsigned char *)p->before_state,
                   strlen(p->before_state)))
        return -1;
    for (i = (size_t)arr + 1; i < j->tokens[arr].next;
         i = j->tokens[i].next, index++) {
        char name[256], path[AK_PATH], *raw;
        uint64_t dev = 0, ino = 0;
        int bad;
        if (ak_string(j, ak_member(j, (int)i, "name"), name, sizeof name) ||
            ak_path(path, c->root, name))
            return -1;
        if (!after) {
            ak_u64(j, ak_member(j, (int)i, "dev"), &dev);
            ak_u64(j, ak_member(j, (int)i, "ino"), &ino);
        }
        raw = ak_slice(j, ak_member(j, (int)i, after ? "next" : "before"));
        if (!raw)
            return -1;
        bad = tree_matches(c, path, raw, dev, ino);
        if (!bad && after && strcmp(raw, "null"))
            bad = new_identity(txn, index, path, 0);
        if (!bad && after) {
            char backup[AK_PATH];
            char *old = ak_slice(j, ak_member(j, (int)i, "before"));
            ak_u64(j, ak_member(j, (int)i, "dev"), &dev);
            ak_u64(j, ak_member(j, (int)i, "ino"), &ino);
            if (!old || change_path(backup, txn, "old", index))
                bad = 1;
            else
                bad = tree_matches(c, backup, old, dev, ino);
            free(old);
        }
        free(raw);
        if (bad)
            return -1;
    }
    return 0;
}
static apkg_status execute_changes_locked(apkg_ctx *c, const apkg_plan *p,
                                          const apkg_snapshot *s,
                                          const apkg_observer *o,
                                          apkg_result *r) {
    ak_json j = {0};
    char txn[AK_PATH], state[AK_PATH], hex[65], id[33];
    size_t i, index = 0;
    int arr, ready = 0, decided = 0;
    uint64_t seq = 0;
    apkg_status code = APKG_IO;
    if (ak_json_parse(p->raw, p->view.canonical.size, &j)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    if (changes_validate(c, &j, &arr) || paths(c, txn, state)) {
        code = APKG_STALE;
        goto done;
    }
    if (emit(o, r, &seq, APKG_PHASE_CHECK, 1, c->root)) {
        code = APKG_CANCELLED;
        goto done;
    }
    if (changes_preflight(c, p, &j, arr, 0)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_STALE;
        goto done;
    }
    if (mkdir(txn, 0700))
        goto done;
    strcpy(r->record, txn);
    r->view.record.data = (unsigned char *)r->record;
    r->view.record.size = strlen(txn);
    ak_tohex(r->view.operation_id.bytes, 16, id);
    ak_tohex(p->view.plan_digest.bytes, 32, hex);
    if (write_in(txn, "archive-id", id, 32) ||
        write_in(txn, "plan", p->raw, p->view.canonical.size) ||
        write_in(txn, "plan-sha", hex, 64) ||
        write_in(txn, "old-state", s->raw, s->view.source_record.size) ||
        write_in(txn, "new-state", p->root_record, strlen(p->root_record)) ||
        fault(1))
        goto prepare_fail;
    for (i = (size_t)arr + 1; i < j.tokens[arr].next;
         i = j.tokens[i].next, index++) {
        char staged[AK_PATH],
            *raw = ak_slice(&j, ak_member(&j, (int)i, "next"));
        if (!raw || change_path(staged, txn, "new", index)) {
            free(raw);
            goto prepare_fail;
        }
        if (strcmp(raw, "null") && (inventory_stage(staged, raw) ||
                                    new_identity(txn, index, staged, 1))) {
            free(raw);
            goto prepare_fail;
        }
        free(raw);
    }
    char parent[AK_PATH];
    if (marker(txn, "ready") || parent_path(c->root, parent) ||
        ak_sync_dir(parent))
        goto prepare_fail;
    ready = 1;
    if (fault(2))
        goto fail;
    if (emit(o, r, &seq, APKG_PHASE_STAGE, 1, c->root)) {
        code = APKG_CANCELLED;
        goto fail;
    }
    if (changes_preflight(c, p, &j, arr, 0)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_STALE;
        goto fail;
    }
    index = 0;
    for (i = (size_t)arr + 1; i < j.tokens[arr].next;
         i = j.tokens[i].next, index++) {
        char name[256], live[AK_PATH], backup[AK_PATH], staged[AK_PATH];
        int before = ak_member(&j, (int)i, "before"),
            next = ak_member(&j, (int)i, "next");
        ak_string(&j, ak_member(&j, (int)i, "name"), name, sizeof name);
        ak_path(live, c->root, name);
        change_path(backup, txn, "old", index);
        change_path(staged, txn, "new", index);
        if (j.tokens[before].type != 'n' &&
            (publish_dir(live, backup) || ak_sync_dir(c->root) ||
             ak_sync_dir(txn)))
            goto fail;
        if (fault(101))
            goto fail;
        if (j.tokens[next].type != 'n' &&
            (publish_dir(staged, live) || ak_sync_dir(c->root) ||
             ak_sync_dir(txn)))
            goto fail;
        if (fault(102) || fault(3))
            goto fail;
        if (emit(o, r, &seq, APKG_PHASE_APPLY, 1, name)) {
            code = APKG_CANCELLED;
            goto fail;
        }
    }
    if (changes_preflight(c, p, &j, arr, 1)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_STALE;
        goto fail;
    }
    decided = 1;
    if (marker(txn, "decision"))
        goto fail;
    emit(o, r, &seq, APKG_PHASE_COMMIT, 0, c->root);
    if (changes_preflight(c, p, &j, arr, 1) || fault(4) ||
        state_publish(state, txn, change_limit(c)) || fault(5))
        goto fail;
    code = recover_changes_locked(c, r, txn, &j);
    if (!code) {
        r->view.committed_generation.state = APKG_KNOWN;
        r->view.committed_generation.value = p->view.generation + 1;
    }
    goto done;
prepare_fail:
    code = errno == ENOMEM ? APKG_NOMEM : APKG_IO;
    if (ak_remove_tree(txn)) {
        code = APKG_RECOVERY_REQUIRED;
        result_set(r, code, APKG_NEEDS_RECOVERY);
    }
    goto done;
fail:
    if (!decided && ready) {
        apkg_status recovered = recover_changes_locked(c, r, txn, &j);
        if (!recovered)
            result_set(r, code, APKG_RESTORED);
        else {
            code = APKG_RECOVERY_REQUIRED;
            result_set(r, code, APKG_NEEDS_RECOVERY);
        }
    } else {
        code = APKG_RECOVERY_REQUIRED;
        result_set(r, code, APKG_NEEDS_RECOVERY);
    }
done:
    ak_json_free(&j);
    if (code && r->view.status == APKG_OK)
        result_set(r, code, APKG_UNCHANGED);
    return code;
}
