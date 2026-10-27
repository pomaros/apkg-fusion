/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include <string.h>

static const char *const fields[] = {"summary", "category", "license", "repository"};
static const size_t bounds[] = {4096, 255, 4096, 4096};

int ak_presentation_valid(const ak_json *json, int object) {
    if (object < 0)
        return 1;
    if (!ak_fields_optional(json, object, NULL, 0, fields, 4))
        return 0;
    char text[4097];
    for (size_t i = 0; i < 4; i++) {
        int token = ak_member(json, object, fields[i]);
        if (token < 0)
            continue;
        if (ak_string(json, token, text, bounds[i] + 1))
            return 0;
        for (const unsigned char *p = (const unsigned char *)text; *p; p++)
            if (*p < 32 || *p == 127)
                return 0;
    }
    return 1;
}

apkg_status apkg_presentation_field(apkg_bytes record, const char *field,
                                    char **out, apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!field)
        return APKG_INVALID;
    size_t selected = 0;
    while (selected < 4 && strcmp(field, fields[selected]))
        selected++;
    if (selected == 4)
        return APKG_INVALID;
    if (!record.size)
        return APKG_NOT_FOUND;
    if (!record.data)
        return APKG_INVALID;
    if (record.size > 32768)
        return APKG_LIMIT;
    ak_json json = {0};
    int parsed = ak_json_parse(record.data, record.size, &json);
    if (parsed)
        return parsed == -2 ? APKG_NOMEM : APKG_INVALID;
    apkg_status code = APKG_INVALID;
    if (!ak_presentation_valid(&json, 0))
        goto done;
    int token = ak_member(&json, 0, field);
    if (token < 0) {
        code = APKG_NOT_FOUND;
        goto done;
    }
    char text[4097];
    if (ak_string(&json, token, text, bounds[selected] + 1))
        goto done;
    *out = ak_copy(text, strlen(text));
    code = *out ? APKG_OK : APKG_NOMEM;
done:
    ak_json_free(&json);
    return code;
}
