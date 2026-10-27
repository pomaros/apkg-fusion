/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#ifndef PM_COMMON_CATALOGUE_H
#define PM_COMMON_CATALOGUE_H
#include "apkg_client.h"
/* One AllocVec allocation, including all rows and text. No library-owned
 * handle or worker malloc allocation survives the worker process. */
typedef struct {
    char *name, *version, *target, *authority, *detail;
    char selection[163];
    uint64_t revision, archive_bytes;
    apkg_signature_state signature;
    apkg_trust_state author_trust;
    int target_matches;
} pm_common_row;
typedef struct {
    size_t count;
    char *endpoint, *summary;
    apkg_channel_view channel;
    pm_common_row rows[];
} pm_common_catalogue;
apkg_status pm_common_catalogue_copy(const apkg_channel *, const char *,
                                     pm_common_catalogue **);
#endif
