/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Common context engine. Initial operation subset is documented in api.md. */
#ifndef APKG_H
#define APKG_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define APKG_CONTRACT_VERSION 1u

typedef struct apkg_ctx apkg_ctx;
typedef struct apkg_snapshot apkg_snapshot;
typedef struct apkg_package apkg_package;
typedef struct apkg_channel apkg_channel;
typedef struct apkg_plan apkg_plan;
typedef struct apkg_approval apkg_approval;
typedef struct apkg_result apkg_result;
typedef struct apkg_error apkg_error;
typedef struct apkg_cancel apkg_cancel;
typedef struct apkg_event_copy apkg_event_copy;
typedef struct apkg_inspection apkg_inspection;
typedef struct apkg_update_summary apkg_update_summary;
typedef struct apkg_observer apkg_observer;
/* Length excludes terminator. Text is UTF-8, no embedded NUL; bytes are opaque.
 * A zero length permits NULL. All returned views are borrowed, never free(). */
typedef struct {
    const char *data;
    size_t size;
} apkg_text;
typedef struct {
    const unsigned char *data;
    size_t size;
} apkg_bytes;
typedef struct {
    unsigned char bytes[16];
} apkg_id;
typedef struct {
    unsigned char bytes[32];
} apkg_digest;
typedef enum {
    APKG_OK = 0,
    APKG_INVALID,
    APKG_UNSUPPORTED,
    APKG_LIMIT,
    APKG_NOMEM,
    APKG_NOT_FOUND,
    APKG_IO,
    APKG_BUSY,
    APKG_STALE,
    APKG_RECOVERY_REQUIRED,
    APKG_INTEGRITY,
    APKG_SIGNATURE,
    APKG_TRUST,
    APKG_TARGET,
    APKG_CONFLICT,
    APKG_DEPENDENCY,
    APKG_CANCELLED,
    APKG_POLICY,
    APKG_NETWORK
} apkg_status;
typedef enum {
    APKG_UNKNOWN = 0,
    APKG_KNOWN,
    APKG_INVALID_FACT
} apkg_fact_state;
typedef struct {
    apkg_fact_state state;
    uint64_t value;
    apkg_text evidence;
} apkg_u64_fact;
typedef struct {
    apkg_fact_state state;
    apkg_text value, evidence;
} apkg_text_fact;
typedef struct {
    apkg_status code;
    apkg_text message, field, subject;
} apkg_error_view;
const apkg_error_view *apkg_error_get(const apkg_error *);
void apkg_error_free(apkg_error *);

typedef enum { APKG_READ = 0, APKG_WRITE } apkg_mode;
typedef struct {
    uint32_t contract_version; /* must be APKG_CONTRACT_VERSION */
    size_t struct_size;        /* must be sizeof(apkg_open_options) */
    apkg_text root; /* explicit host path, never an ambient default */
    apkg_mode mode;
    uint64_t max_object_bytes,
        max_memory_bytes; /* 0 selects documented defaults */
} apkg_open_options;
/* No initialization, locking, recovery, download or path creation at open. */
apkg_status apkg_open(const apkg_open_options *, apkg_ctx **, apkg_error **);
void apkg_close(apkg_ctx *);
/* One active synchronous call per ctx. Distinct contexts may run concurrently.
 */
typedef enum {
    APKG_ROOT_ABSENT = 0,
    APKG_ROOT_LEGACY,
    APKG_ROOT_READY,
    APKG_ROOT_PENDING,
    APKG_ROOT_INVALID
} apkg_root_state;
typedef struct {
    apkg_root_state state;
    apkg_fact_state identity_state;
    apkg_id root_id; /* meaningful only when identity_state == KNOWN */
    apkg_u64_fact generation, pending_count;
    apkg_digest snapshot_digest; /* requires KNOWN identity/generation in common
                                    state */
    apkg_text_fact target;
    apkg_text diagnostic; /* explains INVALID/unsupported root state */
    apkg_bytes
        source_record; /* raw canonical common bytes, or empty for legacy */
} apkg_root_view;
apkg_status apkg_snapshot_read(apkg_ctx *, apkg_snapshot **, apkg_error **);
const apkg_root_view *apkg_snapshot_root(const apkg_snapshot *);
void apkg_snapshot_free(apkg_snapshot *);

typedef enum {
    APKG_REASON_UNKNOWN = 0,
    APKG_REASON_EXPLICIT,
    APKG_REASON_DEPENDENCY,
    APKG_REASON_RECOMMENDATION,
    APKG_REASON_SET
} apkg_reason;
typedef enum {
    APKG_SIGNATURE_UNKNOWN = 0,
    APKG_SIGNATURE_VALID,
    APKG_SIGNATURE_INVALID
} apkg_signature_state;
typedef enum {
    APKG_TRUST_UNKNOWN = 0,
    APKG_TRUST_ACCEPTED,
    APKG_TRUST_REJECTED
} apkg_trust_state;
typedef struct {
    apkg_id instance_id;
    apkg_text_fact authority, name, version, target;
    apkg_u64_fact revision;
    apkg_reason reason;
    apkg_signature_state signature;
    apkg_trust_state trust;
    apkg_bytes
        record; /* full record/evidence; no lossy fixed-size UI projection */
    apkg_digest descriptor_digest; /* exact committed descriptor bytes */
} apkg_installed_view;
uint64_t apkg_snapshot_installed_count(const apkg_snapshot *);
apkg_status apkg_snapshot_installed_at(const apkg_snapshot *, uint64_t,
                                       const apkg_installed_view **,
                                       apkg_error **);
/* Owns its bytes independently of the snapshot/context. Checks descriptor
 * signature and author grant against captured snapshot policy, without
 * fetching, checking installed payload bytes or probing the running OS.
 * Root-target agreement is not runtime qualification. */
typedef struct {
    apkg_text authority, name, version, target, kind;
    uint64_t revision, payload_bytes, file_count, directory_count;
    apkg_root_state root_state;
    apkg_id root_id;
    apkg_digest snapshot_digest;
    uint64_t generation;
    apkg_reason reason;
    apkg_signature_state signature;
    apkg_trust_state author_trust;
    int target_matches;
    apkg_fact_state review, runtime_testing;
    apkg_bytes descriptor, signature_envelope, relations;
    apkg_bytes adoption; /* owned local evidence; original authentication unknown */
} apkg_inspection_view;
apkg_status apkg_snapshot_inspect(const apkg_snapshot *, uint64_t,
    apkg_inspection **, apkg_error **);
const apkg_inspection_view *apkg_inspection_get(const apkg_inspection *);
void apkg_inspection_free(apkg_inspection *);
/* Signed review/runtime claims, independently of author/channel signatures.
 * Exact descriptor binding; captured-policy facts only. No payload execution,
 * evidence retrieval, current revocation/freshness or install authorization.
 * Views own their bytes; previous may link an observed prefix, never a latest
 * head. Missing previous keeps history unchecked rather than inventing it. */
typedef struct apkg_attestation apkg_attestation;
typedef enum { APKG_ATTEST_REVIEW = 1, APKG_ATTEST_RUNTIME } apkg_attestation_kind;
typedef enum { APKG_ATTEST_PASSED = 1, APKG_ATTEST_FAILED, APKG_ATTEST_REVOKED } apkg_attestation_result;
typedef struct {
    apkg_attestation_kind kind;
    apkg_attestation_result result;
    apkg_id issuer;
    apkg_digest record_digest, descriptor_digest, snapshot_digest, supersedes;
    uint64_t generation, revision, sequence, issued_at;
    apkg_text authority, name, version, target, summary, evidence;
    apkg_bytes environment, record, signature_envelope;
    apkg_signature_state signature, descriptor_signature;
    apkg_trust_state issuer_trust, author_trust;
    int independent, target_matches, has_previous, prefix_complete;
    apkg_fact_state freshness;
    apkg_digest scope_digest, accepted_digest;
    uint64_t accepted_sequence;
    int observation_origin; /* 0 standalone, 1 source, 2 retained, 3 both */
    int older_than_accepted;
} apkg_attestation_view;
apkg_status apkg_attestation_read(const apkg_snapshot *, apkg_bytes descriptor,
    apkg_bytes descriptor_signature, apkg_bytes record, apkg_bytes signature,
    const apkg_attestation *previous, apkg_attestation **, apkg_error **);
const apkg_attestation_view *apkg_attestation_get(const apkg_attestation *);
void apkg_attestation_free(apkg_attestation *);
/* Source bytes/signatures are copied; no implicit fetching or trust changes. */
apkg_status apkg_package_read(apkg_ctx *, apkg_bytes descriptor,
                              apkg_bytes signature_envelope, apkg_package **,
                              apkg_error **);
/* Explicit write-mode import of an AROS PKG/1 envelope containing the signed
 * descriptor, signature and exact content-addressed blobs. Managed state is
 * unchanged; verified objects are retained in the context's sibling store. */
apkg_status apkg_package_import(apkg_ctx *, apkg_text archive,
                                const apkg_observer *, apkg_package **,
                                apkg_error **);
void apkg_package_free(apkg_package *);
/* Exact immutable descriptor reference for CLI/UI presentation. */
apkg_bytes apkg_package_descriptor(const apkg_package *);

/* Authenticated discovery is separate from package authorization. Reading a
 * channel creates no files and never grants author/reviewer privileges.
 * Views borrow immutable channel-owned bytes until apkg_channel_free(). */
typedef struct {
    apkg_id id;
    uint64_t sequence, entry_count;
    apkg_digest snapshot_digest;
    apkg_signature_state signature;
    apkg_trust_state trust;
    apkg_fact_state
        freshness; /* UNKNOWN: signature/floor is not online freshness */
    apkg_u64_fact accepted_sequence; /* root's durable floor; UNKNOWN before acceptance */
} apkg_channel_view;
typedef struct {
    apkg_text authority, name, version, target;
    uint64_t revision, archive_bytes;
    apkg_digest descriptor_digest, archive_digest;
    apkg_signature_state signature;
    apkg_trust_state author_trust;
    int target_matches;
    apkg_bytes descriptor, signature_envelope;
    uint64_t attestation_count; /* supplied plus retained observations */
    uint64_t source_attestation_count;
} apkg_channel_entry_view;
apkg_status apkg_channel_read(apkg_ctx *, const apkg_id *expected_id,
                              uint64_t minimum_sequence, apkg_bytes object,
                              apkg_bytes signature, apkg_channel **,
                              apkg_error **);
/* Open the atomic current reference of an immutable local channel. No files
 * are created. Selected lookup refuses a changed reference after opening. */
apkg_status apkg_channel_open_file(apkg_ctx *, apkg_text directory,
                                   const apkg_id *, uint64_t minimum_sequence,
                                   apkg_channel **, apkg_error **);
/* HTTP metadata stays in bounded memory, including in READ contexts. No
 * implicit cache, root creation, key grants or recovery. */
apkg_status apkg_channel_open_http(apkg_ctx *, apkg_text endpoint,
                                   const apkg_id *, uint64_t minimum_sequence,
                                   const apkg_observer *, apkg_channel **,
                                   apkg_error **);
/* Explicit WRITE preparation: retain the exact selected archive and verify its
 * descriptor/signature/payload. No managed generation is committed. */
apkg_status apkg_channel_fetch(apkg_ctx *, const apkg_channel *, uint64_t,
                               const apkg_observer *, apkg_package **,
                               apkg_error **);
/* Exact signed observation, borrowed until channel_free. Copy both byte spans
 * to retain offline metadata. Re-read with channel_read against the current
 * context before use: this conveys neither current trust nor online freshness,
 * and does not retain a transfer origin or authorize an installation. */
apkg_status apkg_channel_observation(const apkg_channel *, apkg_bytes *object,
                                     apkg_bytes *signature);
const apkg_channel_view *apkg_channel_get(const apkg_channel *);
apkg_status apkg_channel_entry_at(const apkg_channel *, uint64_t,
                                  const apkg_channel_entry_view **,
                                  apkg_error **);
/* Supplied claims in wire order, then retained observations in digest order.
 * Views are immutable and borrow the channel owner. */
apkg_status apkg_channel_attestation_at(const apkg_channel *, uint64_t entry,
    uint64_t observation, const apkg_attestation_view **, apkg_error **);
/* Recheck the bound root policy/target and decode a selected descriptor. This
 * is metadata only: archive transfer and blob availability are separate.
 * File/HTTP selections retain owned origins which bind subsequent plans. */
apkg_status apkg_channel_package(apkg_ctx *, const apkg_channel *, uint64_t,
                                 apkg_package **, apkg_error **);
void apkg_channel_free(apkg_channel *);
/* Pure comparison of captured metadata. Views borrow the supplied snapshot;
 * no transfer, live-file verification, dependency resolution or authorization.
 * Changed policy/target/root binding or an obsolete replay floor refuses. */
typedef enum {
    APKG_CANDIDATE_INDETERMINATE = 0,
    APKG_CANDIDATE_NOT_INSTALLED,
    APKG_CANDIDATE_CURRENT,
    APKG_CANDIDATE_NEWER,
    APKG_CANDIDATE_OLDER,
    APKG_CANDIDATE_CONTENT_CONFLICT,
    APKG_CANDIDATE_OTHER_AUTHORITY,
    APKG_CANDIDATE_OTHER_TARGET
} apkg_candidate_state;
typedef struct {
    apkg_candidate_state state;
    const apkg_installed_view *installed;
    apkg_fact_state plan_readiness;
} apkg_candidate_view;
apkg_status apkg_channel_compare(const apkg_snapshot *, const apkg_channel *,
    uint64_t index, apkg_candidate_view *, apkg_error **);
const char *apkg_candidate_state_name(apkg_candidate_state);
/* Owned aggregation by qualified installed identity, with every candidate
 * retained by the separate comparison API. Counts do not assert plan/runtime
 * readiness. Views borrow summary-owned bytes; native clients may deep-copy
 * the complete view for presentation across their worker boundary. */
typedef struct {
    apkg_text authority, name, target, installed_version;
    uint64_t installed_revision, newer_candidates, eligible_candidates;
} apkg_update_entry_view;
typedef struct {
    uint64_t installed, newer, metadata_eligible, blocked, newer_candidates;
    uint64_t blocked_candidates, indeterminate_candidates, conflict_candidates, not_listed;
    uint64_t generation, sequence, entry_count;
    apkg_digest root_snapshot, channel_snapshot;
    apkg_id channel_id;
    apkg_fact_state plan_readiness, freshness;
    int names_ambiguous;
    const apkg_update_entry_view *entries;
} apkg_update_summary_view;
apkg_status apkg_channel_summarize(const apkg_snapshot *, const apkg_channel *,
    apkg_update_summary **, apkg_error **);
const apkg_update_summary_view *apkg_update_summary_get(const apkg_update_summary *);
void apkg_update_summary_free(apkg_update_summary *);

typedef enum {
    APKG_PHASE_CHECK = 0,
    APKG_PHASE_FETCH,
    APKG_PHASE_STAGE,
    APKG_PHASE_APPLY,
    APKG_PHASE_COMMIT,
    APKG_PHASE_RECOVER
} apkg_phase;
typedef enum { APKG_EVENT_PROGRESS = 0, APKG_EVENT_DIAGNOSTIC } apkg_event_kind;
typedef struct {
    apkg_id operation_id;
    uint64_t sequence;
    apkg_event_kind kind;
    apkg_phase phase;
    apkg_text subject, message;
    uint64_t completed;
    apkg_u64_fact total; /* unknown total never means zero or 100% */
    apkg_text unit;      /* bytes, files, packages; no mixed counters */
    int cancellable;     /* cancellation accepted at this boundary */
} apkg_event;
typedef enum { APKG_CONTINUE = 0, APKG_REQUEST_CANCEL } apkg_event_action;
typedef apkg_event_action (*apkg_event_fn)(void *, const apkg_event *);
struct apkg_observer {
    apkg_event_fn event;
    void *user;
    apkg_cancel *cancel;
};
apkg_status apkg_cancel_create(apkg_cancel **);
void apkg_cancel_request(apkg_cancel *); /* only cross-thread mutation API */
void apkg_cancel_free(apkg_cancel *);
apkg_status apkg_event_clone(const apkg_event *, apkg_event_copy **);
const apkg_event *apkg_event_get(const apkg_event_copy *);
void apkg_event_free(apkg_event_copy *);

typedef enum {
    APKG_PLAN_INITIALIZE = 0,
    APKG_PLAN_INSTALL,
    APKG_PLAN_REMOVE,
    APKG_PLAN_REPAIR,
    APKG_PLAN_UPGRADE,
    APKG_PLAN_ROLLBACK,
    APKG_PLAN_RESOLVE,
    APKG_PLAN_CHANNEL_ACCEPT,
    APKG_PLAN_ADOPT
} apkg_plan_kind;
typedef struct {
    apkg_plan_kind kind;
    apkg_digest plan_digest;
    apkg_id root_id;
    uint64_t generation, effect_count;
    apkg_bytes canonical; /* exact approved bytes, not presentation text */
    int executable;       /* 0: preview with blockers; cannot approve */
} apkg_plan_view;
typedef enum {
    APKG_EFFECT_CREATE = 0,
    APKG_EFFECT_REPLACE,
    APKG_EFFECT_REMOVE,
    APKG_EFFECT_PRESERVE,
    APKG_EFFECT_OWNERSHIP,
    APKG_EFFECT_TRUST,
    APKG_EFFECT_DATABASE
} apkg_effect_kind;
typedef struct {
    apkg_effect_kind kind;
    apkg_text destination, subject;
    apkg_bytes record; /* exact old/new references, policy and preconditions */
} apkg_effect_view;
/* First operation tranche. Higher operation families get separate APIs later.
 * Plan is bound to snapshot, target, exact package and current local policy.
 * This function does not mutate the root or silently resolve dependencies. */
/* Explicit first-root path, implemented after CONTEXT. Seed policy is a
 * canonical policy object, displayed as effects and committed with root id.
 * No root writes or ids are persisted while preparing this plan. */
apkg_status apkg_plan_initialize(apkg_ctx *, const apkg_snapshot *absent,
                                 apkg_text target, apkg_bytes seed_policy,
                                 apkg_plan **, apkg_error **);
apkg_status apkg_plan_install(apkg_ctx *, const apkg_snapshot *,
                              const apkg_package *, const apkg_observer *,
                              apkg_plan **, apkg_error **);
/* Explicitly register an existing bundle without changing its observed tree.
 * Canonical adoption evidence supplies recorded file baselines and provenance;
 * these must match the supplied trusted descriptor. Original authentication
 * remains unknown. Personal/unowned paths remain protected in later changes. */
apkg_status apkg_plan_adopt(apkg_ctx *, const apkg_snapshot *,
                           const apkg_package *, apkg_bytes evidence,
                           apkg_plan **, apkg_error **);
/* Configuration entries use config="preserve" in signed file metadata.
 * Mutation plans retain exact observed trees, including foreign/edited files.
 */
apkg_status apkg_plan_remove(apkg_ctx *, const apkg_snapshot *, apkg_text name,
                             apkg_plan **, apkg_error **);
apkg_status apkg_plan_repair(apkg_ctx *, const apkg_snapshot *, apkg_text name,
                             apkg_plan **, apkg_error **);
apkg_status apkg_plan_upgrade(apkg_ctx *, const apkg_snapshot *,
                              const apkg_package *, apkg_plan **,
                              apkg_error **);
apkg_status apkg_plan_rollback(apkg_ctx *, const apkg_snapshot *,
                               apkg_text history, apkg_plan **, apkg_error **);
/* Pool order is a caller priority, never an implicit version ordering.
 * Installed suitable providers are preferred. No fetching/trust expansion. */
apkg_status apkg_plan_resolve(apkg_ctx *, const apkg_snapshot *,
                              apkg_package *const *requested,
                              size_t requested_count,
                              apkg_package *const *candidates,
                              size_t candidate_count, int include_recommends,
                              apkg_plan **, apkg_error **);
/* Explicit journaled metadata change: retain channel floor and independently
 * authorized signed observations. Preserves packages and grants no keys or
 * current approval. Rechecks a bound endpoint; same-sequence forks refuse. */
apkg_status apkg_plan_channel_accept(apkg_ctx *, const apkg_snapshot *,
                                      const apkg_channel *, apkg_plan **,
                                      apkg_error **);
/* Resolve signed metadata, fetch only selected archives and prepare one plan
 * for package changes plus the channel replay floor. Requires WRITE mode;
 * preparation can retain verified cache bytes but commits no installed state. */
apkg_status apkg_plan_channel_install(apkg_ctx *, const apkg_snapshot *,
    const apkg_channel *, const apkg_digest *descriptor, int recommendations,
    const apkg_observer *, apkg_plan **, apkg_error **);
const apkg_plan_view *apkg_plan_get(const apkg_plan *);
apkg_status apkg_plan_effect_at(const apkg_plan *, uint64_t,
                                const apkg_effect_view **, apkg_error **);
void apkg_plan_free(apkg_plan *);
/* Caller confirms only after presenting all effects. Digest must match.
 * Approval is process-local and binds this ctx's physical root and plan. */
apkg_status apkg_plan_approve(apkg_ctx *, const apkg_plan *,
                              const apkg_digest *confirmed, apkg_approval **,
                              apkg_error **);
void apkg_approval_free(apkg_approval *);
typedef enum {
    APKG_UNCHANGED = 0,
    APKG_COMMITTED,
    APKG_RESTORED,
    APKG_NEEDS_RECOVERY
} apkg_outcome;
typedef struct {
    apkg_id operation_id;
    apkg_status status;
    apkg_outcome outcome;
    int cancellation_requested;
    apkg_u64_fact committed_generation;
    apkg_bytes record; /* persistent transaction reference if one exists */
} apkg_result_view;
apkg_status apkg_execute(apkg_ctx *, const apkg_plan *, apkg_approval *,
                         const apkg_observer *, apkg_result **, apkg_error **);
/* Explicit recovery only; does not install new packages or expand authority. */
apkg_status apkg_recover(apkg_ctx *, const apkg_observer *, apkg_result **,
                         apkg_error **);
const apkg_result_view *apkg_result_get(const apkg_result *);
void apkg_result_free(apkg_result *);
#ifdef __cplusplus
}
#endif
#endif
