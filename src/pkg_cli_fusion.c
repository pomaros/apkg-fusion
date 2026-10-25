/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Native command front end for the integration build. Operations dispatch
 * through libpkg; argument translation adds no package-management policy.
 */
#include "pkg.h"
#include "pkg_application.h"
#include "pkg_target.h"
#include "pkg_out.h"
#include "pkg_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void help(void)
{
    pkg_out("apkg-fusion development client\n"
            "  apkg install NAME --root DIR --channel DIR --target CPU-aros-ABI-runtime\n"
            "  apkg publish DIR --channel DIR --name NAME --version VERSION --target TARGET --sign KEY\n"
            "  apkg verify NAME --root DIR\n"
            "  apkg bundle-check DRAWER\n"
            "  apkg help COMMAND\n"
            "\nCommands: install, upgrade, rollback, remove, verify, repair, list,\n"
            "status, search, show, resolve, channel, env, keygen, keyinfo,\n"
            "manifest, publish, withdraw, push, sign, checksig, image, mountlist.\n"
            "Options use --root, --channel, --sign, --file, --version, --machine,\n"
            "--dry-run, --all and the underlying operation's documented names.\n"
            "A target is explicit, for example x86_64-aros-v11-native.\n"
            "bundle-check validates the description; execution is not tested.\n"
            "This development client does not yet replace released APKG clients.\n");
}

static int bundle_check(const char *drawer)
{
    char *path, *text, why[256];
    struct pkg_application app;
    FILE *f;
    size_t n, length = strlen(drawer);
    int rc = PKG_RC_INTEGRITY;
    if (length > 4096) return PKG_RC_USAGE;
    path = pkg_join(drawer, PKG_APPLICATION_FILE);
    text = malloc(PKG_APPLICATION_MAX_SIZE + 1u);
    if (!path || !text) { free(path); free(text); return PKG_RC_IO; }
    f = fopen(path, "rb");
    if (!f) { pkg_err("cannot read %s\n", path); free(path); free(text); return PKG_RC_IO; }
    n = fread(text, 1, PKG_APPLICATION_MAX_SIZE + 1u, f);
    if (ferror(f)) { pkg_err("cannot read %s\n", path); rc = PKG_RC_IO; }
    else if (pkg_application_parse(text, n, &app, why, sizeof why))
        pkg_err("invalid application description: %s\n", why);
    else {
        pkg_out("description: valid\nid: %s\ncpu: %s\nabi: %s\nexecutable: %s\n"
                "runtime: unchecked\n", app.id, app.arch, app.abi, app.executable);
        rc = PKG_RC_OK;
    }
    fclose(f); free(path); free(text);
    return rc;
}

/* Encode long-option values as KEY=value. The established parser then takes
 * them literally, including --names and uppercase keyword names. It remains
 * responsible for validating each operation's options and arity. */
int pkg_fusion_cli(int argc, char **argv, int (*dispatch)(int, char **))
{
    char **words;
    char command[64];
    int i, n = 0, rc, has_target = 0;
    size_t command_length, j;
    if (argc < 2) { help(); return 0; }
    command_length = strlen(argv[1]);
    if (command_length >= sizeof command) {
        pkg_err("command name is too long\n"); return PKG_RC_USAGE;
    }
    for (j = 0; j <= command_length; j++)
        command[j] = argv[1][j] >= 'A' && argv[1][j] <= 'Z'
            ? (char)(argv[1][j] - 'A' + 'a') : argv[1][j];
    if (!strcmp(command, "--help") || !strcmp(command, "-h")) {
        if (argc != 2) { pkg_err("usage: apkg --help\n"); return PKG_RC_USAGE; }
        help(); return 0;
    }
    if (!strcmp(command, "bundle-check")) {
        if (argc != 3) { pkg_err("usage: apkg bundle-check DRAWER\n"); return PKG_RC_USAGE; }
        return bundle_check(argv[2]);
    }
    if (!strcmp(command, "version") || !strcmp(command, "--version") || !strcmp(command, "-v")) {
        if (argc != 2) { pkg_err("usage: apkg --version\n"); return PKG_RC_USAGE; }
        pkg_out("apkg-fusion development build; shared engine %s\n", PKG_VERSION_STRING);
        return 0;
    }
    if (!strcmp(command, "u") || !strcmp(command, "self-update")) {
        pkg_err("fusion self-update requires the merged release channel, which is not configured\n");
        return PKG_RC_POLICY;
    }
    words = calloc((size_t)argc + 1u, sizeof *words);
    if (!words) return PKG_RC_IO;
    for (i = 0; i < argc; i++) {
        const char *s = i == 1 ? command : argv[i];
        char mapped[64];
        size_t j, k = 0;
        if (i > 1 && !strncmp(s, "--", 2)) {
            int takes_value;
            if (!strcmp(s, "--target")) {
                char why[160];
                if (i + 1 >= argc || pkg_target_parse(argv[i + 1], NULL, why, sizeof why)) {
                    pkg_err("--target requires CPU-aros-ABI-runtime\n");
                    rc = PKG_RC_USAGE; goto out;
                }
                has_target = 1; s = "ARCH";
            } else if (!strcmp(s, "--arch")) {
                pkg_err("use --target CPU-aros-ABI-runtime\n"); rc = PKG_RC_USAGE; goto out;
            } else {
                for (j = 2; s[j] && k + 1u < sizeof mapped; j++) {
                    if (s[j] == '-') continue;
                    if (s[j] < 'a' || s[j] > 'z') break;
                    mapped[k++] = (char)(s[j] - 'a' + 'A');
                }
                if (s[j] || !k) { pkg_err("invalid option: %s\n", s); rc = PKG_RC_USAGE; goto out; }
                mapped[k] = 0; s = mapped;
            }
            takes_value = strcmp(s, "MACHINE") && strcmp(s, "DRYRUN") &&
                strcmp(s, "ALL") && strcmp(s, "ORPHANS") && strcmp(s, "DOWNGRADE") &&
                strcmp(s, "TAKEOVER") && strcmp(s, "METADATA") && strcmp(s, "SYSTEM") &&
                strcmp(s, "SSH");
            if (takes_value && i + 1 >= argc) {
                pkg_err("option requires a value: %s\n", argv[i]); rc = PKG_RC_USAGE; goto out;
            }
            if (takes_value) {
                size_t option_length = strlen(s), value_length = strlen(argv[i + 1]);
                words[n] = malloc(option_length + value_length + 2u);
                if (!words[n]) { rc = PKG_RC_IO; goto out; }
                memcpy(words[n], s, option_length);
                words[n][option_length] = '=';
                memcpy(words[n] + option_length + 1u, argv[i + 1], value_length + 1u);
                n++; i++;
                continue;
            }
        }
        words[n] = malloc(strlen(s) + 1u);
        if (!words[n]) { rc = PKG_RC_IO; goto out; }
        strcpy(words[n++], s);
    }
    if ((!strcmp(command, "install") || !strcmp(command, "publish") ||
         !strcmp(command, "upgrade") || !strcmp(command, "rollback") ||
         !strcmp(command, "repair")) && !has_target) {
        pkg_err("this operation requires --target CPU-aros-ABI-runtime\n");
        rc = PKG_RC_USAGE;
    } else rc = dispatch(n, words);
out:
    for (i = 0; i < n; i++) free(words[i]);
    free(words);
    return rc;
}
