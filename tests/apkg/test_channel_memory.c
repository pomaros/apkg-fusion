/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    apkg_open_options options = {0};
    apkg_ctx *ctx = NULL;
    apkg_channel *channel = NULL;
    apkg_package *package = NULL;
    apkg_id id;
    unsigned char *document = NULL, *signature = NULL;
    size_t dn, sn;
    assert(argc == 5);
    assert(!ak_hex(argv[4], id.bytes, 16));
    assert(!ak_read(argv[2], &document, &dn, AK_MAX / 8));
    assert(!ak_read(argv[3], &signature, &sn, 16384));
    options.contract_version = 1;
    options.struct_size = sizeof options;
    options.root.data = argv[1];
    options.root.size = strlen(argv[1]);
    options.mode = APKG_READ;
    assert(apkg_open(&options, &ctx, NULL) == APKG_OK);
    apkg_bytes d = {document, dn}, s = {signature, sn};
    assert(apkg_channel_read(ctx, &id, 1, d, s, &channel, NULL) == APKG_OK);
    free(document);
    free(signature);
    const apkg_channel_entry_view *row = NULL;
    assert(apkg_channel_entry_at(channel, 0, &row, NULL) == APKG_OK);
    apkg_digest expected = row->descriptor_digest;
    assert(apkg_channel_package(ctx, channel, 0, &package, NULL) == APKG_OK);
    apkg_close(ctx);
    assert(row->name.size == 4 && !memcmp(row->name.data, "demo", 4));
    assert(row->descriptor.size > 0);
    const apkg_channel_entry_view *missing = row;
    assert(apkg_channel_entry_at(channel, UINT64_MAX, &missing, NULL) ==
               APKG_NOT_FOUND &&
           !missing);
    apkg_channel_free(channel);
    apkg_channel_free(NULL);
    apkg_digest actual;
    apkg_bytes copied = apkg_package_descriptor(package);
    ak_hash(copied.data, copied.size, &actual);
    assert(!memcmp(expected.bytes, actual.bytes, 32));
    apkg_package_free(package);
    return 0;
}
