/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#ifndef APKG_INTERNAL_H
#define APKG_INTERNAL_H
#include "apkg.h"
#include "pkg_sha256.h"
#include <stddef.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
/* Identity fields stay 64-bit even when the target POSIX ino_t is 32-bit. */
typedef struct {
    struct stat metadata;
    uint64_t st_dev, st_ino;
} ak_identity;
#define AK_PATH 4096
#define AK_MAX 16777216u
#define AK_TOKENS 100000u
/* Total declared file bytes of one descriptor, shared blob references
 * counted once per entry: the materialized tree never exceeds the largest
 * PKG/1 envelope. */
#define AK_TREE_MAX 2147483647u
typedef struct {
    size_t start, end, next;
    char type;
} ak_token;
typedef struct {
    char *text;
    size_t size, count, capacity;
    ak_token *tokens;
} ak_json;
typedef struct {
    char *data;
    size_t size, cap, max;
    int bad;
} ak_buf;
typedef struct {
    char *path;
    int token;
} ak_tree_row;
int ak_fields_optional(const ak_json *, int, const char *const *, size_t,
                       const char *const *, size_t);
apkg_status ak_attributes_format(const ak_json *, int, ak_buf *);
apkg_status ak_attributes_normalize(const char *, uint64_t, char **);
int ak_attributes_valid(const ak_json *, int, uint64_t);
apkg_status ak_attributes_read(const char *, uint64_t, char **);
apkg_status ak_attributes_write(const char *, const char *, uint64_t);
int ak_attributes_equal(const ak_json *, int, const ak_json *, int);
apkg_status ak_attributes_merge(const ak_json *, int, const ak_json *, int,
                                const ak_json *, int, uint64_t, char **);
apkg_status ak_source_entry(const char *, char **, apkg_error **);
apkg_status ak_tree_entry(const char *, const char *, char **);
apkg_status ak_tree_source_read(const char *, size_t, char **, ak_identity *);
apkg_status ak_source_inventory(const char *, char **, apkg_error **);
int ak_legacy_main(int, char **);
apkg_status ak_verify_command(int, char **, char **);
apkg_status ak_author_command(int, char **, char **);
apkg_status ak_signature_check(apkg_bytes, apkg_bytes, const char *,
                               unsigned char[32], apkg_digest *);
int ak_channel_policy(const ak_json *, const char *, const char *);
apkg_status ak_channel_validate(const ak_json *);
apkg_status ak_channel_format(const apkg_channel *, char **);
apkg_status ak_channel_snapshot_check(const apkg_snapshot *, const apkg_channel *);
apkg_status ak_channel_snapshot_capture(const apkg_snapshot *, apkg_channel *);
apkg_status ak_channel_inspect(const char *, const char *, const char *,
                               uint64_t, int, char **, apkg_error **);
#if defined(__GNUC__) || defined(__clang__)
int ak_buf_add(ak_buf *, const char *, ...) __attribute__((format(printf, 2, 3)));
#else
int ak_buf_add(ak_buf *, const char *, ...);
#endif
apkg_status ak_tree_normalize(const char *, char **);
apkg_status ak_blob_retain(apkg_ctx *, const char *, uint64_t, const char *);
int ak_fault(int);
int ak_publish(const char *, const char *);
apkg_status ak_stream_hash(const char *, uint64_t *, char[65],
                           unsigned char[64], size_t *);
apkg_status ak_blob_path(apkg_ctx *, const char *, char *);
apkg_status ak_blob_check(apkg_ctx *, const char *, uint64_t, const char *);
apkg_status ak_blob_copy(apkg_ctx *, const char *, uint64_t, const char *,
                         const char *);
apkg_status ak_blob_inline(apkg_ctx *, const ak_json *, int);
typedef struct {
    const apkg_observer *observer;
    apkg_id operation_id;
    uint64_t sequence, completed, total, last_emitted;
    long long last_ms;
    const char *subject;
    int unknown_total, has_phase;
    apkg_phase phase, last_phase;
} ak_import_progress;
int ak_http_url(const char *);
apkg_status ak_http_path(const char *, const char *, char[AK_PATH]);
apkg_status ak_http_get(const char *, uint64_t, int, unsigned char **, size_t *,
                        ak_import_progress *);
apkg_status ak_channel_metadata(apkg_ctx *, const apkg_channel *, uint64_t,
                                apkg_package **, apkg_error **);
apkg_status ak_channel_select(apkg_ctx *, const apkg_channel *, uint64_t,
                              const apkg_observer *, apkg_package **,
                              apkg_error **);
apkg_status ak_channel_source(const apkg_channel *, const char **, int *);
apkg_status ak_import_tick(ak_import_progress *, uint64_t);
apkg_status ak_blob_ingest(apkg_ctx *, const char *, uint64_t, int,
                           ak_import_progress *);
apkg_status ak_package_payload_check(apkg_ctx *, const apkg_package *);
apkg_status ak_package_source(apkg_ctx *, const char *, const apkg_observer *,
                              apkg_package **, apkg_error **);
int ak_text_path_valid(const char *, size_t);
int ak_json_parse(const void *, size_t, ak_json *);
void ak_json_free(ak_json *);
int ak_member(const ak_json *, int, const char *);
int ak_string(const ak_json *, int, char *, size_t);
int ak_equal(const ak_json *, int, const char *);
int ak_u64(const ak_json *, int, uint64_t *);
int ak_hex(const char *, unsigned char *, size_t);
void ak_tohex(const unsigned char *, size_t, char *);
int ak_object(const ak_json *, const char *);
int ak_fields(const ak_json *, int, const char *const *, size_t);
int ak_empty(const ak_json *, int);
apkg_status ak_fail(apkg_error **, apkg_status, const char *, const char *);
char *ak_copy(const void *, size_t);
int ak_path(char *, const char *, const char *);
int ak_read(const char *, unsigned char **, size_t *, size_t);
int ak_write(const char *, const void *, size_t, int);
int ak_sync_dir(const char *);
int ak_remove_tree(const char *);
int ak_parent(const char *, char *);
int ak_exists(const char *); /* -1 error, 0 absent, 1 present */
int ak_resolve(const char *, char *, ak_identity *);
int ak_physical(const char *, const ak_identity *);
char *ak_slice(const ak_json *, int);
apkg_status ak_tree_read(const char *, size_t, char **, ak_identity *);
apkg_status ak_changes_format(const apkg_plan *, char **);
int ak_relations_valid(const ak_json *, int);
int ak_requirement_matches(const ak_json *, int, const apkg_package *);
apkg_status ak_relations_check(apkg_package *const *, size_t);
apkg_status ak_verified_entry(apkg_ctx *, const char *, apkg_package **);
typedef struct {
    char *entries;
    apkg_package **packages;
    size_t count;
} ak_resolution;
apkg_status ak_resolve_metadata(apkg_ctx *, const apkg_snapshot *,
                                apkg_package *const *, size_t,
                                apkg_package *const *, size_t, int,
                                ak_resolution *, apkg_error **);
void ak_resolution_free(ak_resolution *);
int ak_version_compare(const char *, const char *);
/* Root-owned ordered source list (sources.c). */
#define AK_SOURCES 16u
apkg_status ak_plan_channels_install(apkg_ctx *, const apkg_snapshot *,
                                     const apkg_channel *const *, size_t,
                                     size_t, const apkg_digest *, int,
                                     const apkg_observer *, apkg_plan **,
                                     apkg_error **);
int ak_sources_valid(const ak_json *, int);
char *ak_sources_field(const char *sources_array); /* NULL on NOMEM */
apkg_status ak_sources_delta(const char *before_state, const char *after_state,
                             ak_buf *);
/* stat/lstat whose st_dev/st_ino name the object across renames and
 * restarts (identity.c); st_ino 0 means the filesystem gives no identity. */
int ak_stat(const char *, ak_identity *);
int ak_lstat(const char *, ak_identity *);
int ak_identity_known(const ak_identity *);
int ak_tree_find(const ak_json *, const char *);
/* System requirements (system.c). */
int ak_system_valid(const ak_json *, int);
apkg_status ak_system_observe(const apkg_package *const *, size_t, size_t,
                              char **, apkg_error **);
int ak_requirements_valid(const ak_json *, int);
apkg_status ak_plan_system_check(const apkg_plan *);
apkg_status ak_system_format(const char *, ak_buf *);
struct apkg_ctx {
    char root[AK_PATH], anchor[AK_PATH];
    ak_identity binding;
    apkg_mode mode;
    size_t object_max, memory_max;
    unsigned char session[16];
    /* Retained qualification records are pure functions of their bytes;
     * the last set verified by this context is not re-verified. */
    int records_verified;
    apkg_digest records_digest;
};
struct apkg_snapshot {
    apkg_root_view view;
    char *raw, *policy, *target, *diagnostic, *channel_heads, *qualification_records;
    /* "" without a source list, else ,"sources":[...] for state builders. */
    char *sources_field;
    apkg_installed_view *rows;
    size_t nrows;
    uint64_t package_generation;
    char **records;
    char anchor[AK_PATH];
    ak_identity binding;
};
struct apkg_error {
    apkg_error_view view;
    char *message, *subject;
};
void ak_hash(const void *, size_t, apkg_digest *);
apkg_status ak_snapshot(apkg_ctx *, apkg_snapshot **, apkg_error **);
int ak_policy(const ak_json *, int, const char *, const char *);
apkg_status ak_attestation_scope(const ak_json *, apkg_digest *);
void ak_attestation_memory(apkg_attestation *, int, uint64_t, const apkg_digest *);
int ak_qualification_records_valid(const ak_json *, int);
apkg_status ak_qualification_records_add(const apkg_snapshot *,const apkg_channel *,char **);
apkg_status ak_qualification_augment(const apkg_snapshot *,apkg_bytes,apkg_bytes,apkg_attestation ***,size_t *);
apkg_status ak_attestation_observe(const apkg_snapshot *, apkg_bytes, apkg_bytes,
    apkg_bytes, apkg_bytes, const apkg_attestation *, apkg_attestation **, apkg_error **);
int ak_attestation_scope_equal(const apkg_attestation *, const apkg_attestation *);
apkg_status ak_channel_attestations(const apkg_snapshot *, apkg_bytes, apkg_bytes,
    const ak_json *, int, apkg_attestation ***, size_t *);
apkg_status ak_channel_claim_format(const apkg_channel *,uint64_t,uint64_t,char **,apkg_error **);
int ak_channel_claims_json(const apkg_channel *, uint64_t, ak_buf *);
apkg_status ak_attestation_validate(const ak_json *);
int ak_attestation_policy(const ak_json *, const char *, const char *, const char *);
int ak_policy_valid(const ak_json *);
struct apkg_package {
    char *raw, *signature, *name, *version, *authority, *target, *origin;
    char *adoption; /* operator ownership evidence, never publisher metadata */
    size_t raw_size;
    uint64_t revision;
    ak_json json;
    unsigned char key[32];
    apkg_digest digest;
};
int ak_adoption_valid(const ak_json *, int, const ak_json *);
int ak_adoption_protected(const ak_json *, const char *);
void ak_entry_write(ak_buf *, const apkg_package *, const char *,
                    const apkg_package *);
struct apkg_plan {
    apkg_plan_view view;
    apkg_effect_view *effects;
    char *raw, *root_record, *package, *name;
    size_t package_size;
    char root[AK_PATH], anchor[AK_PATH];
    ak_identity binding;
    unsigned char session[16];
    apkg_digest before;
    int absent;
    char *changes, *before_state, *origins;
    char *requirements; /* system requirement observations, or NULL */
};
struct apkg_approval {
    unsigned char session[16];
    apkg_digest digest;
    int used;
};
struct apkg_result {
    apkg_result_view view;
    char record[AK_PATH + 128];
};
struct apkg_cancel {
    unsigned requested;
};
struct apkg_event_copy {
    apkg_event view;
    char *strings[4];
};
/* policy NULL keeps the snapshot's trust policy; only APKG_PLAN_POLICY
 * replaces it. */
apkg_status ak_plan_records_heads(apkg_ctx *, const apkg_snapshot *,
                                  const char *, apkg_plan_kind, const char *,
                                  const char *, const char *, const char *,
                                  const char *, apkg_plan **, apkg_error **);
apkg_status ak_policy_delta(const char *before_state, const char *after_state,
                            ak_buf *);
apkg_status ak_plan_records(apkg_ctx *, const apkg_snapshot *, const char *,
                            apkg_plan_kind, const char *, apkg_plan **,
                            apkg_error **);
apkg_status ak_make_plan(apkg_ctx *, const apkg_snapshot *, const char *,
                         const char *, const char *, apkg_plan **,
                         apkg_error **);
apkg_status ak_origin_check(apkg_ctx *, const ak_json *, int,
                            const apkg_observer *);
apkg_status ak_plan_bind(apkg_ctx *, apkg_plan *, const apkg_package *const *,
                         size_t, apkg_error **);
apkg_status ak_plan_inputs_check(apkg_ctx *, const apkg_plan *, const apkg_observer *);
apkg_status ak_plan_origins_check(apkg_ctx *, const apkg_plan *,
                                  const apkg_observer *);
int ak_origins_valid(const ak_json *, int);
apkg_status ak_channel_heads_add(const apkg_snapshot *, const apkg_id *,
                                 uint64_t, const apkg_digest *, char **);
int ak_channel_heads_valid(const ak_json *, int);
apkg_status ak_channel_head_check(const apkg_snapshot *, const apkg_id *,
                                  uint64_t, const apkg_digest *,
                                  apkg_u64_fact *);
apkg_status ak_plan_heads(apkg_ctx *, const apkg_snapshot *, const char *, const char *,
                          apkg_plan **, apkg_error **);
int ak_cancelled(const apkg_observer *);
int ak_descriptor(const ak_json *);
int ak_presentation_valid(const ak_json *, int);
int ak_valid_name(const char *);
int ak_valid_path(const char *);
#ifdef APKG_TESTING
void *ak_test_malloc(size_t);
void *ak_test_calloc(size_t, size_t);
#ifndef APKG_ALLOC_IMPL
#define malloc ak_test_malloc
#define calloc ak_test_calloc
#endif
#endif
#endif
