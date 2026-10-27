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
    APKG_CLIENT_CHANNEL,
    APKG_CLIENT_CHANNEL_ACCEPT,
    APKG_CLIENT_ADOPT,
    APKG_CLIENT_ADOPT_SET,
    APKG_CLIENT_POLICY, /* argument: a complete policy object file */
    APKG_CLIENT_ROLLBACK_LAST /* argument: package name, or "-" for any */
} apkg_client_action;
/* ADOPT: argument=artifact, pool=evidence file. ADOPT_SET: argument=canonical
 * adoption-set manifest. Both use the same exact confirmation and journal. */
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
apkg_status apkg_client_channel_accept(const char *root,const char *endpoint,
    const char *identity,apkg_confirm_fn,void *,const apkg_observer *,apkg_result **,apkg_error **);
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
/* Read-only live system observations using the install probes. Missing
 * requirements are report facts, not a query error. No plan/archive/root writes.
 * The package form uses its already validated immutable handle. The channel
 * form selects the newest trusted current-target build by exact name; authors,
 * incomparable versions and differing equal builds refuse as ambiguous.
 * Returned text is owned by the caller (apkg_client_text_free). */
apkg_status apkg_package_requirements(const apkg_package *, char **, apkg_error **);
apkg_status apkg_channel_requirements(apkg_ctx *, const apkg_channel *,
    const char *name, char **, apkg_error **);
apkg_status apkg_client_requirements(const char *root, const char *endpoint,
    const char *identity, const char *name, char **, apkg_error **);
/* Explicit per-source profile, separate from root trust/installed state.
 * Views borrow their config owner. Reading settings validates framing only;
 * open revalidates the copied signed observation against the current root.
 * Outputs own independent handles. Offline channels convey no transfer origin.
 * save(NULL channel) saves settings only and clears any previous observation.
 * Profiles are private atomic-file replacements; concurrent saves are last-wins.
 * No automatic locations, network fallback or trust grants. */
typedef struct apkg_source_config apkg_source_config;
/* Decode one descriptive field from a presentation object, without granting
 * signature/trust/review facts. NULL/zero-size or an absent field is NOT_FOUND;
 * a recorded empty string is OK. Output is owned; free with client_text_free.
 * Known fields: summary, category, license, repository. No I/O. */
apkg_status apkg_presentation_field(apkg_bytes, const char *field, char **,
                                   apkg_error **);
typedef struct {
    apkg_text endpoint;
    apkg_id id;
    apkg_bytes observation, signature;
} apkg_source_config_view;
apkg_status apkg_source_config_save(const char *path, const char *endpoint,
    const apkg_id *, const apkg_channel *, apkg_error **);
apkg_status apkg_source_config_read(const char *path, apkg_source_config **,
    apkg_error **);
const apkg_source_config_view *apkg_source_config_get(const apkg_source_config *);
void apkg_source_config_free(apkg_source_config *);
apkg_status apkg_source_config_open(const apkg_source_config *, const char *root,
    apkg_snapshot **, apkg_channel **, apkg_error **);
const char *apkg_outdated_fields(void);
/* Accepts a library view or its unchanged owned deep copy. Unknown readiness
 * and freshness remain explicit. Ambiguous unqualified package lists refuse.
 * Text is owned; release with apkg_client_text_free. */
apkg_status apkg_update_summary_format(const apkg_update_summary_view *,
    const char *field, char **, apkg_error **);
apkg_status apkg_client_outdated(const char *root, const char *endpoint,
    const char *identity, const char *field, const apkg_observer *, char **,
    apkg_error **);
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
apkg_status apkg_channel_attestations_format(const apkg_channel *, uint64_t entry,
    char **, apkg_error **);
apkg_status apkg_attestation_format(const apkg_attestation *, char **, apkg_error **);
/* Read-only folder: descriptor.json, signature.json, attestation.json and
 * attestation.signature.json. No network/evidence fetch or implicit policy. */
apkg_status apkg_client_attestation(const char *root, const char *folder,
    char **, apkg_error **);
/* Self-update of the client from a channel, for a client installed as a
 * managed package of `root`. `executable` is the running client's path; it
 * must be a file of an installed package with its recorded bytes. The newest
 * trusted candidate of the same authority/name/target that carries the file at
 * the same path is applied through apkg_client_channel_apply, so the shared
 * plan, confirmation, journal and recovery apply. check_only reports without
 * planning. *report (owned, apkg_client_text_free) says what was found and that
 * a restart is required. Recorded copies and an unsettled legacy arospkg
 * self-update refuse. */
apkg_status apkg_client_self_update(const char *root, const char *endpoint,
    const char *identity, const char *executable, int check_only,
    apkg_confirm_fn, void *, const apkg_observer *, char **report,
    apkg_result **, apkg_error **);
/* The root's ordered source list. add opens and verifies the channel under the
 * current policy first; neither add nor remove grants or withdraws a key.
 * install reads every listed source in order and installs or upgrades `name`:
 * the newest trusted candidate wins, an equal version goes to the earlier
 * source, dependencies are pooled across the list, and an unreadable source,
 * two authors for one name or incomparable candidates refuse. A current
 * package yields an UNCHANGED result. list text: ORDER<TAB>NAME<TAB>ID<TAB>
 * ENDPOINT lines (apkg_client_text_free). */
apkg_status apkg_client_source_add(const char *root, const char *name,
    const char *endpoint, const char *identity, apkg_confirm_fn, void *,
    const apkg_observer *, apkg_result **, apkg_error **);
apkg_status apkg_client_source_remove(const char *root, const char *name,
    apkg_confirm_fn, void *, const apkg_observer *, apkg_result **,
    apkg_error **);
apkg_status apkg_client_source_list(const char *root, char **text,
                                    apkg_error **);
apkg_status apkg_client_source_install(const char *root, const char *name,
    int recommendations, apkg_confirm_fn, void *, const apkg_observer *,
    apkg_result **, apkg_error **);
/* Hash current declared paths against the installed signed descriptor.
 * Success means the diagnostic report completed; changed/missing/unknown are
 * report facts, never an implicit repair. Signature invalidity, unsupported
 * live nodes, interrupted roots and overflow refuse without partial text.
 * Read-only, streamed hashes; cancellation before/after inventory and between
 * comparisons. Return text with apkg_client_text_free. */
apkg_status apkg_client_verify(const char *root, const char *name,
    const apkg_observer *, char **report, apkg_error **);
/* Bundle inspection/launch requires no registration and mutates no root. */
apkg_status apkg_bundle_check(const char *folder, const char *target, char **,
                              apkg_error **);
apkg_status apkg_bundle_run(const char *folder, const char *target,
                            apkg_error **);
#ifdef __cplusplus
}
#endif
#endif
