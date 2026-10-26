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
typedef int (*apkg_confirm_fn)(void *, const apkg_plan *, const char *presentation);
/* Local signed package directory: descriptor.json and signature.json.
 * No shell, no network and no implicit initialization/trust expansion. */
apkg_status apkg_client_install(const char *root, const char *package_dir, apkg_confirm_fn, void *,
                                const apkg_observer *, apkg_result **, apkg_error **);
#ifdef __cplusplus
}
#endif
#endif
