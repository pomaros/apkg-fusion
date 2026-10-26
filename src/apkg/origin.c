/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include <errno.h>
#include <string.h>
/* A plan owns these records. Installed state deliberately does not: retained
 * content and recovery must remain usable without consulting remote sources. */
static int compare(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
int ak_origins_valid(const ak_json *j, int array) {
    static const char *const fields[] = {
        "anchor_dev",      "anchor_ino", "anchor_path", "endpoint", "id",
        "policy_sha256",   "root_id",    "root_path",   "sequence", "signer",
        "snapshot_sha256", "target",     "transport"};
    if (array < 0)
        return 1; /* Older journals have no live-source obligation. */
    if (j->tokens[array].type != '[')
        return 0;
    size_t count = 0;
    for (size_t i = (size_t)array + 1; i < j->tokens[array].next;
         i = j->tokens[i].next) {
        uint64_t number;
        char text[AK_PATH * 2 + 1];
        unsigned char bytes[AK_PATH];
        if (++count > 256 || !ak_fields(j, (int)i, fields, 13))
            return 0;
        for (size_t k = 0; k < 13; k++) {
            int token = ak_member(j, (int)i, fields[k]);
            if (k == 0 || k == 1 || k == 8) {
                if (ak_u64(j, token, &number) || (k != 0 && !number))
                    return 0;
            } else if (ak_string(j, token, text, sizeof text))
                return 0;
            else if (k == 2 || k == 3 || k == 7) {
                size_t n = strlen(text);
                if (!n || n % 2 || n >= AK_PATH * 2 ||
                    ak_hex(text, bytes, n / 2) || memchr(bytes, 0, n / 2) ||
                    !ak_text_path_valid((const char *)bytes, n / 2))
                    return 0;
            } else if (k == 4 || k == 6) {
                if (ak_hex(text, bytes, 16))
                    return 0;
            } else if (k == 5 || k == 9 || k == 10) {
                if (ak_hex(text, bytes, 32))
                    return 0;
            } else if (k == 11) {
                if (strlen(text) >= 32)
                    return 0;
            } else if (strcmp(text, "file") && strcmp(text, "http"))
                return 0;
        }
    }
    return 1;
}
apkg_status ak_plan_origins_check(apkg_ctx *c, const apkg_plan *p,
                                  const apkg_observer *observer) {
    if (!p->origins)
        return APKG_OK;
    ak_json j = {0};
    int parsed = ak_json_parse(p->origins, strlen(p->origins), &j);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    apkg_status code = APKG_INVALID;
    if (ak_origins_valid(&j, 0)) {
        code = APKG_OK;
        for (size_t i = 1; i < j.tokens[0].next; i = j.tokens[i].next) {
            code = ak_origin_check(c, &j, (int)i, observer);
            if (code)
                break;
        }
    }
    ak_json_free(&j);
    return code;
}
apkg_status ak_plan_bind(apkg_ctx *c, apkg_plan *p,
                         const apkg_package *const *packages, size_t count) {
    const char *records[256];
    size_t n = 0;
    ak_buf list = {0}, raw = {0};
    if (count > 256 || p->origins)
        return APKG_LIMIT;
    for (size_t i = 0; i < count; i++)
        if (packages[i]->origin)
            records[n++] = packages[i]->origin;
    if (!n)
        return APKG_OK;
    qsort(records, n, sizeof *records, compare);
    list.max = raw.max =
        c->object_max < c->memory_max / 32 ? c->object_max : c->memory_max / 32;
    ak_buf_add(&list, "[");
    for (size_t i = 0; i < n; i++)
        if (!i || strcmp(records[i - 1], records[i]))
            ak_buf_add(&list, "%s%s", list.size > 1 ? "," : "", records[i]);
    ak_buf_add(&list, "]");
    const char *at = strstr(p->raw, ",\"root_path\":");
    if (!at) {
        free(list.data);
        return APKG_INVALID;
    }
    if (!list.bad)
        ak_buf_add(&raw, "%.*s,\"origins\":%s%s", (int)(at - p->raw), p->raw,
                   list.data, at);
    if (list.bad || raw.bad) {
        apkg_status code = list.bad ? list.bad : raw.bad;
        free(list.data);
        free(raw.data);
        return code;
    }
    /* Validate before publishing new canonical plan bytes. No side effects. */
    apkg_plan candidate = *p;
    candidate.origins = list.data;
    apkg_status code = ak_plan_origins_check(c, &candidate, NULL);
    if (code) {
        free(list.data);
        free(raw.data);
        return code;
    }
    free(p->raw);
    p->raw = raw.data;
    p->origins = list.data;
    p->view.canonical.data = (unsigned char *)p->raw;
    p->view.canonical.size = raw.size;
    ak_hash(p->raw, raw.size, &p->view.plan_digest);
    return APKG_OK;
}
