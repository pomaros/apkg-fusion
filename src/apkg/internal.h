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
#define AK_PATH 4096
#define AK_MAX 16777216u
#define AK_TOKENS 100000u
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
    char path[AK_PATH];
    int token;
} ak_tree_row;
int ak_buf_add(ak_buf *, const char *, ...);
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
int ak_exists(const char *); /* -1 error, 0 absent, 1 present */
int ak_resolve(const char *, char *, struct stat *);
int ak_physical(const char *, const struct stat *);
char *ak_slice(const ak_json *, int);
apkg_status ak_tree_read(const char *, size_t, char **, struct stat *);
apkg_status ak_changes_format(const apkg_plan *, char **);
int ak_relations_valid(const ak_json *, int);
int ak_requirement_matches(const ak_json *, int, const apkg_package *);
apkg_status ak_relations_check(apkg_package *const *, size_t);
apkg_status ak_verified_entry(apkg_ctx *, const char *, apkg_package **);
int ak_version_compare(const char *, const char *);
int ak_tree_find(const ak_json *, const char *);
struct apkg_ctx {
    char root[AK_PATH], anchor[AK_PATH];
    struct stat binding;
    apkg_mode mode;
    size_t object_max, memory_max;
    unsigned char session[16];
};
struct apkg_snapshot {
    apkg_root_view view;
    char *raw, *policy, *target, *diagnostic;
    apkg_installed_view *rows;
    size_t nrows;
    char **records;
    char anchor[AK_PATH];
    struct stat binding;
};
struct apkg_error {
    apkg_error_view view;
    char *message, *subject;
};
void ak_hash(const void *, size_t, apkg_digest *);
apkg_status ak_snapshot(apkg_ctx *, apkg_snapshot **, apkg_error **);
int ak_policy(const ak_json *, int, const char *, const char *);
int ak_policy_valid(const ak_json *);
struct apkg_package {
    char *raw, *signature, *name, *version, *authority, *target;
    size_t raw_size;
    uint64_t revision;
    ak_json json;
    unsigned char key[32];
    apkg_digest digest;
};
struct apkg_plan {
    apkg_plan_view view;
    apkg_effect_view *effects;
    char *raw, *root_record, *package, *name;
    size_t package_size;
    char root[AK_PATH], anchor[AK_PATH];
    struct stat binding;
    unsigned char session[16];
    apkg_digest before;
    int absent;
    char *changes, *before_state;
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
apkg_status ak_plan_records(apkg_ctx *, const apkg_snapshot *, const char *,
                            apkg_plan_kind, const char *, apkg_plan **,
                            apkg_error **);
apkg_status ak_make_plan(apkg_ctx *, const apkg_snapshot *, const char *,
                         const char *, const char *, apkg_plan **,
                         apkg_error **);
int ak_cancelled(const apkg_observer *);
int ak_descriptor(const ak_json *);
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
