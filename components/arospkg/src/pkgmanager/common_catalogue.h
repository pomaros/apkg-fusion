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
    uint64_t review_observations, runtime_observations, revocations;
    char *summary, *category;
    int summary_recorded, category_recorded;
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
    PM_CATALOGUE_REVIEW_OBSERVATIONS, PM_CATALOGUE_RUNTIME_OBSERVATIONS, PM_CATALOGUE_REVOCATIONS,
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
    char *summary, *category; /* NULL means unrecorded; empty remains recorded */
} pm_common_installed_row;
typedef struct {
    size_t count;
    uint64_t generation;
    apkg_digest snapshot;
    pm_common_installed_row rows[];
} pm_common_installed;
apkg_status pm_common_installed_read(const char *root, const char *term,
                                     pm_common_installed **, apkg_error **);
/* Pure combined view. Inputs must outlive this AllocVec and its list rows.
 * Candidate comparisons must come from the same captured root snapshot.
 * Complete identities and exact candidate selections remain in the inputs. */
typedef struct {
    const pm_common_installed_row *installed;
    const pm_common_row *candidate;
    const char *name, *authority, *version, *target, *summary, *category, *state;
} pm_common_library_row;
typedef struct {
    size_t count, category_count;
    pm_common_library_row *rows;
    char **categories; /* All categories, sorted top-level names, NULL */
} pm_common_library;
typedef enum {
    PM_LIBRARY_ALL, PM_LIBRARY_INSTALLED, PM_LIBRARY_NOT_INSTALLED,
    PM_LIBRARY_UPDATES
} pm_library_filter;
apkg_status pm_common_library_build(const pm_common_installed *,
    const pm_common_catalogue *, pm_common_library **);
int pm_common_library_matches(const pm_common_library_row *, pm_library_filter,
    const char *term, const char *category);
/* ARexx's 16-bit result boundary: refuse overflow, never truncate facts.
 * Successful text is owned by the client library; free with its text free. */
/* Current-target names from a revalidated retained catalogue; no network.
 * Duplicate builds collapse, but different authorities sharing a name refuse. */
apkg_status pm_common_rexx_list(const char *root, const pm_common_catalogue *,
                                const char *match, char **);
apkg_status pm_common_rexx_requirements(const char *root, const pm_common_catalogue *,
                                        const char *name, char **);
apkg_status pm_common_rexx_info(const char *, const char *, const char *,
                                char **);
apkg_status pm_common_rexx_outdated(const char *root,
                                    const pm_common_catalogue *,
                                    const char *field, char **,
                                    pm_common_catalogue **refreshed);
#endif
