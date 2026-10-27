#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
# Header/type check only. This does not link or execute an AROS program.
set -eu
: "${SDK_INCLUDE:?set SDK_INCLUDE to public AROS SDK include directory}"
AROS_CC=${AROS_CC:-clang}
cd "$(dirname "$0")/.."
set -- -std=gnu99 -Werror=implicit-function-declaration -Wno-typedef-redefinition -DAPKG_FUSION -DPKG_FS_LOCAL_ONLY -Iinclude -Isrc/apkg "-I$SDK_INCLUDE/aros/posixc" "-I$SDK_INCLUDE/aros/stdc" "-I$SDK_INCLUDE"
if [ -n "${AROS_TARGET:-}" ]; then set -- "--target=$AROS_TARGET" "$@"; fi
"$AROS_CC" "$@" -fsyntax-only src/apkg/*.c src/pkg_sha256.c src/pkg_sha512.c src/pkg_ed25519.c src/pkg_target.c src/pkg_application.c src/pkg_fs_posix.c components/arospkg/src/pkgmanager/common_worker.c components/arospkg/src/pkgmanager/common_browser.c components/arospkg/src/pkgmanager/main.c components/arospkg/src/pkgmanager/worker.c components/arospkg/src/pkgmanager/rexxcmd.c
# Optional SDK object audit: verify the native clients cannot reference the
# inherited mutation or self-update writers, even when those libraries remain
# linked for read-only presentation/network services.
if [ -n "${AROS_NM:-}" ]; then
    audit_dir=$(mktemp -d)
    trap 'rm -rf "$audit_dir"' EXIT HUP INT TERM
    for unit in main worker; do
        "$AROS_CC" "$@" -O2 -c "components/arospkg/src/pkgmanager/$unit.c" -o "$audit_dir/$unit.o"
        "$AROS_NM" --undefined-only "$audit_dir/$unit.o" | awk '{print $NF}' | sed 's/^_//' > "$audit_dir/$unit.symbols"
        if grep -E '^(pkg_(install|install_plan_run|remove|upgrade|rollback|update|recover)|su_(record|handoff_save|result_take|stage_release|apply))($|_)' "$audit_dir/$unit.symbols"; then
            echo "inherited mutation reference in PkgManager $unit" >&2
            exit 1
        fi
    done
    echo 'PkgManager object audit: no inherited mutation references'
fi
