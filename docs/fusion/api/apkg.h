/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * CONTRACT DRAFT: declarations only; not shipped or linked by libpkg.a. */
#ifndef APKG_CONTRACT_DRAFT_H
#define APKG_CONTRACT_DRAFT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define APKG_CONTRACT_VERSION 1u

typedef struct apkg_ctx apkg_ctx;
typedef struct apkg_snapshot apkg_snapshot;
typedef struct apkg_package apkg_package;
typedef struct apkg_plan apkg_plan;
typedef struct apkg_approval apkg_approval;
typedef struct apkg_result apkg_result;
typedef struct apkg_error apkg_error;
typedef struct apkg_cancel apkg_cancel;
typedef struct apkg_event_copy apkg_event_copy;
/* Length excludes terminator. Text is UTF-8, no embedded NUL; bytes are opaque.
 * A zero length permits NULL. All returned views are borrowed, never free(). */
typedef struct { const char *data; size_t size; } apkg_text;
typedef struct { const unsigned char *data; size_t size; } apkg_bytes;
typedef struct { unsigned char bytes[16]; } apkg_id;
typedef struct { unsigned char bytes[32]; } apkg_digest;
typedef enum {
    APKG_OK = 0, APKG_INVALID, APKG_UNSUPPORTED, APKG_LIMIT, APKG_NOMEM,
    APKG_NOT_FOUND, APKG_IO, APKG_BUSY, APKG_STALE, APKG_RECOVERY_REQUIRED,
    APKG_INTEGRITY, APKG_SIGNATURE, APKG_TRUST, APKG_TARGET, APKG_CONFLICT,
    APKG_DEPENDENCY, APKG_CANCELLED, APKG_POLICY, APKG_NETWORK
} apkg_status;
typedef enum { APKG_UNKNOWN = 0, APKG_KNOWN, APKG_INVALID_FACT } apkg_fact_state;
typedef struct { apkg_fact_state state; uint64_t value; apkg_text evidence; } apkg_u64_fact;
typedef struct { apkg_fact_state state; apkg_text value, evidence; } apkg_text_fact;
typedef struct { apkg_status code; apkg_text message, field, subject; } apkg_error_view;
const apkg_error_view *apkg_error_get(const apkg_error *);
void apkg_error_free(apkg_error *);

typedef enum { APKG_READ = 0, APKG_WRITE } apkg_mode;
typedef struct {
    uint32_t contract_version;  /* must be APKG_CONTRACT_VERSION */
    size_t struct_size;         /* must be sizeof(apkg_open_options) */
    apkg_text root;             /* explicit host path, never an ambient default */
    apkg_mode mode;
    uint64_t max_object_bytes, max_memory_bytes; /* 0 selects documented defaults */
} apkg_open_options;
/* No initialization, locking, recovery, download or path creation at open. */
apkg_status apkg_open(const apkg_open_options *, apkg_ctx **, apkg_error **);
void apkg_close(apkg_ctx *);
/* One active synchronous call per ctx. Distinct contexts may run concurrently. */
typedef enum { APKG_ROOT_ABSENT = 0, APKG_ROOT_LEGACY, APKG_ROOT_READY,
               APKG_ROOT_PENDING, APKG_ROOT_INVALID } apkg_root_state;
typedef struct {
    apkg_root_state state;
    apkg_fact_state identity_state;
    apkg_id root_id;            /* meaningful only when identity_state == KNOWN */
    apkg_u64_fact generation, pending_count;
    apkg_digest snapshot_digest; /* requires KNOWN identity/generation in common state */
    apkg_text_fact target;
    apkg_text diagnostic;       /* explains INVALID/unsupported root state */
    apkg_bytes source_record;   /* raw canonical common bytes, or empty for legacy */
} apkg_root_view;
apkg_status apkg_snapshot_read(apkg_ctx *, apkg_snapshot **, apkg_error **);
const apkg_root_view *apkg_snapshot_root(const apkg_snapshot *);
void apkg_snapshot_free(apkg_snapshot *);

typedef enum { APKG_REASON_UNKNOWN = 0, APKG_REASON_EXPLICIT,
    APKG_REASON_DEPENDENCY, APKG_REASON_RECOMMENDATION, APKG_REASON_SET } apkg_reason;
typedef enum { APKG_SIGNATURE_UNKNOWN = 0, APKG_SIGNATURE_VALID,
    APKG_SIGNATURE_INVALID } apkg_signature_state;
typedef enum { APKG_TRUST_UNKNOWN = 0, APKG_TRUST_ACCEPTED,
    APKG_TRUST_REJECTED } apkg_trust_state;
typedef struct {
    apkg_id instance_id;
    apkg_text_fact authority, name, version, target;
    apkg_u64_fact revision;
    apkg_reason reason;
    apkg_signature_state signature;
    apkg_trust_state trust;
    apkg_bytes record; /* full record/evidence; no lossy fixed-size UI projection */
} apkg_installed_view;
uint64_t apkg_snapshot_installed_count(const apkg_snapshot *);
apkg_status apkg_snapshot_installed_at(const apkg_snapshot *, uint64_t,
                                      const apkg_installed_view **, apkg_error **);
/* Source bytes/signatures are copied; no implicit fetching or trust changes. */
apkg_status apkg_package_read(apkg_ctx *, apkg_bytes descriptor,
                             apkg_bytes signature_envelope,
                             apkg_package **, apkg_error **);
void apkg_package_free(apkg_package *);

typedef enum { APKG_PHASE_CHECK = 0, APKG_PHASE_FETCH, APKG_PHASE_STAGE,
    APKG_PHASE_APPLY, APKG_PHASE_COMMIT, APKG_PHASE_RECOVER } apkg_phase;
typedef enum { APKG_EVENT_PROGRESS = 0, APKG_EVENT_DIAGNOSTIC } apkg_event_kind;
typedef struct {
    apkg_id operation_id;
    uint64_t sequence;
    apkg_event_kind kind;
    apkg_phase phase;
    apkg_text subject, message;
    uint64_t completed;
    apkg_u64_fact total;        /* unknown total never means zero or 100% */
    apkg_text unit;             /* bytes, files, packages; no mixed counters */
    int cancellable;            /* cancellation accepted at this boundary */
} apkg_event;
typedef enum { APKG_CONTINUE = 0, APKG_REQUEST_CANCEL } apkg_event_action;
typedef apkg_event_action (*apkg_event_fn)(void *, const apkg_event *);
typedef struct { apkg_event_fn event; void *user; apkg_cancel *cancel; } apkg_observer;
apkg_status apkg_cancel_create(apkg_cancel **);
void apkg_cancel_request(apkg_cancel *); /* only cross-thread mutation API */
void apkg_cancel_free(apkg_cancel *);
apkg_status apkg_event_clone(const apkg_event *, apkg_event_copy **);
const apkg_event *apkg_event_get(const apkg_event_copy *);
void apkg_event_free(apkg_event_copy *);

typedef enum { APKG_PLAN_INITIALIZE = 0, APKG_PLAN_INSTALL } apkg_plan_kind;
typedef struct {
    apkg_plan_kind kind;
    apkg_digest plan_digest;
    apkg_id root_id;
    uint64_t generation, effect_count;
    apkg_bytes canonical;       /* exact approved bytes, not presentation text */
    int executable;            /* 0: preview with blockers; cannot approve */
} apkg_plan_view;
typedef enum { APKG_EFFECT_CREATE = 0, APKG_EFFECT_REPLACE, APKG_EFFECT_REMOVE,
    APKG_EFFECT_PRESERVE, APKG_EFFECT_OWNERSHIP, APKG_EFFECT_TRUST,
    APKG_EFFECT_DATABASE } apkg_effect_kind;
typedef struct {
    apkg_effect_kind kind;
    apkg_text destination, subject;
    apkg_bytes record;          /* exact old/new references, policy and preconditions */
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
const apkg_plan_view *apkg_plan_get(const apkg_plan *);
apkg_status apkg_plan_effect_at(const apkg_plan *, uint64_t,
                               const apkg_effect_view **, apkg_error **);
void apkg_plan_free(apkg_plan *);
/* Caller confirms only after presenting all effects. Digest must match.
 * Approval is process-local and binds this ctx's physical root and plan. */
apkg_status apkg_plan_approve(apkg_ctx *, const apkg_plan *,
                             const apkg_digest *confirmed,
                             apkg_approval **, apkg_error **);
void apkg_approval_free(apkg_approval *);
typedef enum { APKG_UNCHANGED = 0, APKG_COMMITTED, APKG_RESTORED,
               APKG_NEEDS_RECOVERY } apkg_outcome;
typedef struct {
    apkg_id operation_id;
    apkg_status status;
    apkg_outcome outcome;
    int cancellation_requested;
    apkg_u64_fact committed_generation;
    apkg_bytes record;          /* persistent transaction reference if one exists */
} apkg_result_view;
apkg_status apkg_execute(apkg_ctx *, const apkg_plan *, apkg_approval *,
                         const apkg_observer *, apkg_result **, apkg_error **);
/* Explicit recovery only; does not install new packages or expand authority. */
apkg_status apkg_recover(apkg_ctx *, const apkg_observer *,
                         apkg_result **, apkg_error **);
const apkg_result_view *apkg_result_get(const apkg_result *);
void apkg_result_free(apkg_result *);
#ifdef __cplusplus
}
#endif
#endif
