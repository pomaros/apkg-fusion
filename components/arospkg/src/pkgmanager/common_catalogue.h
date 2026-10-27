/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#ifndef PM_COMMON_CATALOGUE_H
#define PM_COMMON_CATALOGUE_H
#include "apkg_client.h"
/* One AllocVec allocation, including all rows and text. No library-owned
 * handle or worker malloc allocation survives the worker process. */
typedef struct {
    char *name, *version, *target, *authority, *detail, *installed_version;
    char selection[163];
    uint64_t revision, archive_bytes;
    apkg_signature_state signature;
    apkg_trust_state author_trust;
    int target_matches;
    apkg_candidate_state comparison;
    uint64_t installed_revision;
    apkg_fact_state installed_revision_state;
} pm_common_row;
typedef struct {
    size_t count;
    char *endpoint, *summary;
    apkg_channel_view channel;
    apkg_bytes observation, observation_signature;
    uint64_t root_generation;
    apkg_digest root_snapshot;
    apkg_update_summary_view
        updates; /* entries/text deep-copied into AllocVec */
    pm_common_row rows[];
} pm_common_catalogue;
typedef enum {
    PM_CATALOGUE_ALL,
    PM_CATALOGUE_UPDATES,
    PM_CATALOGUE_NOT_INSTALLED,
    PM_CATALOGUE_OTHER_TARGET,
    PM_CATALOGUE_UNTRUSTED,
    PM_CATALOGUE_CONTENT_CONFLICT,
    PM_CATALOGUE_INDETERMINATE,
    PM_CATALOGUE_FILTER_COUNT
} pm_catalogue_filter;
/* Pure offline rebuild; input remains owned by the UI and unchanged on failure.
 * Output is a new AllocVec owned by the caller. No transfer origin is reused.
 */
apkg_status pm_common_catalogue_refresh(const char *,
                                        const pm_common_catalogue *,
                                        pm_common_catalogue **);
apkg_status pm_common_source_load(const char *root, const char *profile,
                                  pm_common_catalogue **);
apkg_status pm_common_source_save(const char *root, const char *profile,
                                  const char *endpoint, const char *identity,
                                  const pm_common_catalogue *);
int pm_common_row_matches(const pm_common_row *, pm_catalogue_filter,
                          const char *);
int pm_common_row_reviewable(const pm_common_row *);
apkg_status pm_common_catalogue_copy(const apkg_channel *,
                                     const apkg_snapshot *, const char *,
                                     pm_common_catalogue **);
/* The installed list retains the complete typed view, including evidence and
 * opaque record bytes. Every borrowed field is copied into this AllocVec. */
typedef struct {
    apkg_installed_view view;
    char *version_label;
} pm_common_installed_row;
typedef struct {
    size_t count;
    pm_common_installed_row rows[];
} pm_common_installed;
apkg_status pm_common_installed_read(const char *root, const char *term,
                                     pm_common_installed **, apkg_error **);
/* ARexx's 16-bit result boundary: refuse overflow, never truncate facts.
 * Successful text is owned by the client library; free with its text free. */
apkg_status pm_common_rexx_info(const char *, const char *, const char *,
                                char **);
apkg_status pm_common_rexx_outdated(const char *root,
                                    const pm_common_catalogue *,
                                    const char *field, char **,
                                    pm_common_catalogue **refreshed);
#endif
