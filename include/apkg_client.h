/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#ifndef APKG_CLIENT_H
#define APKG_CLIENT_H
#include "apkg.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Shared CLI/GUI presentation: all destinations, bytes, modes and hashes.
 * Caller owns *text and releases it with apkg_client_text_free. */
apkg_status apkg_plan_format(const apkg_plan *, char **text);
void apkg_client_text_free(char *);
typedef int (*apkg_confirm_fn)(void *, const apkg_plan *,
                               const char *presentation);
/* Local signed folder (descriptor.json/signature.json) or PKG/1 archive.
 * Explicit archive imports may populate a sibling verified-object store.
 * No shell, no network and no implicit initialization/trust expansion. */
apkg_status apkg_client_install(const char *root, const char *package_dir,
                                apkg_confirm_fn, void *, const apkg_observer *,
                                apkg_result **, apkg_error **);
typedef enum {
    APKG_CLIENT_INSTALL = 0,
    APKG_CLIENT_REMOVE,
    APKG_CLIENT_REPAIR,
    APKG_CLIENT_UPGRADE,
    APKG_CLIENT_ROLLBACK,
    APKG_CLIENT_RESOLVE,
    APKG_CLIENT_RECOVER,
    APKG_CLIENT_CHANNEL
} apkg_client_action;
apkg_status apkg_client_apply(apkg_client_action, const char *root,
                              const char *argument, const char *pool,
                              apkg_confirm_fn, void *, const apkg_observer *,
                              apkg_result **, apkg_error **);
/* Endpoint-bound discovery and exact selection shared by CLI/PkgManager.
 * selection is CHANNEL-ID/SNAPSHOT-SHA256/DESCRIPTOR-SHA256, with lowercase hex identities.
 * Apply prepares selected archives, then approves one immutable package/floor
 * plan. No implicit initialization, trust expansion or post-confirm resolution. */
apkg_status apkg_client_channel_apply(const char *root, const char *endpoint,
    const char *selection, int recommendations, apkg_confirm_fn, void *,
    const apkg_observer *, apkg_result **, apkg_error **);
apkg_status apkg_client_channel_query(const char *root, const char *endpoint,
    const char *identity, uint64_t minimum_sequence, const apkg_observer *,
    char **, apkg_error **);
/* Typed read-only discovery for interactive clients. The returned channel
 * owns every view, independently of the temporary read context. Release with
 * apkg_channel_free. Browse with apkg_channel_get/entry_at; no archive transfer
 * or root mutation. Signature/trust/target facts remain separate.
 * Selection retains the observed snapshot identity: apply refuses a changed
 * head rather than silently choosing a replacement. Caller owns *selection
 * and releases it with apkg_client_text_free. */
apkg_status apkg_client_channel_open(const char *root, const char *endpoint,
    const char *identity, uint64_t minimum_sequence, const apkg_observer *,
    apkg_channel **, apkg_error **);
apkg_status apkg_client_channel_selection(const apkg_channel *, uint64_t index,
    char **selection, apkg_error **);
/* Consistent captured root/channel pair; both outputs are independently owned.
 * Read-only, cancellable discovery; no archives or implicit acceptance. */
apkg_status apkg_client_channel_refresh(const char *root, const char *endpoint,
    const char *identity, uint64_t minimum_sequence, const apkg_observer *,
    apkg_snapshot **, apkg_channel **, apkg_error **);
apkg_status apkg_client_channel_updates(const char *root, const char *endpoint,
    const char *identity, uint64_t minimum_sequence, const apkg_observer *,
    char **, apkg_error **);
apkg_status apkg_client_folder(const char *, const char *, char **,
                               apkg_error **);
apkg_status apkg_client_run(const char *, const char *, apkg_error **);
apkg_status apkg_client_query(const char *root, const char *match, char **,
                              apkg_error **);
/* Exact installed name, read-only inspection. NULL/empty field emits all
 * fields as name/value lines; a field emits its value only. Unperformed
 * checks remain unknown/not-checked. Free text with apkg_client_text_free. */
const char *apkg_info_fields(void);
apkg_status apkg_inspection_format(const apkg_inspection *, const char *field,
    char **, apkg_error **);
apkg_status apkg_client_info(const char *root, const char *name,
    const char *field, char **, apkg_error **);
/* Bundle inspection/launch requires no registration and mutates no root. */
apkg_status apkg_bundle_check(const char *folder, const char *target, char **,
                              apkg_error **);
apkg_status apkg_bundle_run(const char *folder, const char *target,
                            apkg_error **);
#ifdef __cplusplus
}
#endif
#endif
