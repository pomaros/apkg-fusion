#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
# Build only: no running OS is started or modified.
set -eu
: "${APKG_AROS_SDK:?set APKG_AROS_SDK to an AROS target SDK root (containing AROS/Developer and gen)}"
: "${APKG_AROS_TOOLS:?set APKG_AROS_TOOLS to the cross-toolchain root}"
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
target=${APKG_AROS_TARGET:-aarch64-unknown-aros}
abi=${APKG_AROS_ABI:-v1}
case "$abi" in v0|v1|v11) ;; *) echo "invalid ABI" >&2; exit 2;; esac
case "$target" in *[!a-zA-Z0-9._-]*|'') echo 'invalid target' >&2; exit 2;; esac
sdk=$APKG_AROS_SDK
dev=$sdk/AROS/Developer
cc=$APKG_AROS_TOOLS/bin/clang
ar=$APKG_AROS_TOOLS/bin/llvm-ar
out=$repo/build/aros-common/$target
export COMPILER_PATH="$sdk/tools:$APKG_AROS_TOOLS/bin"
for need in "$cc" "$ar" "$dev/lib/startup.o"; do [ -e "$need" ] || { echo "missing $need" >&2; exit 2; }; done
mkdir -p "$out"
cd "$repo"
build_id=$(python3 - <<'BUILDID'
import hashlib
import subprocess
from pathlib import Path
paths = set()
for folder in ('src/apkg', 'components/arospkg/src/libpkg', 'components/arospkg/src/pkgmanager', 'components/arospkg/src/tls'):
    for pattern in ('*.c', '*.h'): paths.update(Path(folder).glob(pattern))
paths.update(Path('include').glob('*.h'))
paths.update(Path(p) for p in subprocess.check_output(['make', '-s', 'print-apkg-sources'], text=True).split())
paths.add(Path('src/apkg/changes.inc'))
h = hashlib.sha256()
for p in sorted(paths): h.update(str(p).encode()+b'\0'+p.read_bytes())
print(h.hexdigest()[:16])
BUILDID
)
set -- "--target=$target" "-DPKG_ABI=\"$abi\"" '-DPKG_RUNTIME="hosted"' "-DPKG_BUILD_ID=\"$build_id\"" -mcmodel=large -ffixed-x18 -O2 -std=gnu11 -DPKG_FS_LOCAL_ONLY -isystem "$dev/include" -isystem "$sdk/gen/include" -isystem "$sdk/gen/include/aros/posixc" -isystem "$dev/include/aros/stdc" -Iinclude -Isrc/apkg
sources=$(make -s print-apkg-sources)
# The source list consists of repository-owned relative paths without spaces.
# shellcheck disable=SC2086
"$cc" "$@" -nostartfiles -nodefaultlibs -L "$dev/lib" -L "$APKG_AROS_TOOLS/lib/generic" "$dev/lib/startup.o" src/apkg/main.c $sources -o "$out/apkg-core" -Wl,--allow-multiple-definition -Wl,--start-group -lworkbench -licon -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros -lautoinit -llibinit -lutility -lamiga -larossupport -Wl,--end-group -lclang_rt.builtins-aarch64
# AROS programs are relocatable ELF. A successful link with garbage collection
# can silently discard main/startup, so verify real program roots explicitly.
"$APKG_AROS_TOOLS/bin/llvm-nm" --defined-only "$out/apkg-core" > "$out/core-symbols.txt"
for symbol in main __startup_entry apkg_execute; do
    awk -v symbol="$symbol" '$NF==symbol {found=1} END {exit !found}' "$out/core-symbols.txt" || { echo "missing CLI program symbol $symbol" >&2; exit 1; }
done
[ "${APKG_AROS_GUI:-0}" = 1 ] || { echo "built $out/apkg-core"; exit 0; }
# Explicit entropy profile, matching the common engine's getentropy service.
mb=components/arospkg/third_party/mbedtls
config=components/arospkg/src/tls/mbedtls_config.h
mkdir -p "$out/mbedtls"
for source in "$mb"/library/*.c; do
    object=$out/mbedtls/$(basename "$source" .c).o
    if [ "$object" -nt "$source" ] && [ "$object" -nt "$config" ] && [ "${APKG_AROS_REBUILD:-0}" != 1 ]; then continue; fi
    "$cc" "$@" -Os -w -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>' -DMBEDTLS_TEST_SW_INET_PTON -Icomponents/arospkg/src/tls -I"$mb/include" -I"$mb/library" -c "$source" -o "$object"
done
"$ar" rcs "$out/libmbedtls.a" "$out"/mbedtls/*.o
# shellcheck disable=SC2086
"$cc" "$@" -DAPKG_FUSION -DPKG_FS_LOCAL_ONLY -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>' -Icomponents/arospkg/src/tls -I"$mb/include" -nostartfiles -nodefaultlibs -L "$dev/lib" -L "$APKG_AROS_TOOLS/lib/generic" "$dev/lib/startup.o" components/arospkg/src/libpkg/*.c components/arospkg/src/pkgmanager/*.c components/arospkg/src/tls/mbedtls.c components/arospkg/src/tls/platform_aros.c components/arospkg/src/tls/entropy_getentropy.c $sources "$out/libmbedtls.a" "$dev/lib/libz.static.a" -o "$out/PkgManager" -Wl,--allow-multiple-definition -Wl,--start-group -lmui -lmuimaster -lintuition -lgraphics -lworkbench -licon -ldatatypes -lasl -lrexxsyslib -lworkbench -licon -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros -lautoinit -llibinit -lutility -lamiga -larossupport -Wl,--end-group -lclang_rt.builtins-aarch64
"$APKG_AROS_TOOLS/bin/llvm-nm" --defined-only "$out/PkgManager" > "$out/gui-symbols.txt"
for symbol in main __startup_entry pm_common_install apkg_execute; do
    awk -v symbol="$symbol" '$NF==symbol {found=1} END {exit !found}' "$out/gui-symbols.txt" || { echo "missing GUI program symbol $symbol" >&2; exit 1; }
done
echo "built $out/apkg-core and $out/PkgManager"
