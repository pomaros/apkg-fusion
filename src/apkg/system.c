/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
/* System requirements: what the machine must already provide.
 *
 * A descriptor may declare `requires_system`, an ordered array of
 * {"id","min_version","type"} entries signed with the rest of the package.
 * They are not dependencies: no package in any channel can satisfy one, so a
 * missing requirement never turns into a search for a provider.
 *
 * Planning an install, upgrade or resolution probes every requirement of
 * every package it brings in and binds the observations into the canonical
 * plan, so the operator approves exactly what was observed. Three outcomes,
 * as in arospkg:
 *
 *   satisfied     met here, now;
 *   missing       not met; the plan is refused before anything is fetched;
 *   undetermined  this client cannot decide (another requirement type, or a
 *                 host build); the plan proceeds and shows the doubt.
 *
 * Execution probes again and refuses a plan whose observations changed.
 * Installation-time observations are history, not a current qualification:
 * a launch may need a fresh probe. */
#include "internal.h"
#include "apkg_client.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __AROS__
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <proto/dos.h>
#include <proto/exec.h>
#endif

#define AK_REQUIREMENTS 16u
#define AK_OBSERVATIONS 4096u

typedef struct {
    char type[24], id[64], detail[160];
    uint64_t min_version;
    int state;       /* 0 satisfied, 1 missing, 2 undetermined */
    long version;    /* -1 unknown */
} ak_probe;

static const char *const state_names[] = {"satisfied", "missing",
                                          "undetermined"};

static int valid_type(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 23)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!islower((unsigned char)s[i]))
            return 0;
    return 1;
}
static int valid_id(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 63 || !isalnum((unsigned char)s[0]))
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || strchr("._+-", s[i])))
            return 0;
    return 1;
}
/* One entry's fields; 0 on success. */
static int entry(const ak_json *j, int e, char type[24], char id[64],
                 uint64_t *min) {
    static const char *const f[] = {"id", "min_version", "type"};
    return !ak_fields(j, e, f, 3) ||
           ak_string(j, ak_member(j, e, "type"), type, 24) ||
           !valid_type(type) || ak_string(j, ak_member(j, e, "id"), id, 64) ||
           !valid_id(id) || ak_u64(j, ak_member(j, e, "min_version"), min) ||
           *min > 65535u;
}
int ak_system_valid(const ak_json *j, int array) {
    char last[96] = "", key[96], type[24], id[64];
    uint64_t min;
    size_t n = 0;
    if (array < 0)
        return 1;
    if (j->tokens[array].type != '[' || ak_empty(j, array))
        return 0; /* an empty list is omitted, never written */
    for (size_t i = (size_t)array + 1; i < j->tokens[array].next;
         i = j->tokens[i].next) {
        if (++n > AK_REQUIREMENTS || entry(j, (int)i, type, id, &min))
            return 0;
        snprintf(key, sizeof key, "%s/%s", type, id);
        if (*last && strcmp(last, key) >= 0)
            return 0;
        strcpy(last, key);
    }
    return 1;
}

/* ------------------------------------------------------------------ probe */

#ifdef APKG_TESTING
static char *test_spec;
static int test_active;
void apkg_test_system(const char *spec) {
    free(test_spec);
    test_spec = spec ? ak_copy(spec, strlen(spec)) : NULL;
    test_active = spec != NULL;
}
/* "a.library=3,b.library=1": listed libraries exist at that version, every
 * other library is missing, every other type is undetermined. */
static int test_probe(ak_probe *r) {
    if (!test_active)
        return 0;
    if (strcmp(r->type, "library")) {
        r->state = 2;
        snprintf(r->detail, sizeof r->detail,
                 "requirement type %s is not one this client can check",
                 r->type);
        return 1;
    }
    for (const char *at = test_spec; at && *at;) {
        const char *eq = strchr(at, '='), *end = strchr(at, ',');
        if (!end)
            end = at + strlen(at);
        if (eq && eq < end && (size_t)(eq - at) == strlen(r->id) &&
            !memcmp(at, r->id, strlen(r->id))) {
            r->version = strtol(eq + 1, NULL, 10);
            r->state = (uint64_t)r->version >= r->min_version ? 0 : 1;
            snprintf(r->detail, sizeof r->detail,
                     r->state ? "version %ld, %" PRIu64 " required"
                              : "opens, version %ld",
                     r->version, r->min_version);
            return 1;
        }
        at = *end ? end + 1 : end;
    }
    r->state = 1;
    snprintf(r->detail, sizeof r->detail, "not on this machine");
    return 1;
}
#endif

#ifdef __AROS__
/* Cheapest test first: the resident list, then OpenLibrary(), which runs the
 * library's initialisation exactly as the program will, then the file in
 * LIBS: to tell "absent" from "present but will not open". A machine short
 * of memory can make an existing library look absent. */
static void probe_library(ak_probe *r) {
    struct Library *l;
    char path[80];
    BPTR lock;
    Forbid();
    l = (struct Library *)FindName(&SysBase->LibList, (STRPTR)r->id);
    if (l)
        r->version = l->lib_Version;
    Permit();
    if (l && (uint64_t)r->version >= r->min_version) {
        r->state = 0;
        snprintf(r->detail, sizeof r->detail, "already open, version %ld",
                 r->version);
        return;
    }
    l = OpenLibrary((CONST_STRPTR)r->id, (ULONG)r->min_version);
    if (l) {
        r->version = l->lib_Version;
        CloseLibrary(l);
        r->state = 0;
        snprintf(r->detail, sizeof r->detail, "opens, version %ld",
                 r->version);
        return;
    }
    r->state = 1;
    snprintf(path, sizeof path, "LIBS:%s", r->id);
    lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (lock) {
        UnLock(lock);
        snprintf(r->detail, sizeof r->detail,
                 "%s exists but does not open%s", path,
                 r->min_version ? " at the required version" : "");
    } else if (r->min_version)
        snprintf(r->detail, sizeof r->detail,
                 "not available at version %" PRIu64 " or later",
                 r->min_version);
    else
        snprintf(r->detail, sizeof r->detail, "not on this machine");
}
/* Resources guarantee a Node, not a Library (FileSysResource is one example).
 * Only devices expose lib_Version. A device that is not loaded but present
 * in DEVS: cannot be versioned without opening a unit. */
static void probe_list(ak_probe *r, struct List *list, const char *dir) {
    struct Library *l;
    char path[80];
    BPTR lock;
    Forbid();
    l = (struct Library *)FindName(list, (STRPTR)r->id);
    if (l && dir)
        r->version = l->lib_Version;
    Permit();
    if (l) {
        if (!dir) {
            r->state = r->min_version ? 2 : 0;
            snprintf(r->detail, sizeof r->detail,
                     "resident resource; no generic version information");
            return;
        }
        r->state = (uint64_t)r->version >= r->min_version ? 0 : 1;
        snprintf(r->detail, sizeof r->detail, "resident, version %ld",
                 r->version);
        return;
    }
    if (dir) {
        snprintf(path, sizeof path, "%s%s", dir, r->id);
        lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
        if (lock) {
            UnLock(lock);
            r->state = 2;
            snprintf(r->detail, sizeof r->detail,
                     "%s exists; its version is not checked without opening "
                     "a unit",
                     path);
            return;
        }
    }
    r->state = 1;
    snprintf(r->detail, sizeof r->detail, "not on this machine");
}
#endif

static void probe(ak_probe *r) {
    r->version = -1;
#ifdef APKG_TESTING
    if (test_probe(r))
        return;
#endif
#ifdef __AROS__
    if (!strcmp(r->type, "library")) {
        probe_library(r);
        return;
    }
    if (!strcmp(r->type, "resource")) {
        probe_list(r, &SysBase->ResourceList, NULL);
        return;
    }
    if (!strcmp(r->type, "device")) {
        probe_list(r, &SysBase->DeviceList, "DEVS:");
        return;
    }
#else
    if (!strcmp(r->type, "library") || !strcmp(r->type, "resource") ||
        !strcmp(r->type, "device")) {
        r->state = 2;
        snprintf(r->detail, sizeof r->detail,
                 "this host build cannot probe an AROS %s", r->type);
        return;
    }
#endif
    r->state = 2;
    snprintf(r->detail, sizeof r->detail,
             "requirement type %s is not one this client can check", r->type);
}

apkg_status apkg_package_requirements(const apkg_package *package, char **out,
                                      apkg_error **error) {
    if (error) *error = NULL;
    if (!out) return APKG_INVALID;
    *out = NULL;
    if (!package) return APKG_INVALID;
    const ak_json *j = &package->json;
    int array = ak_member(j, ak_member(j, 0, "body"), "requires_system");
    ak_buf b = {0}; b.max = 65535;
    ak_buf_add(&b, "System requirements observed now for %s/%s %s (revision %" PRIu64 ")\n",
               package->authority, package->name, package->version, package->revision);
    if (array < 0) ak_buf_add(&b, "No system requirements declared.");
    else for (size_t i = (size_t)array + 1; i < j->tokens[array].next; i = j->tokens[i].next) {
        ak_probe r = {0};
        if (entry(j, (int)i, r.type, r.id, &r.min_version)) {
            free(b.data); return APKG_INVALID;
        }
        probe(&r);
        ak_buf_add(&b, "%s%s %s (minimum %" PRIu64 "): %s (%s)",
                   i == (size_t)array + 1 ? "" : "\n", r.type, r.id,
                   r.min_version, state_names[r.state], r.detail);
    }
    if (b.bad) { free(b.data); return ak_fail(error, (apkg_status)b.bad, "requirements report unavailable", ""); }
    *out = b.data;
    return APKG_OK;
}

/* ------------------------------------------------------- plan observations */

static int compare_names(const void *a, const void *b) {
    const apkg_package *x = *(const apkg_package *const *)a,
                       *y = *(const apkg_package *const *)b;
    return strcmp(x->name, y->name);
}
static void observation(ak_buf *b, const char *package, const ak_probe *r) {
    char version[24] = "";
    if (r->version >= 0)
        snprintf(version, sizeof version, "%ld", r->version);
    ak_buf_add(b,
               "%s{\"detail\":\"%s\",\"id\":\"%s\",\"min_version\":\"%" PRIu64
               "\",\"package\":\"%s\",\"state\":\"%s\",\"type\":\"%s\","
               "\"version\":\"%s\"}",
               b->size > 1 ? "," : "", r->detail, r->id, r->min_version,
               package, state_names[r->state], r->type, version);
}
apkg_status ak_system_observe(const apkg_package *const *packages, size_t n,
                              size_t max, char **out, apkg_error **e) {
    const apkg_package **sorted;
    ak_buf b = {0}, missing = {0};
    size_t total = 0;
    apkg_status code = APKG_OK;
    *out = NULL;
    if (!n)
        return APKG_OK;
    sorted = malloc(n * sizeof *sorted);
    if (!sorted)
        return APKG_NOMEM;
    memcpy(sorted, packages, n * sizeof *sorted);
    qsort(sorted, n, sizeof *sorted, compare_names);
    b.max = missing.max = max;
    ak_buf_add(&b, "[");
    for (size_t k = 0; k < n && !code; k++) {
        const ak_json *j = &sorted[k]->json;
        int arr =
            ak_member(j, ak_member(j, 0, "body"), "requires_system");
        if (arr < 0)
            continue;
        for (size_t i = (size_t)arr + 1; i < j->tokens[arr].next;
             i = j->tokens[i].next) {
            ak_probe r;
            memset(&r, 0, sizeof r);
            if (++total > AK_OBSERVATIONS) {
                code = APKG_LIMIT;
                break;
            }
            if (entry(j, (int)i, r.type, r.id, &r.min_version)) {
                code = APKG_INVALID;
                break;
            }
            probe(&r);
            observation(&b, sorted[k]->name, &r);
            if (r.state == 1)
                ak_buf_add(&missing, "%s%s %s %s (%s)", missing.size ? "; " : "",
                           sorted[k]->name, r.type, r.id, r.detail);
        }
    }
    free(sorted);
    ak_buf_add(&b, "]");
    if (!code && (b.bad || missing.bad))
        code = b.bad ? (apkg_status)b.bad : (apkg_status)missing.bad;
    if (!code && missing.size)
        code = ak_fail(e, APKG_REQUIREMENT,
                       "this system does not provide what the package needs",
                       missing.data);
    if (!code && total)
        *out = b.data;
    else
        free(b.data);
    free(missing.data);
    return code;
}
int ak_requirements_valid(const ak_json *j, int array) {
    static const char *const f[] = {"detail",  "id",   "min_version", "package",
                                    "state",   "type", "version"};
    size_t n = 0;
    if (array < 0)
        return 1;
    if (j->tokens[array].type != '[' || ak_empty(j, array))
        return 0;
    for (size_t i = (size_t)array + 1; i < j->tokens[array].next;
         i = j->tokens[i].next) {
        char text[256];
        uint64_t number;
        int state = -1;
        if (++n > AK_OBSERVATIONS || !ak_fields(j, (int)i, f, 7) ||
            ak_string(j, ak_member(j, (int)i, "type"), text, 24) ||
            !valid_type(text) ||
            ak_string(j, ak_member(j, (int)i, "id"), text, 64) ||
            !valid_id(text) ||
            ak_string(j, ak_member(j, (int)i, "package"), text, 256) ||
            !ak_valid_name(text) ||
            ak_u64(j, ak_member(j, (int)i, "min_version"), &number) ||
            number > 65535u ||
            ak_string(j, ak_member(j, (int)i, "detail"), text, 160) ||
            ak_string(j, ak_member(j, (int)i, "version"), text, 24))
            return 0;
        for (int s = 0; s < 3; s++)
            if (ak_equal(j, ak_member(j, (int)i, "state"), state_names[s]))
                state = s;
        if (state < 0)
            return 0;
    }
    return 1;
}
apkg_status ak_plan_system_check(const apkg_plan *p) {
    ak_json j = {0};
    apkg_status code = APKG_OK;
    if (!p->requirements)
        return APKG_OK;
    int parsed = ak_json_parse(p->requirements, strlen(p->requirements), &j);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    if (!ak_requirements_valid(&j, 0)) {
        ak_json_free(&j);
        return APKG_INVALID;
    }
    for (size_t i = 1; i < j.tokens[0].next && !code; i = j.tokens[i].next) {
        ak_probe r;
        char version[24], now[24] = "";
        memset(&r, 0, sizeof r);
        ak_string(&j, ak_member(&j, (int)i, "type"), r.type, sizeof r.type);
        ak_string(&j, ak_member(&j, (int)i, "id"), r.id, sizeof r.id);
        ak_u64(&j, ak_member(&j, (int)i, "min_version"), &r.min_version);
        ak_string(&j, ak_member(&j, (int)i, "version"), version,
                  sizeof version);
        probe(&r);
        if (r.version >= 0)
            snprintf(now, sizeof now, "%ld", r.version);
        if (!ak_equal(&j, ak_member(&j, (int)i, "state"),
                      state_names[r.state]) ||
            strcmp(version, now))
            code = APKG_STALE;
    }
    ak_json_free(&j);
    return code;
}
apkg_status ak_system_format(const char *requirements, ak_buf *b) {
    ak_json j = {0};
    if (!requirements)
        return APKG_OK;
    int parsed = ak_json_parse(requirements, strlen(requirements), &j);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    ak_buf_add(b, "\nSystem requirements, observed on this machine at "
                  "planning:\n");
    for (size_t i = 1; i < j.tokens[0].next; i = j.tokens[i].next) {
        char type[24], id[64], package[256], state[16], detail[160];
        uint64_t min = 0;
        ak_string(&j, ak_member(&j, (int)i, "type"), type, sizeof type);
        ak_string(&j, ak_member(&j, (int)i, "id"), id, sizeof id);
        ak_string(&j, ak_member(&j, (int)i, "package"), package,
                  sizeof package);
        ak_string(&j, ak_member(&j, (int)i, "state"), state, sizeof state);
        ak_string(&j, ak_member(&j, (int)i, "detail"), detail, sizeof detail);
        ak_u64(&j, ak_member(&j, (int)i, "min_version"), &min);
        if (min)
            ak_buf_add(b, "  %s needs %s %s version %" PRIu64 ": %s (%s)\n",
                       package, type, id, min, state, detail);
        else
            ak_buf_add(b, "  %s needs %s %s: %s (%s)\n", package, type, id,
                       state, detail);
    }
    ak_json_free(&j);
    return APKG_OK;
}
