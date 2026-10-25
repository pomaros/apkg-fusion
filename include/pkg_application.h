/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Application folder schema 1, from Tomasz Staniak's arospkg RFC 0002.
 */
#ifndef PKG_APPLICATION_H
#define PKG_APPLICATION_H
#include <stddef.h>

#define PKG_APPLICATION_FILE "application.json"
#define PKG_APPLICATION_MAX_SIZE (64u * 1024u)
#define PKG_APPLICATION_MAX_REQUIREMENTS 16

enum pkg_application_mode {
    PKG_APPLICATION_WORKBENCH = 1,
    PKG_APPLICATION_SHELL = 2
};
struct pkg_application_requirement {
    char type[24];
    char id[64];
    unsigned long min_version;
};
struct pkg_application {
    char id[64], name[128], version[64];
    unsigned long revision;
    char os[16], arch[16], abi[16];
    enum pkg_application_mode mode;
    char executable[256], working_directory[256];
    struct pkg_application_requirement requirements[PKG_APPLICATION_MAX_REQUIREMENTS];
    size_t nrequirements;
};

/* Pure parsing, without catalogue, registry, filesystem access or execution.
 * Zero on success; -1 with a reason on refusal. Output is zeroed on failure.
 * Unknown launch/target/requirement fields and all duplicate fields fail.
 * A successful parse makes no assertion about installed runtime requirements,
 * file containment through links, binary compatibility or launch success.
 */
int pkg_application_parse(const char *text, size_t length,
                          struct pkg_application *out, char *error, size_t error_size);
/* RFC 0002 relative path validation. "." names the drawer itself; callers
 * must refuse it when selecting an executable. Zero means valid. */
int pkg_application_path_valid(const char *path, char *error, size_t error_size);
/* ELF machine / Amiga hunk identification. Empty string means unrecognised.
 * This identifies a CPU only; ABI and successful execution need other checks. */
const char *pkg_application_binary_arch(const unsigned char *header, size_t size);
#endif
