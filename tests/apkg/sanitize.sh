#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
set -eu
cd "$(dirname "$0")/../.."
CC=${CC:-cc}
export ASAN_OPTIONS=${ASAN_OPTIONS:-halt_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1}
sources=$(make -s print-apkg-sources)
rexx_sources=$(make -s print-pm-rexx-sources)
mkdir -p build/common-sanitized
set -- -DPKG_FS_LOCAL_ONLY -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude -Isrc/apkg
# Source paths are the repository-owned manifest emitted above.
# shellcheck disable=SC2086
"$CC" "$@" tests/apkg/test_memory.c $sources -o build/common-sanitized/memory
# shellcheck disable=SC2086
"$CC" "$@" src/apkg/main.c $sources -o build/common-sanitized/apkg-core
# shellcheck disable=SC2086
"$CC" "$@" -DAPKG_FUSION -Itests/pm_stubs -Icomponents/arospkg/src/pkgmanager tests/apkg/test_pm_worker.c components/arospkg/src/pkgmanager/common_worker.c components/arospkg/src/pkgmanager/common_browser.c $sources -o build/common-sanitized/pm-worker
# shellcheck disable=SC2086
"$CC" "$@" tests/apkg/test_channel_memory.c $sources -o build/common-sanitized/channel-memory
# shellcheck disable=SC2086
"$CC" "$@" -DAPKG_TESTING tests/apkg/test_preparation_memory.c $sources -o build/common-sanitized/preparation-memory
# The inherited parser corpus also runs with fusion's complete name/job fields.
# shellcheck disable=SC2086
"$CC" "$@" -std=gnu99 -DAPKG_FUSION $rexx_sources -o build/common-sanitized/rexx
./build/common-sanitized/rexx
./build/common-sanitized/memory
APKG_PREPARATION_MEMORY="$PWD/build/common-sanitized/preparation-memory" APKG_CHANNEL_MEMORY="$PWD/build/common-sanitized/channel-memory" APKG_CLI="$PWD/build/common-sanitized/apkg-core" APKG_PM_WORKER="$PWD/build/common-sanitized/pm-worker" PYTHONPATH=tests/apkg python3 -m unittest test_client test_attributes.Attributes.test_actual_cli_and_worker_install_identical_metadata test_source test_author test_channel test_file_channel test_http_channel test_channel_plan test_channel_heads test_preparation test_channel_clients test_browser test_installed_browser test_inspection test_updates test_summary test_source_config test_attestation test_channel_attestation test_qualification_memory test_adopt.AdoptionClients test_migration.MigrationCopies test_self_update test_policy test_sources test_system test_migration.MigrationSystem test_alpha test_review_regressions test_rexx_catalogue test_requirements_query test_verify -v
