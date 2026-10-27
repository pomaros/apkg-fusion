/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include "pkg_fs.h"
#include <errno.h>
#include <string.h>
#define CONFIG_MAX (16u * 1024u * 1024u)
static const unsigned char magic[] = "APKG-SOURCE\0\1";
#define HEADER (sizeof magic - 1 + 16 + 12)
struct apkg_source_config {
    apkg_source_config_view view;
    unsigned char *bytes;
    char endpoint[4096];
};
static uint32_t get32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void put32(unsigned char *p, size_t n) {
    for (int i = 3; i >= 0; i--) {
        p[i] = (unsigned char)n;
        n >>= 8;
    }
}
static int endpoint_valid(const char *s, size_t n) {
    if (!n || n >= 4096)
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 32 || s[i] == 127)
            return 0;
    return 1;
}
static apkg_status io_status(void) {
    return errno == ENOENT   ? APKG_NOT_FOUND
           : errno == ENOMEM ? APKG_NOMEM
           : errno == EFBIG  ? APKG_LIMIT
                             : APKG_IO;
}
void apkg_source_config_free(apkg_source_config *c) {
    if (c) {
        free(c->bytes);
        free(c);
    }
}
const apkg_source_config_view *
apkg_source_config_get(const apkg_source_config *c) {
    return c ? &c->view : NULL;
}
apkg_status apkg_source_config_read(const char *path, apkg_source_config **out,
                                    apkg_error **error) {
    if (error)
        *error = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    if (!path || !*path || strlen(path) >= AK_PATH)
        return APKG_INVALID;
    apkg_source_config *c = calloc(1, sizeof *c);
    if (!c)
        return APKG_NOMEM;
    size_t size = 0;
    apkg_status code = APKG_INVALID;
    if (ak_read(path, &c->bytes, &size, CONFIG_MAX)) {
        code = io_status();
        goto done;
    }
    if (size < HEADER || memcmp(c->bytes, magic, sizeof magic - 1))
        goto done;
    const unsigned char *lengths = c->bytes + sizeof magic - 1 + 16;
    size_t endpoint = get32(lengths), object = get32(lengths + 4),
           signature = get32(lengths + 8);
    size_t remaining = size - HEADER;
    if (endpoint > remaining ||
        !endpoint_valid((const char *)c->bytes + HEADER, endpoint))
        goto done;
    remaining -= endpoint;
    if (object > remaining)
        goto done;
    remaining -= object;
    if (signature != remaining || ((object == 0) != (signature == 0)))
        goto done;
    memcpy(c->view.id.bytes, c->bytes + sizeof magic - 1, 16);
    memcpy(c->endpoint, c->bytes + HEADER, endpoint);
    c->view.endpoint = (apkg_text){c->endpoint, endpoint};
    c->view.observation = (apkg_bytes){c->bytes + HEADER + endpoint, object};
    c->view.signature =
        (apkg_bytes){c->bytes + HEADER + endpoint + object, signature};
    *out = c;
    c = NULL;
    code = APKG_OK;
done:
    apkg_source_config_free(c);
    return code ? ak_fail(error, code, "source profile refused", path)
                : APKG_OK;
}
apkg_status apkg_source_config_save(const char *path, const char *endpoint,
                                    const apkg_id *identity,
                                    const apkg_channel *channel,
                                    apkg_error **error) {
    if (error)
        *error = NULL;
    if (!path || !*path || strlen(path) >= AK_PATH || !endpoint || !identity ||
        !endpoint_valid(endpoint, strlen(endpoint)))
        return APKG_INVALID;
    if (!pkg_fs_atomic_replace_supported())
        return APKG_UNSUPPORTED;
    apkg_bytes object = {0}, signature = {0};
    apkg_status code = APKG_OK;
    if (channel) {
        if (memcmp(identity->bytes, apkg_channel_get(channel)->id.bytes, 16))
            return APKG_CONFLICT;
        code = apkg_channel_observation(channel, &object, &signature);
        if (code)
            return code;
    }
    size_t n = strlen(endpoint), total = HEADER + n;
    if (object.size > CONFIG_MAX - total ||
        signature.size > CONFIG_MAX - total - object.size)
        return APKG_LIMIT;
    total += object.size + signature.size;
    unsigned char *bytes = malloc(total);
    if (!bytes)
        return APKG_NOMEM;
    memcpy(bytes, magic, sizeof magic - 1);
    memcpy(bytes + sizeof magic - 1, identity->bytes, 16);
    unsigned char *lengths = bytes + sizeof magic - 1 + 16;
    put32(lengths, n);
    put32(lengths + 4, object.size);
    put32(lengths + 8, signature.size);
    memcpy(bytes + HEADER, endpoint, n);
    if (object.size)
        memcpy(bytes + HEADER + n, object.data, object.size);
    if (signature.size)
        memcpy(bytes + HEADER + n + object.size, signature.data,
               signature.size);
    /* Explicit settings writes use the platform's private atomic-file service.
     * No root, policy or replay floor is changed. Last successful save wins. */
    if (pkg_fs_path_is_link(path) != 0)
        code = APKG_IO;
    else if (pkg_fs_write_private(path, bytes, total))
        code = io_status();
    free(bytes);
    return code ? ak_fail(error, code, "source profile save failed", path)
                : APKG_OK;
}
apkg_status apkg_source_config_open(const apkg_source_config *config,
                                    const char *root,
                                    apkg_snapshot **snapshot_out,
                                    apkg_channel **channel_out,
                                    apkg_error **error) {
    if (error)
        *error = NULL;
    if (snapshot_out)
        *snapshot_out = NULL;
    if (channel_out)
        *channel_out = NULL;
    if (!config || !root || !snapshot_out || !channel_out ||
        (void *)snapshot_out == (void *)channel_out)
        return APKG_INVALID;
    if (!config->view.observation.size)
        return APKG_NOT_FOUND;
    apkg_open_options options = {0};
    options.contract_version = APKG_CONTRACT_VERSION;
    options.struct_size = sizeof options;
    options.root = (apkg_text){root, strlen(root)};
    apkg_ctx *ctx = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_channel *channel = NULL;
    apkg_status code = apkg_open(&options, &ctx, error);
    if (!code)
        code = apkg_channel_read(ctx, &config->view.id, 0,
                                 config->view.observation,
                                 config->view.signature, &channel, error);
    if (!code)
        code = apkg_snapshot_read(ctx, &snapshot, error);
    if (!code)
        code = ak_channel_snapshot_capture(snapshot, channel);
    if (!code) {
        *snapshot_out = snapshot;
        snapshot = NULL;
        *channel_out = channel;
        channel = NULL;
    }
    apkg_snapshot_free(snapshot);
    apkg_channel_free(channel);
    apkg_close(ctx);
    return code;
}
