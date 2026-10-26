#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
# Header/type check only. This does not link or execute an AROS program.
set -eu
: "${SDK_INCLUDE:?set SDK_INCLUDE to public AROS SDK include directory}"
AROS_CC=${AROS_CC:-clang}
cd "$(dirname "$0")/.."
set -- -std=gnu99 -Werror=implicit-function-declaration -Wno-typedef-redefinition -fsyntax-only -DAPKG_FUSION -DPKG_FS_LOCAL_ONLY -Iinclude -Isrc/apkg "-I$SDK_INCLUDE/aros/posixc" "-I$SDK_INCLUDE/aros/stdc" "-I$SDK_INCLUDE"
if [ -n "${AROS_TARGET:-}" ]; then set -- "--target=$AROS_TARGET" "$@"; fi
"$AROS_CC" "$@" src/apkg/*.c src/pkg_sha256.c src/pkg_sha512.c src/pkg_ed25519.c src/pkg_target.c src/pkg_application.c src/pkg_fs_posix.c components/arospkg/src/pkgmanager/common_worker.c components/arospkg/src/pkgmanager/main.c components/arospkg/src/pkgmanager/worker.c
