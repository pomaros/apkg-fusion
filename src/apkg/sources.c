/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
/* The root's own ordered list of named sources.
 *
 * A machine that installs from the same places every day names them once.
 * The list belongs to the root, as pkg's channel list did, and changes only
 * through a plan the operator approves; adding a source grants no key, so a
 * source whose channel the policy does not trust cannot be added. Install by
 * name reads every source in order: the newest trusted candidate wins, an
 * equal version goes to the earlier source, dependencies are pooled across
 * the list, and each package is prepared from the source that offers it. */
#include "apkg_client.h"
#include "internal.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int name_valid(const char *n) {
    size_t len = strlen(n);
    if (!len || len > 63 || !isalnum((unsigned char)n[0]))
        return 0;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)n[i]) && !strchr("._-", n[i]))
            return 0;
    return 1;
}
/* Printable ASCII, so the canonical encoding needs no escapes. */
static int endpoint_valid(const char *e) {
    size_t len = strlen(e);
    if (!len || len >= AK_PATH)
        return 0;
    for (size_t i = 0; i < len; i++)
        if (e[i] < 0x20 || e[i] > 0x7e || e[i] == '"' || e[i] == '\\')
            return 0;
    return 1;
}
static int id_valid(const char *id) {
    if (strlen(id) != 32)
        return 0;
    for (size_t i = 0; i < 32; i++)
        if (!isdigit((unsigned char)id[i]) && !(id[i] >= 'a' && id[i] <= 'f'))
            return 0;
    return 1;
}
typedef struct {
    char name[64], id[33], endpoint[AK_PATH];
} source;
/* Reads a list into rows; -1 on an invalid list. An absent list is empty. */
static int read_list(const ak_json *j, int array, source *rows,
                     size_t *count) {
    *count = 0;
    if (array < 0)
        return 0;
    if (j->tokens[array].type != '[')
        return -1;
    for (size_t i = (size_t)array + 1; i < j->tokens[array].next;
         i = j->tokens[i].next) {
        source *r = &rows[*count];
        if (*count == AK_SOURCES ||
            !ak_fields(j, (int)i, (const char *const[]){"endpoint", "id", "name"},
                       3) ||
            ak_string(j, ak_member(j, (int)i, "endpoint"), r->endpoint,
                      sizeof r->endpoint) ||
            ak_string(j, ak_member(j, (int)i, "id"), r->id, sizeof r->id) ||
            ak_string(j, ak_member(j, (int)i, "name"), r->name, sizeof r->name) ||
            !endpoint_valid(r->endpoint) || !id_valid(r->id) ||
            !name_valid(r->name))
            return -1;
        for (size_t k = 0; k < *count; k++)
            if (!strcmp(rows[k].name, r->name) ||
                (!strcmp(rows[k].id, r->id) &&
                 !strcmp(rows[k].endpoint, r->endpoint)))
                return -1;
        (*count)++;
    }
    return 0;
}
int ak_sources_valid(const ak_json *j, int array) {
    source *rows;
    size_t count;
    int ok;
    if (array < 0)
        return 1;
    rows = malloc(AK_SOURCES * sizeof *rows);
    if (!rows) {
        errno = ENOMEM;
        return 0;
    }
    /* A stored list is never empty: no list is its canonical form. */
    ok = !read_list(j, array, rows, &count) && count;
    free(rows);
    return ok;
}
char *ak_sources_field(const char *array) {
    static const char prefix[] = ",\"sources\":";
    if (!strcmp(array, "[]"))
        return ak_copy("", 0);
    size_t n = strlen(array);
    char *out = malloc(sizeof prefix + n);
    if (out) {
        memcpy(out, prefix, sizeof prefix - 1);
        memcpy(out + sizeof prefix - 1, array, n + 1);
    }
    return out;
}
static const char *list_of(const apkg_snapshot *s) {
    return *s->sources_field ? s->sources_field + strlen(",\"sources\":") : "[]";
}
static char *format_list(const source *rows, size_t count) {
    ak_buf b = {0};
    b.max = AK_SOURCES * (AK_PATH + 160);
    ak_buf_add(&b, "[");
    for (size_t i = 0; i < count; i++)
        ak_buf_add(&b, "%s{\"endpoint\":\"%s\",\"id\":\"%s\",\"name\":\"%s\"}",
                   i ? "," : "", rows[i].endpoint, rows[i].id, rows[i].name);
    ak_buf_add(&b, "]");
    if (b.bad) {
        free(b.data);
        return NULL;
    }
    return b.data;
}
static int state_list(const char *state, source *rows, size_t *count) {
    ak_json j = {0};
    int r;
    if (ak_json_parse(state, strlen(state), &j))
        return -1;
    r = read_list(&j, ak_member(&j, ak_member(&j, 0, "body"), "sources"), rows,
                  count);
    ak_json_free(&j);
    return r;
}
apkg_status ak_sources_delta(const char *before_state, const char *after_state,
                             ak_buf *out) {
    source *old = malloc(2 * AK_SOURCES * sizeof *old), *now;
    size_t on, nn;
    if (!old)
        return APKG_NOMEM;
    now = old + AK_SOURCES;
    if (state_list(before_state, old, &on) || state_list(after_state, now, &nn)) {
        free(old);
        return APKG_INVALID;
    }
    for (size_t i = 0; i < nn; i++) {
        size_t k;
        for (k = 0; k < on; k++)
            if (!strcmp(old[k].name, now[i].name) &&
                !strcmp(old[k].id, now[i].id) &&
                !strcmp(old[k].endpoint, now[i].endpoint))
                break;
        if (k == on)
            ak_buf_add(out, "Add source %s: channel %s at %s\n", now[i].name,
                       now[i].id, now[i].endpoint);
    }
    for (size_t k = 0; k < on; k++) {
        size_t i;
        for (i = 0; i < nn; i++)
            if (!strcmp(old[k].name, now[i].name) &&
                !strcmp(old[k].id, now[i].id) &&
                !strcmp(old[k].endpoint, now[i].endpoint))
                break;
        if (i == nn)
            ak_buf_add(out, "Remove source %s: channel %s at %s\n", old[k].name,
                       old[k].id, old[k].endpoint);
    }
    ak_buf_add(out, nn ? "Source order:\n" : "No sources remain.\n");
    for (size_t i = 0; i < nn; i++)
        ak_buf_add(out, "  %zu. %s\n", i + 1, now[i].name);
    ak_buf_add(out, "No key is granted; installed packages are unchanged.\n");
    free(old);
    return out->bad ? (apkg_status)out->bad : APKG_OK;
}
apkg_status apkg_plan_sources(apkg_ctx *c, const apkg_snapshot *s,
                              apkg_bytes list, apkg_plan **out,
                              apkg_error **e) {
    ak_json j = {0}, root = {0};
    source *rows = NULL;
    size_t count;
    char *text = NULL, *entries = NULL;
    apkg_status code;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!c || !s || !list.data || !list.size)
        return APKG_INVALID;
    if (c->mode != APKG_WRITE)
        return APKG_POLICY;
    if (s->view.state != APKG_ROOT_READY)
        return s->view.state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED
                                                  : APKG_POLICY;
    if (list.size > AK_SOURCES * (AK_PATH + 160))
        return APKG_LIMIT;
    rows = malloc(AK_SOURCES * sizeof *rows);
    if (!rows)
        return APKG_NOMEM;
    {
        int parsed = ak_json_parse(list.data, list.size, &j);
        if (parsed || read_list(&j, 0, rows, &count)) {
            ak_json_free(&j);
            free(rows);
            return parsed == -2 ? APKG_NOMEM
                                : ak_fail(e, APKG_INVALID,
                                          "invalid source list", "");
        }
    }
    ak_json_free(&j);
    free(rows);
    text = ak_copy(list.data, list.size);
    if (!text)
        return APKG_NOMEM;
    if (!strcmp(text, list_of(s))) {
        free(text);
        return ak_fail(e, APKG_INVALID, "the source list is unchanged", "");
    }
    {
        int parsed = ak_json_parse(s->raw, s->view.source_record.size, &root);
        if (parsed) {
            free(text);
            return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        }
    }
    entries = ak_slice(&root,
                       ak_member(&root, ak_member(&root, 0, "body"), "installed"));
    ak_json_free(&root);
    if (!entries) {
        free(text);
        return APKG_NOMEM;
    }
    code = ak_plan_records_heads(c, s, entries, APKG_PLAN_SOURCES, NULL,
                                 s->channel_heads, s->qualification_records,
                                 NULL, text, out, e);
    free(entries);
    free(text);
    if (!code) {
        apkg_plan *p = *out;
        p->effects[0].kind = APKG_EFFECT_DATABASE;
        p->effects[0].destination.data = ".apkg/state#sources";
        p->effects[0].destination.size = strlen(p->effects[0].destination.data);
        p->effects[0].record.data = (unsigned char *)p->root_record;
        p->effects[0].record.size = strlen(p->root_record);
    }
    if (code && e && !*e)
        ak_fail(e, code, "source list change refused", "");
    return code;
}

/* ---- client operations ---------------------------------------------- */

static apkg_status open_root(const char *root, apkg_mode mode, apkg_ctx **c,
                             apkg_snapshot **s, apkg_error **e) {
    apkg_open_options o = {0};
    o.contract_version = APKG_CONTRACT_VERSION;
    o.struct_size = sizeof o;
    o.mode = mode;
    o.root = (apkg_text){root, strlen(root)};
    apkg_status code = apkg_open(&o, c, e);
    if (!code)
        code = apkg_snapshot_read(*c, s, e);
    if (!code && (*s)->view.state != APKG_ROOT_READY)
        code = (*s)->view.state == APKG_ROOT_PENDING ? APKG_RECOVERY_REQUIRED
                                                     : APKG_POLICY;
    return code;
}
static apkg_status open_channel(apkg_ctx *c, const source *r,
                                const apkg_observer *observer,
                                apkg_channel **out, apkg_error **e) {
    apkg_id id;
    apkg_text endpoint = {r->endpoint, strlen(r->endpoint)};
    if (ak_hex(r->id, id.bytes, 16))
        return APKG_INVALID;
    return ak_http_url(r->endpoint)
               ? apkg_channel_open_http(c, endpoint, &id, 0, observer, out, e)
               : apkg_channel_open_file(c, endpoint, &id, 0, out, e);
}
static apkg_status apply_list(apkg_ctx *c, const apkg_snapshot *s,
                              const source *rows, size_t count,
                              apkg_confirm_fn confirm, void *user,
                              const apkg_observer *observer, apkg_result **out,
                              apkg_error **e) {
    apkg_plan *p = NULL;
    apkg_approval *a = NULL;
    char *text = NULL, *list = format_list(rows, count);
    apkg_status code = list ? APKG_OK : APKG_NOMEM;
    if (!code)
        code = apkg_plan_sources(c, s, (apkg_bytes){(unsigned char *)list,
                                                    strlen(list)},
                                 &p, e);
    if (!code)
        code = apkg_plan_format(p, &text);
    if (!code && (!confirm(user, p, text) || ak_cancelled(observer)))
        code = APKG_CANCELLED;
    if (!code)
        code = apkg_plan_approve(c, p, &p->view.plan_digest, &a, e);
    if (!code)
        code = apkg_execute(c, p, a, observer, out, e);
    free(list);
    apkg_client_text_free(text);
    apkg_approval_free(a);
    apkg_plan_free(p);
    return code;
}
apkg_status apkg_client_source_add(const char *root, const char *name,
                                   const char *endpoint, const char *identity,
                                   apkg_confirm_fn confirm, void *user,
                                   const apkg_observer *observer,
                                   apkg_result **out, apkg_error **e) {
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    apkg_channel *channel = NULL;
    source *rows = NULL;
    size_t count = 0;
    apkg_status code;
    if (e)
        *e = NULL;
    if (out)
        *out = NULL;
    if (!root || !name || !endpoint || !identity || !confirm || !out ||
        !name_valid(name) || !endpoint_valid(endpoint) || !id_valid(identity))
        return APKG_INVALID;
    rows = malloc(AK_SOURCES * sizeof *rows);
    if (!rows)
        return APKG_NOMEM;
    code = open_root(root, APKG_WRITE, &c, &s, e);
    if (!code && state_list(s->raw, rows, &count))
        code = APKG_INVALID;
    for (size_t i = 0; !code && i < count; i++)
        if (!strcmp(rows[i].name, name) ||
            (!strcmp(rows[i].id, identity) && !strcmp(rows[i].endpoint, endpoint)))
            code = ak_fail(e, APKG_CONFLICT, "the root already lists this source",
                           rows[i].name);
    if (!code && count == AK_SOURCES)
        code = APKG_LIMIT;
    if (!code) {
        strcpy(rows[count].name, name);
        strcpy(rows[count].id, identity);
        strcpy(rows[count].endpoint, endpoint);
        /* The channel must open and verify under the current policy: adding
         * a source never stands in for granting its key. */
        code = open_channel(c, &rows[count], observer, &channel, e);
        count++;
    }
    if (!code)
        code = apply_list(c, s, rows, count, confirm, user, observer, out, e);
    apkg_channel_free(channel);
    apkg_snapshot_free(s);
    apkg_close(c);
    free(rows);
    return code;
}
apkg_status apkg_client_source_remove(const char *root, const char *name,
                                      apkg_confirm_fn confirm, void *user,
                                      const apkg_observer *observer,
                                      apkg_result **out, apkg_error **e) {
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    source *rows = NULL;
    size_t count = 0, k = 0;
    int found = 0;
    apkg_status code;
    if (e)
        *e = NULL;
    if (out)
        *out = NULL;
    if (!root || !name || !confirm || !out)
        return APKG_INVALID;
    rows = malloc(AK_SOURCES * sizeof *rows);
    if (!rows)
        return APKG_NOMEM;
    code = open_root(root, APKG_WRITE, &c, &s, e);
    if (!code && state_list(s->raw, rows, &count))
        code = APKG_INVALID;
    for (size_t i = 0; !code && i < count; i++) {
        if (!strcmp(rows[i].name, name)) {
            found = 1;
            continue;
        }
        rows[k++] = rows[i];
    }
    if (!code && !found)
        code = ak_fail(e, APKG_NOT_FOUND, "the root lists no such source", name);
    if (!code)
        code = apply_list(c, s, rows, k, confirm, user, observer, out, e);
    apkg_snapshot_free(s);
    apkg_close(c);
    free(rows);
    return code;
}
apkg_status apkg_client_source_list(const char *root, char **text,
                                    apkg_error **e) {
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    source *rows = NULL;
    size_t count = 0;
    ak_buf b = {0};
    apkg_status code;
    if (e)
        *e = NULL;
    if (!text)
        return APKG_INVALID;
    *text = NULL;
    if (!root)
        return APKG_INVALID;
    rows = malloc(AK_SOURCES * sizeof *rows);
    if (!rows)
        return APKG_NOMEM;
    code = open_root(root, APKG_READ, &c, &s, e);
    if (!code && state_list(s->raw, rows, &count))
        code = APKG_INVALID;
    b.max = AK_SOURCES * (AK_PATH + 160);
    for (size_t i = 0; !code && i < count; i++)
        ak_buf_add(&b, "%zu\t%s\t%s\t%s\n", i + 1, rows[i].name, rows[i].id,
                   rows[i].endpoint);
    if (!code && !b.data)
        b.data = ak_copy("", 0);
    if (!code)
        code = b.bad ? (apkg_status)b.bad : b.data ? APKG_OK : APKG_NOMEM;
    if (!code) {
        *text = b.data;
        b.data = NULL;
    }
    free(b.data);
    apkg_snapshot_free(s);
    apkg_close(c);
    free(rows);
    return code;
}
static int text_equal(apkg_text t, const char *s) {
    return t.size == strlen(s) && !memcmp(t.data, s, t.size);
}
apkg_status apkg_client_source_install(const char *root, const char *name,
                                       int recommendations,
                                       apkg_confirm_fn confirm, void *user,
                                       const apkg_observer *observer,
                                       apkg_result **out, apkg_error **e) {
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    apkg_channel *channels[AK_SOURCES] = {0};
    apkg_plan *p = NULL;
    apkg_approval *a = NULL;
    source *rows = NULL;
    char *text = NULL;
    size_t count = 0, best_k = 0;
    const apkg_channel_entry_view *best = NULL;
    int current = 0;
    apkg_status code;
    if (e)
        *e = NULL;
    if (out)
        *out = NULL;
    if (!root || !name || !*name || !confirm || !out ||
        (recommendations != 0 && recommendations != 1))
        return APKG_INVALID;
    rows = malloc(AK_SOURCES * sizeof *rows);
    if (!rows)
        return APKG_NOMEM;
    code = open_root(root, APKG_WRITE, &c, &s, e);
    if (!code && state_list(s->raw, rows, &count))
        code = APKG_INVALID;
    if (!code && !count)
        code = ak_fail(e, APKG_NOT_FOUND, "the root lists no sources", root);
    /* Every source is read; one that cannot be read refuses rather than
     * silently narrowing the choice. */
    for (size_t k = 0; !code && k < count; k++) {
        code = open_channel(c, &rows[k], observer, &channels[k], e);
        if (code && e && !*e)
            ak_fail(e, code, "a listed source cannot be read", rows[k].name);
    }
    for (size_t k = 0; !code && k < count; k++) {
        const apkg_channel_view *view = apkg_channel_get(channels[k]);
        for (uint64_t i = 0; !code && i < view->entry_count; i++) {
            const apkg_channel_entry_view *row;
            apkg_candidate_view state;
            code = apkg_channel_entry_at(channels[k], i, &row, e);
            if (!code)
                code = apkg_channel_compare(s, channels[k], i, &state, e);
            if (code || !text_equal(row->name, name))
                continue;
            if (state.state == APKG_CANDIDATE_CURRENT)
                current = 1;
            if ((state.state != APKG_CANDIDATE_NOT_INSTALLED &&
                 state.state != APKG_CANDIDATE_NEWER) ||
                row->author_trust != APKG_TRUST_ACCEPTED || !row->target_matches)
                continue;
            if (!best) {
                best = row;
                best_k = k;
                continue;
            }
            if (best->authority.size != row->authority.size ||
                memcmp(best->authority.data, row->authority.data,
                       row->authority.size)) {
                code = ak_fail(e, APKG_CONFLICT,
                               "sources offer this name from different "
                               "authors; install from one channel explicitly",
                               name);
                break;
            }
            char x[256], y[256];
            if (row->version.size >= sizeof x || best->version.size >= sizeof y) {
                code = APKG_LIMIT;
                break;
            }
            memcpy(x, row->version.data, row->version.size);
            x[row->version.size] = 0;
            memcpy(y, best->version.data, best->version.size);
            y[best->version.size] = 0;
            int order = ak_version_compare(x, y);
            if (order == 0)
                order = row->revision > best->revision   ? 1
                        : row->revision < best->revision ? -1
                                                         : 0;
            if (order == 2 ||
                (order == 0 && k == best_k &&
                 memcmp(row->descriptor_digest.bytes,
                        best->descriptor_digest.bytes, 32))) {
                code = ak_fail(e, APKG_CONFLICT,
                               "candidates have no defined order; install "
                               "from one channel explicitly",
                               name);
                break;
            }
            if (order == 1) { /* equal versions stay with the earlier source */
                best = row;
                best_k = k;
            }
        }
    }
    if (!code && !best) {
        if (current) {
            apkg_result *r = calloc(1, sizeof *r);
            if (!r)
                code = APKG_NOMEM;
            else {
                r->view.status = APKG_OK;
                r->view.outcome = APKG_UNCHANGED;
                *out = r;
            }
        } else
            code = ak_fail(e, APKG_NOT_FOUND,
                           "no listed source offers a trusted candidate", name);
    }
    if (!code && best) {
        code = ak_plan_channels_install(
            c, s, (const apkg_channel *const *)channels, count, best_k,
            &best->descriptor_digest, recommendations, observer, &p, e);
        if (!code)
            code = apkg_plan_format(p, &text);
        if (!code && (!confirm(user, p, text) || ak_cancelled(observer)))
            code = APKG_CANCELLED;
        if (!code)
            code = apkg_plan_approve(c, p, &p->view.plan_digest, &a, e);
        if (!code)
            code = apkg_execute(c, p, a, observer, out, e);
    }
    apkg_client_text_free(text);
    apkg_approval_free(a);
    apkg_plan_free(p);
    for (size_t k = 0; k < AK_SOURCES; k++)
        apkg_channel_free(channels[k]);
    apkg_snapshot_free(s);
    apkg_close(c);
    free(rows);
    return code;
}
