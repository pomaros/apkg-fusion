/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include <errno.h>
#include <string.h>
#define RECORDS_MAX 1024
static apkg_bytes bytes(const ak_json *j, int t) {
    return (apkg_bytes){(const unsigned char *)j->text + j->tokens[t].start,
                        j->tokens[t].end - j->tokens[t].start};
}
typedef struct {
    apkg_digest digest, scope, previous;
    uint64_t sequence, time;
    int has_previous;
} remembered;
int ak_qualification_records_valid(const ak_json *j, int array) {
    errno = 0;
    if (array < 0)
        return 1;
    if (j->tokens[array].type != '[')
        return 0;
    size_t count = 0;
    for (size_t i = (size_t)array + 1; i < j->tokens[array].next;
         i = j->tokens[i].next)
        if (++count > RECORDS_MAX)
            return 0;
    if (!count)
        return 1;
    remembered *rows = calloc(count ? count : 1, sizeof *rows);
    if (!rows) {
        errno = ENOMEM;
        return 0;
    }
    size_t n = 0;
    int valid = 0;
    for (size_t i = (size_t)array + 1; i < j->tokens[array].next;
         i = j->tokens[i].next, n++) {
        if (!ak_fields(j, (int)i, (const char *const[]){"record", "signature"},
                       2))
            goto done;
        apkg_bytes d = bytes(j, ak_member(j, (int)i, "record")),
                   s = bytes(j, ak_member(j, (int)i, "signature"));
        ak_json record = {0};
        int parsed = ak_json_parse(d.data, d.size, &record);
        if (parsed) {
            if (parsed == -2)
                errno = ENOMEM;
            goto done;
        }
        unsigned char key[32];
        apkg_status code = ak_attestation_validate(&record);
        if (!code)
            code =
                ak_signature_check(d, s, "attestation", key, &rows[n].digest);
        if (!code)
            code = ak_attestation_scope(&record, &rows[n].scope);
        int b = ak_member(&record, 0, "body");
        if (!code) {
            ak_u64(&record, ak_member(&record, b, "sequence"),
                   &rows[n].sequence);
            ak_u64(&record, ak_member(&record, b, "issued_at"), &rows[n].time);
            rows[n].has_previous = rows[n].sequence > 1;
            if (rows[n].has_previous) {
                char hex[65];
                ak_string(&record, ak_member(&record, b, "supersedes"), hex,
                          sizeof hex);
                ak_hex(hex, rows[n].previous.bytes, 32);
            }
        }
        ak_json_free(&record);
        if (code) {
            if (code == APKG_NOMEM)
                errno = ENOMEM;
            goto done;
        }
        if (n &&
            memcmp(rows[n - 1].digest.bytes, rows[n].digest.bytes, 32) >= 0)
            goto done;
        for (size_t k = 0; k < n; k++)
            if (rows[k].sequence == rows[n].sequence &&
                !memcmp(rows[k].scope.bytes, rows[n].scope.bytes, 32))
                goto done;
    }
    for (size_t i = 0; i < count; i++)
        if (rows[i].has_previous) {
            int found = 0, contradictory = 0;
            for (size_t k = 0; k < count; k++) {
                if (!memcmp(rows[k].scope.bytes, rows[i].scope.bytes, 32) &&
                    rows[k].sequence == rows[i].sequence - 1)
                    contradictory = 1;
                if (!memcmp(rows[k].digest.bytes, rows[i].previous.bytes, 32)) {
                    if (memcmp(rows[k].scope.bytes, rows[i].scope.bytes, 32) ||
                        rows[k].sequence == UINT64_MAX ||
                        rows[k].sequence + 1 != rows[i].sequence ||
                        rows[k].time > rows[i].time)
                        goto done;
                    found = 1;
                }
            }
            if (!found && contradictory)
                goto done;
        }
    valid = 1;
done:
    free(rows);
    return valid;
}
typedef struct {
    apkg_digest digest;
    char *raw;
} record_copy;
static int order(const void *a, const void *b) {
    return memcmp(((const record_copy *)a)->digest.bytes,
                  ((const record_copy *)b)->digest.bytes, 32);
}
apkg_status ak_qualification_records_add(const apkg_snapshot *snapshot,
                                         const apkg_channel *channel,
                                         char **out) {
    *out = NULL;
    ak_json stored = {0};
    record_copy *records = NULL;
    size_t count = 0;
    apkg_status code = APKG_OK;
    ak_buf result = {0};
    result.max = AK_MAX / 8;
    int parsed =
        ak_json_parse(snapshot->qualification_records,
                      strlen(snapshot->qualification_records), &stored);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    records = calloc(RECORDS_MAX, sizeof *records);
    if (!records) {
        code = APKG_NOMEM;
        goto done;
    }
    for (size_t i = 1; i < stored.tokens[0].next; i = stored.tokens[i].next) {
        if (count == RECORDS_MAX) {
            code = APKG_LIMIT;
            goto done;
        }
        apkg_bytes d = bytes(&stored, ak_member(&stored, (int)i, "record"));
        ak_hash(d.data, d.size, &records[count].digest);
        records[count].raw = ak_slice(&stored, (int)i);
        if (!records[count].raw) {
            code = APKG_NOMEM;
            goto done;
        }
        count++;
    }
    const apkg_channel_view *source = apkg_channel_get(channel);
    for (uint64_t i = 0; i < source->entry_count; i++) {
        const apkg_channel_entry_view *entry = NULL;
        code = apkg_channel_entry_at(channel, i, &entry, NULL);
        if (code)
            goto done;
        for (uint64_t k = 0; k < entry->attestation_count; k++) {
            const apkg_attestation_view *v = NULL;
            code = apkg_channel_attestation_at(channel, i, k, &v, NULL);
            if (code)
                goto done;
            if (v->issuer_trust != APKG_TRUST_ACCEPTED || !v->independent)
                continue;
            ak_buf record = {0};
            record.max = AK_MAX / 8;
            ak_buf_add(&record, "{\"record\":%.*s,\"signature\":%.*s}",
                       (int)v->record.size, (const char *)v->record.data,
                       (int)v->signature_envelope.size,
                       (const char *)v->signature_envelope.data);
            if (record.bad) {
                code = (apkg_status)record.bad;
                free(record.data);
                goto done;
            }
            size_t n = 0;
            while (n < count &&
                   memcmp(records[n].digest.bytes, v->record_digest.bytes, 32))
                n++;
            if (n < count) {
                if (strcmp(records[n].raw, record.data))
                    code = APKG_CONFLICT;
                free(record.data);
                if (code)
                    goto done;
                continue;
            }
            if (count == RECORDS_MAX) {
                free(record.data);
                code = APKG_LIMIT;
                goto done;
            }
            records[count++] = (record_copy){v->record_digest, record.data};
        }
    }
    qsort(records, count, sizeof *records, order);
    ak_buf_add(&result, "[");
    for (size_t i = 0; i < count; i++)
        ak_buf_add(&result, "%s%s", i ? "," : "", records[i].raw);
    ak_buf_add(&result, "]");
    if (result.bad) {
        code = (apkg_status)result.bad;
        goto done;
    }
    ak_json check = {0};
    parsed = ak_json_parse(result.data, result.size, &check);
    if (parsed)
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    else if (!ak_qualification_records_valid(&check, 0))
        code = errno == ENOMEM ? APKG_NOMEM : APKG_CONFLICT;
    ak_json_free(&check);
    if (!code) {
        *out = result.data;
        result.data = NULL;
    }
done:
    if (records) {
        for (size_t i = 0; i < count; i++)
            free(records[i].raw);
        free(records);
    }
    free(result.data);
    ak_json_free(&stored);
    return code;
}
apkg_status ak_qualification_augment(const apkg_snapshot *snapshot,
                                     apkg_bytes descriptor,
                                     apkg_bytes signature,
                                     apkg_attestation ***claims,
                                     size_t *count) {
    if (!snapshot)
        return APKG_OK;
    ak_json stored = {0};
    int parsed =
        ak_json_parse(snapshot->qualification_records,
                      strlen(snapshot->qualification_records), &stored);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    apkg_digest digest;
    ak_hash(descriptor.data, descriptor.size, &digest);
    char wanted[65];
    ak_tohex(digest.bytes, 32, wanted);
    size_t possible = 0;
    for (size_t i = 1; i < stored.tokens[0].next; i = stored.tokens[i].next) {
        int record = ak_member(&stored, (int)i, "record");
        if (ak_equal(&stored,
                     ak_member(&stored, ak_member(&stored, record, "body"),
                               "descriptor_sha256"),
                     wanted))
            possible++;
    }
    if (!possible) {
        ak_json_free(&stored);
        return APKG_OK;
    }
    apkg_attestation **expanded = calloc(*count + possible, sizeof *expanded);
    if (!expanded) {
        ak_json_free(&stored);
        return APKG_NOMEM;
    }
    if (*count)
        memcpy(expanded, *claims, *count * sizeof *expanded);
    free(*claims);
    *claims = expanded;
    apkg_status code = APKG_OK;
    for (size_t i = 1; i < stored.tokens[0].next; i = stored.tokens[i].next) {
        int token = ak_member(&stored, (int)i, "record"),
            b = ak_member(&stored, token, "body");
        if (!ak_equal(&stored, ak_member(&stored, b, "descriptor_sha256"),
                      wanted))
            continue;
        apkg_bytes record = bytes(&stored, token),
                   envelope =
                       bytes(&stored, ak_member(&stored, (int)i, "signature"));
        apkg_digest hash;
        ak_hash(record.data, record.size, &hash);
        size_t n = 0;
        while (n < *count &&
               memcmp(apkg_attestation_get((*claims)[n])->record_digest.bytes,
                      hash.bytes, 32))
            n++;
        if (n < *count) {
            const apkg_attestation_view *v = apkg_attestation_get((*claims)[n]);
            if (v->signature_envelope.size != envelope.size ||
                memcmp(v->signature_envelope.data, envelope.data,
                       envelope.size)) {
                code = APKG_CONFLICT;
                break;
            }
            ak_attestation_memory((*claims)[n], 3, 0, NULL);
        } else {
            apkg_attestation *a = NULL;
            code = apkg_attestation_read(snapshot, descriptor, signature,
                                         record, envelope, NULL, &a, NULL);
            if (code)
                break;
            ak_attestation_memory(a, 2, 0, NULL);
            (*claims)[(*count)++] = a;
        }
    }
    ak_json_free(&stored);
    return code;
}
