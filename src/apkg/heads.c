/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include <inttypes.h>
#include <string.h>
#define AK_HEADS_MAX 1024u
int ak_channel_heads_valid(const ak_json *j, int array) {
    if (array < 0)
        return 1; /* Pre-floor roots remain readable. */
    if (j->tokens[array].type != '[')
        return 0;
    char last[33] = "", id[33], sha[65];
    unsigned char bytes[32];
    uint64_t sequence;
    size_t count = 0;
    for (size_t i = (size_t)array + 1; i < j->tokens[array].next;
         i = j->tokens[i].next) {
        if (++count > AK_HEADS_MAX ||
            !ak_fields(
                j, (int)i,
                (const char *const[]){"id", "sequence", "snapshot_sha256"},
                3) ||
            ak_string(j, ak_member(j, (int)i, "id"), id, sizeof id) ||
            ak_hex(id, bytes, 16) || (*last && strcmp(last, id) >= 0) ||
            ak_u64(j, ak_member(j, (int)i, "sequence"), &sequence) ||
            !sequence ||
            ak_string(j, ak_member(j, (int)i, "snapshot_sha256"), sha,
                      sizeof sha) ||
            ak_hex(sha, bytes, 32))
            return 0;
        strcpy(last, id);
    }
    return 1;
}
apkg_status ak_channel_head_check(const apkg_snapshot *s, const apkg_id *id,
                                  uint64_t sequence, const apkg_digest *digest,
                                  apkg_u64_fact *accepted) {
    if (accepted)
        memset(accepted, 0, sizeof *accepted);
    if (!s->channel_heads)
        return APKG_INVALID;
    ak_json j = {0};
    int parsed = ak_json_parse(s->channel_heads, strlen(s->channel_heads), &j);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    char identity[33], sha[65];
    ak_tohex(id->bytes, 16, identity);
    apkg_status code = APKG_OK;
    for (size_t i = 1; i < j.tokens[0].next; i = j.tokens[i].next)
        if (ak_equal(&j, ak_member(&j, (int)i, "id"), identity)) {
            uint64_t floor;
            ak_u64(&j, ak_member(&j, (int)i, "sequence"), &floor);
            if (accepted) {
                accepted->state = APKG_KNOWN;
                accepted->value = floor;
                accepted->evidence.data = "root channel acceptance";
                accepted->evidence.size = strlen(accepted->evidence.data);
            }
            ak_tohex(digest->bytes, 32, sha);
            if (sequence < floor)
                code = APKG_STALE;
            else if (sequence == floor &&
                     !ak_equal(&j, ak_member(&j, (int)i, "snapshot_sha256"),
                               sha))
                code = APKG_CONFLICT;
            break;
        }
    ak_json_free(&j);
    return code;
}
apkg_status ak_channel_heads_add(const apkg_snapshot *s, const apkg_id *id,
                                 uint64_t sequence, const apkg_digest *digest,
                                 char **out) {
    *out = NULL;
    apkg_status code = ak_channel_head_check(s, id, sequence, digest, NULL);
    if (code)
        return code;
    ak_json j = {0};
    int parsed = ak_json_parse(s->channel_heads, strlen(s->channel_heads), &j);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    char identity[33], sha[65];
    ak_tohex(id->bytes, 16, identity);
    ak_tohex(digest->bytes, 32, sha);
    ak_buf b = {0};
    b.max = AK_MAX / 8;
    size_t count = 0;
    int added = 0;
    ak_buf_add(&b, "[");
    for (size_t i = 1; i < j.tokens[0].next; i = j.tokens[i].next) {
        char old_id[33];
        ak_string(&j, ak_member(&j, (int)i, "id"), old_id, sizeof old_id);
        int cmp = strcmp(identity, old_id);
        if (!added && cmp <= 0) {
            ak_buf_add(&b,
                       "%s{\"id\":\"%s\",\"sequence\":\"%" PRIu64
                       "\",\"snapshot_sha256\":\"%s\"}",
                       count++ ? "," : "", identity, sequence, sha);
            added = 1;
        }
        if (!cmp)
            continue;
        ak_buf_add(&b, "%s%.*s", count++ ? "," : "",
                   (int)(j.tokens[i].end - j.tokens[i].start),
                   j.text + j.tokens[i].start);
    }
    if (!added)
        ak_buf_add(&b,
                   "%s{\"id\":\"%s\",\"sequence\":\"%" PRIu64
                   "\",\"snapshot_sha256\":\"%s\"}",
                   count++ ? "," : "", identity, sequence, sha);
    ak_buf_add(&b, "]");
    ak_json_free(&j);
    if (b.bad || count > AK_HEADS_MAX) {
        code = b.bad ? b.bad : APKG_LIMIT;
        free(b.data);
        return code;
    }
    *out = b.data;
    return APKG_OK;
}
