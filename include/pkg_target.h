/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#ifndef PKG_TARGET_H
#define PKG_TARGET_H

#include <stddef.h>

/* A qualified target fits the channel's 31-character architecture field.
 * The profile identifies the runtime being targeted, independently of the
 * host running the publisher. Parsing identifies a target; it does not
 * certify that a build or its operating system has been tested there. */
#define PKG_TARGET_MAX 31

struct pkg_target {
    char cpu[8];                 /* x86_64, i386, aarch64, arm, m68k, ppc */
    char os[5];                  /* aros */
    char abi[4];                 /* v0, v1, v11 */
    char runtime[7];             /* native, hosted, raspi */
};

/* Canonical form: CPU-aros-ABI-runtime. No default, wildcard or inference.
 * raspi is the ARM Raspberry Pi runtime profile. Other CPU/ABI/runtime
 * combinations are syntactically representable without claiming support.
 * Return 0 on success, -1 on invalid input. Failure clears *out and writes
 * a reason when why != NULL and why_size > 0. NULL out is allowed to validate.
 */
int pkg_target_parse(const char *text, struct pkg_target *out,
                     char *why, size_t why_size);

/* Validate and format the separate fields. Return 0 or -1; failure empties
 * out when a nonempty output buffer was provided. The structure must have
 * NUL-terminated fields. Unterminated fields are refused within their bounds.
 */
int pkg_target_format(const struct pkg_target *target, char *out, size_t size);

/* Exact match on all four validated fields. Invalid or NULL input cannot
 * match, including two identical invalid structures. */
int pkg_target_matches(const struct pkg_target *a, const struct pkg_target *b);

/* Compare an ELF/Hunk CPU with either a qualified target or a legacy CPU
 * name. This preserves the engine's CPU-only inputs while new callers use
 * pkg_target_parse to require a complete target before publishing/installing.
 * CPU evidence cannot establish ABI or runtime compatibility. */
int pkg_target_cpu_matches(const char *declared, const char *binary_cpu);

#endif
