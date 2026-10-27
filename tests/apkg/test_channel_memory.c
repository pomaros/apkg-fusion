/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
/* Exercise owned source/plan lifetimes in an instrumented native process. */
static int prepared_plan(char **argv) {
    apkg_open_options options = {0};
    apkg_ctx *ctx = NULL;
    apkg_channel *channel = NULL;
    apkg_package *package = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_plan *plan = NULL;
    apkg_approval *approval = NULL;
    apkg_result *result = NULL;
    apkg_id id;
    assert(!ak_hex(argv[3], id.bytes, 16));
    options.contract_version = 1;
    options.struct_size = sizeof options;
    options.mode = APKG_WRITE;
    options.root.data = argv[1];
    options.root.size = strlen(argv[1]);
    assert(apkg_open(&options, &ctx, NULL) == APKG_OK);
    apkg_text source = {argv[2], strlen(argv[2])};
    apkg_status code =
        ak_http_url(argv[2])
            ? apkg_channel_open_http(ctx, source, &id, 1, NULL, &channel, NULL)
            : apkg_channel_open_file(ctx, source, &id, 1, &channel, NULL);
    assert(code == APKG_OK);
    assert(apkg_channel_fetch(ctx, channel, 0, NULL, &package, NULL) ==
           APKG_OK);
    assert(apkg_snapshot_read(ctx, &snapshot, NULL) == APKG_OK);
    assert(apkg_plan_install(ctx, snapshot, package, NULL, &plan, NULL) ==
           APKG_OK);
    apkg_package_free(package);
    apkg_channel_free(channel);
    apkg_snapshot_free(snapshot);
    const apkg_plan_view *view = apkg_plan_get(plan);
    assert(apkg_plan_approve(ctx, plan, &view->plan_digest, &approval, NULL) ==
           APKG_OK);
    assert(apkg_execute(ctx, plan, approval, NULL, &result, NULL) == APKG_OK);
    assert(apkg_result_get(result)->outcome == APKG_COMMITTED);
    apkg_result_free(result);
    apkg_approval_free(approval);
    apkg_plan_free(plan);
    apkg_close(ctx);
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[4], "--browse")) {
        apkg_channel *channel = NULL;
        assert(apkg_client_channel_open(argv[1], argv[2], argv[3], 0, NULL,
                                        &channel, NULL) == APKG_OK);
        const apkg_channel_view *view = apkg_channel_get(channel);
        for (uint64_t i = 0; i < view->entry_count; i++) {
            const apkg_channel_entry_view *row = NULL;
            char *selection = NULL;
            assert(apkg_channel_entry_at(channel, i, &row, NULL) == APKG_OK);
            assert(row->name.size && row->version.size);
            assert(apkg_client_channel_selection(channel, i, &selection,
                                                 NULL) == APKG_OK);
            assert(strlen(selection) == 162);
            assert(!memcmp(selection, argv[3], 32));
            apkg_client_text_free(selection);
        }
        apkg_channel_free(channel);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[4], "--plan"))
        return prepared_plan(argv);
    apkg_open_options options = {0};
    apkg_ctx *ctx = NULL;
    apkg_channel *channel = NULL;
    apkg_package *package = NULL;
    apkg_id id;
    unsigned char *document = NULL, *signature = NULL;
    size_t dn = 0, sn = 0;
    assert(argc == 4 || argc == 5);
    assert(!ak_hex(argv[argc == 4 ? 3 : 4], id.bytes, 16));
    if (argc == 5) {
        assert(!ak_read(argv[2], &document, &dn, AK_MAX / 8));
        assert(!ak_read(argv[3], &signature, &sn, 16384));
    }
    options.contract_version = 1;
    options.struct_size = sizeof options;
    options.root.data = argv[1];
    options.root.size = strlen(argv[1]);
    options.mode = APKG_READ;
    assert(apkg_open(&options, &ctx, NULL) == APKG_OK);
    apkg_bytes d = {document, dn}, s = {signature, sn};
    if (argc == 4) {
        apkg_text endpoint = {argv[2], strlen(argv[2])};
        assert(apkg_channel_open_file(ctx, endpoint, &id, 1, &channel, NULL) ==
               APKG_OK);
    } else
        assert(apkg_channel_read(ctx, &id, 1, d, s, &channel, NULL) == APKG_OK);
    free(document);
    free(signature);
    const apkg_channel_entry_view *row = NULL;
    assert(apkg_channel_entry_at(channel, 0, &row, NULL) == APKG_OK);
    apkg_digest expected = row->descriptor_digest;
    assert(apkg_channel_package(ctx, channel, 0, &package, NULL) == APKG_OK);
    apkg_snapshot *captured = NULL;
    assert(apkg_snapshot_read(ctx, &captured, NULL) == APKG_OK);
    apkg_close(ctx);
    apkg_news *news = NULL;
    assert(apkg_channel_news(captured, channel, (apkg_bytes){0},
        (apkg_bytes){0}, &news, NULL) == APKG_OK);
    apkg_snapshot_free(captured);
    char *news_text = NULL;
    assert(apkg_news_format(news, 1, &news_text, NULL) == APKG_OK);
    assert(strstr(news_text, "\"has_previous\":false"));
    apkg_client_text_free(news_text);
    apkg_news_free(news);
    apkg_recent *recent = NULL;
    char *recent_text = NULL;
    assert(apkg_channel_recent(channel, NULL, &recent, NULL) == APKG_OK);
    assert(apkg_recent_format(recent, 1, &recent_text, NULL) == APKG_OK);
    assert(strstr(recent_text, "\"date_source\":\"channel-publisher\""));
    apkg_client_text_free(recent_text);
    apkg_recent_free(recent);
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
