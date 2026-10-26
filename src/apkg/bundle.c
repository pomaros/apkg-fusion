/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#define _POSIX_C_SOURCE 200809L
#include "apkg_client.h"
#include "internal.h"
#include "pkg_application.h"
#include "pkg_fs.h"
#include "pkg_target.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef __AROS__
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/icon.h>
#include <proto/workbench.h>
#include <workbench/icon.h>
#include <workbench/workbench.h>
#endif
static int contained_path(const char *folder, const char *relative, char *out,
                          int directory) {
    char component[AK_PATH], canonical[AK_PATH];
    struct stat st;
    size_t i, n;
    if (!strcmp(relative, ".")) {
        if (!directory)
            return -1;
        strcpy(out, folder);
        return 0;
    }
    if (!ak_valid_path(relative) || ak_path(out, folder, relative))
        return -1;
    n = strlen(folder);
    strcpy(component, out);
    for (i = n + 1;; i++)
        if (component[i] == '/' || !component[i]) {
            char save = component[i];
            component[i] = 0;
            if (pkg_fs_path_is_link(component) != 0) {
                component[i] = save;
                return -1;
            }
            component[i] = save;
            if (!save)
                break;
        }
    if (directory)
        return pkg_fs_canonical_dir(out, canonical, sizeof canonical) ||
               strcmp(canonical, out);
    return lstat(out, &st) || !S_ISREG(st.st_mode);
}
static apkg_status inspect(const char *folder, const char *target,
                           struct pkg_application *app, char *canonical,
                           char *program, char *working, apkg_error **e) {
    unsigned char *raw = NULL, binary[64];
    size_t len, n;
    char path[AK_PATH], why[256];
    struct pkg_target tuple;
    apkg_status code = APKG_IO;
    if (!folder || !target || pkg_target_parse(target, &tuple, why, sizeof why))
        return APKG_TARGET;
    if (pkg_fs_canonical_dir(folder, canonical, AK_PATH))
        return ak_fail(e, APKG_IO, "cannot resolve application folder", folder);
    if (ak_path(path, canonical, "application.json") ||
        ak_read(path, &raw, &len, PKG_APPLICATION_MAX_SIZE))
        goto done;
    if (pkg_application_parse((char *)raw, len, app, why, sizeof why)) {
        code = ak_fail(e, APKG_INVALID, why, path);
        goto done;
    }
    if (strcmp(app->os, "aros") || strcmp(app->arch, tuple.cpu) ||
        strcmp(app->abi, tuple.abi)) {
        code = APKG_TARGET;
        goto done;
    }
    if (contained_path(canonical, app->executable, program, 0) ||
        contained_path(canonical, app->working_directory, working, 1)) {
        code = ak_fail(e, APKG_INVALID,
                       "launch path escapes bundle or contains a link", folder);
        goto done;
    }
    uint64_t binary_size;
    char sha[65];
    code = ak_stream_hash(program, &binary_size, sha, binary, &n);
    if (code)
        goto done;
    const char *cpu = pkg_application_binary_arch(binary, n);
    if (!*cpu || !pkg_target_cpu_matches(target, cpu)) {
        code = APKG_TARGET;
        goto done;
    }
    if (app->mode == PKG_APPLICATION_WORKBENCH) {
        struct stat st;
        if (snprintf(path, sizeof path, "%s.info", program) >=
                (int)sizeof path ||
            pkg_fs_path_is_link(path) != 0 || lstat(path, &st) ||
            !S_ISREG(st.st_mode)) {
            code = ak_fail(e, APKG_NOT_FOUND,
                           "Workbench launch requires a contained program icon",
                           program);
            goto done;
        }
    }
    code = APKG_OK;
done:
    free(raw);
    return code;
}
apkg_status apkg_bundle_check(const char *folder, const char *target,
                              char **out, apkg_error **e) {
    struct pkg_application app;
    char canonical[AK_PATH], program[AK_PATH], working[AK_PATH];
    ak_buf b = {0};
    apkg_status code;
    if (e)
        *e = NULL;
    if (!out)
        return APKG_INVALID;
    *out = NULL;
    code = inspect(folder, target, &app, canonical, program, working, e);
    if (code)
        return code;
    b.max = AK_MAX;
    ak_buf_add(&b,
               "Bundle: %s\nApplication: %s %s\nTarget: %s\nExecutable: "
               "%s\nWorking directory: %s\nMode: %s\nSystem requirements: %zu "
               "(runtime probing required at launch)\nRegistration: not "
               "required\nRuntime qualification: not asserted\n",
               canonical, app.id, app.version, target, program, working,
               app.mode == PKG_APPLICATION_WORKBENCH ? "workbench" : "shell",
               app.nrequirements);
    code = b.bad;
    if (!code) {
        *out = b.data;
        b.data = NULL;
    }
    free(b.data);
    return code;
}
apkg_status apkg_bundle_run(const char *folder, const char *target,
                            apkg_error **e) {
    struct pkg_application app;
    char canonical[AK_PATH], program[AK_PATH], working[AK_PATH];
    apkg_status code;
    if (e)
        *e = NULL;
    code = inspect(folder, target, &app, canonical, program, working, e);
    if (code)
        return code;
#ifndef __AROS__
    return ak_fail(e, APKG_UNSUPPORTED,
                   "AROS execution is unavailable on this host", folder);
#else
    /* Execution must target this binary's CPU/ABI, not a caller-selected cross
     * target. The native build supplies its ABI explicitly. */
    char running[32];
#if defined(__aarch64__)
    const char *cpu = "aarch64";
#elif defined(__x86_64__)
    const char *cpu = "x86_64";
#elif defined(__i386__)
    const char *cpu = "i386";
#else
    return APKG_UNSUPPORTED;
#endif
#ifndef PKG_RUNTIME
#define PKG_RUNTIME "native"
#endif
#ifndef PKG_ABI
    return APKG_UNSUPPORTED;
#else
    snprintf(running, sizeof running, "%s-aros-%s-%s", cpu, PKG_ABI,
             PKG_RUNTIME);
    if (strcmp(target, running))
        return APKG_TARGET;
#endif
    for (size_t i = 0; i < app.nrequirements; i++) {
        struct pkg_application_requirement *r = &app.requirements[i];
        if (strcmp(r->type, "library"))
            return ak_fail(e, APKG_UNSUPPORTED,
                           "unsupported runtime requirement probe", r->id);
        struct Library *library =
            OpenLibrary((CONST_STRPTR)r->id, r->min_version);
        if (!library)
            return ak_fail(e, APKG_DEPENDENCY,
                           "runtime library requirement is unavailable", r->id);
        CloseLibrary(library);
    }
    LONG stack = 8 * 1024 * 1024;
    struct Library *IconBase = OpenLibrary((CONST_STRPTR) "icon.library", 44);
    if (!IconBase)
        return APKG_UNSUPPORTED;
    struct DiskObject *icon = GetDiskObject((STRPTR)program);
    int valid_workbench = icon && icon->do_Type == WBTOOL &&
                          !(icon->do_ToolTypes &&
                            FindToolType(icon->do_ToolTypes, (STRPTR) "CLI"));
    if (icon) {
        if (icon->do_Type == WBTOOL && icon->do_StackSize > stack)
            stack = icon->do_StackSize;
        FreeDiskObject(icon);
    }
    CloseLibrary(IconBase);
    if (stack > 128 * 1024 * 1024)
        return APKG_LIMIT;
    if (app.mode == PKG_APPLICATION_WORKBENCH && !valid_workbench)
        return ak_fail(
            e, APKG_INVALID,
            "Workbench requires a real tool icon without CLI fallback",
            program);
    if (app.mode == PKG_APPLICATION_WORKBENCH) {
        struct Library *WorkbenchBase =
            OpenLibrary((CONST_STRPTR) "workbench.library", 0);
        if (!WorkbenchBase)
            return APKG_UNSUPPORTED;
        int accepted = OpenWorkbenchObjectA((STRPTR)program, NULL);
        CloseLibrary(WorkbenchBase);
        return accepted
                   ? APKG_OK
                   : APKG_IO; /* accepted request, not process confirmation */
    }
    BPTR cwd = Lock((CONST_STRPTR)working, ACCESS_READ), segment;
    if (!cwd)
        return APKG_IO;
    segment = LoadSeg((CONST_STRPTR)program);
    if (!segment) {
        UnLock(cwd);
        return APKG_IO;
    }
    BPTR previous = CurrentDir(cwd);
    LONG exit_code = RunCommand(segment, stack, (STRPTR) "", 0);
    CurrentDir(previous);
    UnLoadSeg(segment);
    UnLock(cwd);
    return exit_code == 0 ? APKG_OK : APKG_IO;
#endif
}
