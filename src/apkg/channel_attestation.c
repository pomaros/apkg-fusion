/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include <string.h>
#define CLAIMS_PER_ENTRY 64
static apkg_bytes token_bytes(const ak_json *j, int t) {
    return (apkg_bytes){(const unsigned char *)j->text + j->tokens[t].start,
                        j->tokens[t].end - j->tokens[t].start};
}
apkg_status ak_channel_attestations(const apkg_snapshot *snapshot,
                                    apkg_bytes descriptor, apkg_bytes signature,
                                    const ak_json *j, int array,
                                    apkg_attestation ***out,
                                    size_t *count_out) {
    if (out)
        *out = NULL;
    if (count_out)
        *count_out = 0;
    if (array < 0)
        return APKG_OK;
    if (j->tokens[array].type != '[')
        return APKG_UNSUPPORTED;
    size_t count = 0;
    for (size_t i = (size_t)array + 1; i < j->tokens[array].next;
         i = j->tokens[i].next)
        if (++count > CLAIMS_PER_ENTRY)
            return APKG_LIMIT;
    if (!count)
        return APKG_OK;
    apkg_attestation **claims = calloc(count, sizeof *claims);
    if (!claims)
        return APKG_NOMEM;
    apkg_status code = APKG_OK;
    size_t index = 0;
    for (size_t i = (size_t)array + 1; i < j->tokens[array].next;
         i = j->tokens[i].next, index++) {
        if (!ak_fields(j, (int)i, (const char *const[]){"record", "signature"},
                       2)) {
            code = APKG_UNSUPPORTED;
            goto done;
        }
        code = ak_attestation_observe(
            snapshot, descriptor, signature,
            token_bytes(j, ak_member(j, (int)i, "record")),
            token_bytes(j, ak_member(j, (int)i, "signature")), NULL,
            &claims[index], NULL);
        if (code)
            goto done;
        const apkg_attestation_view *v = apkg_attestation_get(claims[index]);
        if (index &&
            memcmp(apkg_attestation_get(claims[index - 1])->record_digest.bytes,
                   v->record_digest.bytes, 32) >= 0) {
            code = APKG_CONFLICT;
            goto done;
        }
        for (size_t k = 0; k < index; k++)
            if (v->sequence == apkg_attestation_get(claims[k])->sequence &&
                ak_attestation_scope_equal(claims[index], claims[k])) {
                code = APKG_CONFLICT;
                goto done;
            }
    }
    unsigned char processed[CLAIMS_PER_ENTRY] = {0};
    for (size_t pass = 0; pass < count; pass++) {
        size_t chosen = count;
        for (size_t i = 0; i < count; i++)
            if (!processed[i] &&
                (chosen == count ||
                 apkg_attestation_get(claims[i])->sequence <
                     apkg_attestation_get(claims[chosen])->sequence))
                chosen = i;
        processed[chosen] = 1;
        const apkg_attestation_view *v = apkg_attestation_get(claims[chosen]);
        if (!v->has_previous)
            continue;
        const apkg_attestation *previous = NULL;
        int contradictory = 0;
        for (size_t k = 0; k < count; k++) {
            const apkg_attestation_view *p = apkg_attestation_get(claims[k]);
            if (!memcmp(p->record_digest.bytes, v->supersedes.bytes, 32))
                previous = claims[k];
            if (p->sequence == v->sequence - 1 &&
                ak_attestation_scope_equal(claims[chosen], claims[k]))
                contradictory = 1;
        }
        if (!previous) {
            if (contradictory) {
                code = APKG_CONFLICT;
                goto done;
            }
            continue;
        }
        apkg_attestation *linked = NULL;
        code = ak_attestation_observe(snapshot, descriptor, signature,
                                      v->record, v->signature_envelope,
                                      previous, &linked, NULL);
        if (code)
            goto done;
        apkg_attestation_free(claims[chosen]);
        claims[chosen] = linked;
    }
    if (out) {
        *out = claims;
        if (count_out)
            *count_out = count;
        claims = NULL;
    }
done:
    if (claims) {
        for (size_t i = 0; i < count; i++)
            apkg_attestation_free(claims[i]);
        free(claims);
    }
    return code;
}
int ak_channel_claims_json(const apkg_channel *channel, uint64_t entry,
                           ak_buf *b) {
    const apkg_channel_entry_view *row = NULL;
    apkg_status code = apkg_channel_entry_at(channel, entry, &row, NULL);
    if (code) {
        b->bad = code;
        return -1;
    }
    ak_buf_add(b, "[");
    for (uint64_t i = 0; i < row->attestation_count && !b->bad; i++) {
        const apkg_attestation_view *v = NULL;
        code = apkg_channel_attestation_at(channel, entry, i, &v, NULL);
        if (code) {
            b->bad = code;
            return -1;
        }
        char hash[65], snapshot[65];
        ak_tohex(v->snapshot_digest.bytes, 32, snapshot);
        ak_tohex(v->record_digest.bytes, 32, hash);
        ak_buf_add(
            b,
            "%s{\"author_trust\":\"%s\",\"freshness\":\"unknown\",\"history\":"
            "\"%s\",\"independent\":%s,\"issuer_trust\":\"%s\",\"root_"
            "generation\":\"%llu\",\"root_snapshot_sha256\":\"%s\",\"record\":%"
            ".*s,"
            "\"record_sha256\":\"%s\",\"signature\":%.*s,\"signature_state\":"
            "\"verified\",\"target_matches\":%s}",
            i ? "," : "",
            v->author_trust == APKG_TRUST_ACCEPTED ? "accepted" : "rejected",
            v->prefix_complete ? "linked-prefix" : "not-checked",
            v->independent ? "true" : "false",
            v->issuer_trust == APKG_TRUST_ACCEPTED ? "accepted" : "rejected",
            (unsigned long long)v->generation, snapshot, (int)v->record.size,
            (const char *)v->record.data, hash, (int)v->signature_envelope.size,
            (const char *)v->signature_envelope.data,
            v->target_matches ? "true" : "false");
    }
    return ak_buf_add(b, "]");
}
apkg_status apkg_channel_attestations_format(const apkg_channel *channel,
                                             uint64_t entry, char **out,
                                             apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    const apkg_channel_entry_view *row = NULL;
    apkg_status code = apkg_channel_entry_at(channel, entry, &row, error);
    if (code)
        return code;
    ak_buf b = {0};
    b.max = AK_MAX / 8;
    if (!row->attestation_count)
        ak_buf_add(&b, "No signed qualification observations supplied.\n");
    for (uint64_t i = 0; i < row->attestation_count && !b.bad; i++) {
        char *text = NULL;
        code = ak_channel_claim_format(channel, entry, i, &text, error);
        if (code)
            break;
        ak_buf_add(&b, "Observation %llu:\n%s\n", (unsigned long long)i, text);
        apkg_client_text_free(text);
    }
    if (!code)
        code = (apkg_status)b.bad;
    if (!code) {
        *out = b.data;
        b.data = NULL;
    }
    free(b.data);
    return code;
}
