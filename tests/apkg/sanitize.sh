#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
set -eu
cd "$(dirname "$0")/../.."
CC=${CC:-cc}
sources=$(make -s print-apkg-sources)
mkdir -p build/common-sanitized
set -- -DPKG_FS_LOCAL_ONLY -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude -Isrc/apkg
# Source paths are the repository-owned manifest emitted above.
# shellcheck disable=SC2086
"$CC" "$@" tests/apkg/test_memory.c $sources -o build/common-sanitized/memory
# shellcheck disable=SC2086
"$CC" "$@" src/apkg/main.c $sources -o build/common-sanitized/apkg-core
# shellcheck disable=SC2086
"$CC" "$@" -DAPKG_FUSION -Itests/pm_stubs -Icomponents/arospkg/src/pkgmanager tests/apkg/test_pm_worker.c components/arospkg/src/pkgmanager/common_worker.c components/arospkg/src/pkgmanager/common_browser.c $sources -o build/common-sanitized/pm-worker
./build/common-sanitized/memory
APKG_CLI="$PWD/build/common-sanitized/apkg-core" APKG_PM_WORKER="$PWD/build/common-sanitized/pm-worker" PYTHONPATH=tests/apkg python3 -m unittest test_client test_attributes.Attributes.test_actual_cli_and_worker_install_identical_metadata -v
