#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
# AArch64 SDK profile, hosted or native. Build only: no OS is started or modified.
set -eu
: "${APKG_AROS_SDK:?set APKG_AROS_SDK to an AROS target SDK root (containing AROS/Developer and gen)}"
: "${APKG_AROS_TOOLS:?set APKG_AROS_TOOLS to the cross-toolchain root}"
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
target=${APKG_AROS_TARGET:-aarch64-unknown-aros}
abi=${APKG_AROS_ABI:-v1}
case "$abi" in v0|v1|v11) ;; *) echo "invalid ABI" >&2; exit 2;; esac
# hosted reserves x18 for the host OS; native AROS does not and links
# position-dependent with emulated TLS, like the native SDK's own programs.
runtime=${APKG_AROS_RUNTIME:-hosted}
case "$runtime" in
hosted) runtime_flags="-ffixed-x18" ;;
native) runtime_flags="-fno-pic -fno-pie -femulated-tls" ;;
*) echo "invalid runtime" >&2; exit 2;;
esac
case "$target" in aarch64-unknown-aros) ;; *) echo 'this SDK profile supports aarch64-unknown-aros only' >&2; exit 2;; esac
sdk=$APKG_AROS_SDK
dev=$sdk/AROS/Developer
cc=$APKG_AROS_TOOLS/bin/clang
ar=$APKG_AROS_TOOLS/bin/llvm-ar
out=${APKG_AROS_OUT:-$repo/build/aros-common/$target-$abi-$runtime}
# collect-aros lives in the SDK's tools, or beside a native build's host tools.
export COMPILER_PATH="${APKG_AROS_HOST_TOOLS:-$sdk/tools}:$APKG_AROS_TOOLS/bin"
# A build tree keeps generated headers and posixc under gen/; a published SDK
# (PomAROS-SDK-aarch64) has posixc under AROS/Developer/include and no gen/.
if [ -d "$sdk/gen/include" ]; then
    gen_include="-isystem $sdk/gen/include"; posixc=$sdk/gen/include/aros/posixc
else
    gen_include=; posixc=$dev/include/aros/posixc
fi
for need in "$cc" "$ar" "$dev/lib/startup.o"; do [ -e "$need" ] || { echo "missing $need" >&2; exit 2; }; done
mkdir -p "$out"
cd "$repo"
if [ "${APKG_AROS_GUI:-0}" = 1 ]; then
    SDK_INCLUDE="$dev/include" AROS_CC="$cc" AROS_NM="$APKG_AROS_TOOLS/bin/llvm-nm" AROS_TARGET="$target" sh tools/check-apkg-aros.sh
fi
build_id=$(python3 - <<'BUILDID'
import hashlib
import subprocess
from pathlib import Path
paths = set()
for folder in ('src/apkg', 'components/arospkg/src/libpkg', 'components/arospkg/src/pkgmanager', 'components/arospkg/src/tls', 'components/arospkg/third_party/zunekit'):
    for pattern in ('*.c', '*.h'): paths.update(Path(folder).glob(pattern))
paths.update(Path('include').glob('*.h'))
paths.update(Path(p) for p in subprocess.check_output(['make', '-s', 'print-apkg-sources'], text=True).split())
paths.add(Path('src/apkg/changes.inc'))
h = hashlib.sha256()
for p in sorted(paths): h.update(str(p).encode()+b'\0'+p.read_bytes())
print(h.hexdigest()[:16])
BUILDID
)
# Links without --allow-multiple-definition, so a duplicate symbol in our
# code fails the build. SDKs whose <aros/symbolsets.h> still defines
# AROS_LIBREQ with ".global" (mainline before it became ".weak", such as the
# Macaros SDK of 2026-09) emit __aros_libreq_<base>.<version> in every unit
# that calls a library; only when those are the sole duplicates is the link
# repeated with the option, and the symbols are listed.
link_checked() {
    log=$out/link-$(basename "$1").log
    shift
    if "$@" -Wl,--error-limit=0 2>"$log"; then cat "$log" >&2; return 0; fi
    dups=$(sed -n 's/^ld\.lld: error: duplicate symbol: //p' "$log" | sort -u)
    other=$(grep ': error:' "$log" | grep -v ': error: duplicate symbol: ' | grep -v 'linker command failed' || true)
    if [ -n "$dups" ] && [ -z "$other" ] && ! printf '%s\n' "$dups" | grep -qv '^__aros_libreq_' &&
        grep -q '\.global __aros_libreq_' "$dev/include/aros/symbolsets.h"; then
        echo "note: this SDK defines AROS_LIBREQ symbols as global; relinking with --allow-multiple-definition for:" $dups >&2
        "$@" -Wl,--allow-multiple-definition
        return
    fi
    cat "$log" >&2
    return 1
}
# shellcheck disable=SC2086
set -- "--target=$target" "-DPKG_ABI=\"$abi\"" "-DPKG_RUNTIME=\"$runtime\"" "-DPKG_BUILD_ID=\"$build_id\"" -mcmodel=large $runtime_flags -O2 -std=gnu11 -isystem "$dev/include" $gen_include -isystem "$posixc" -isystem "$dev/include/aros/stdc" -Iinclude -Isrc/apkg
sources=$(make -s print-apkg-sources)
# AROS has no curl: the engine reads a channel with the inherited HTTP client,
# whose https is Mbed TLS carried in the program, checked against the bundled
# certificate authorities. MBEDTLS_TEST_SW_INET_PTON: posixc has no inet_pton.
sh tools/ca-bundle-c.sh third_party/cacert/cacert.pem > "$out/pkg_cabundle.c"
tls=third_party/mbedtls
mkdir -p "$out/tls"
for source in "$tls"/library/*.c; do
    object=$out/tls/$(basename "$source" .c).o
    if [ "$object" -nt "$source" ] && [ "$object" -nt "$tls/pkg_mbedtls_config.h" ] && [ "${APKG_AROS_REBUILD:-0}" != 1 ]; then continue; fi
    "$cc" "$@" -Os -w -DMBEDTLS_CONFIG_FILE='"pkg_mbedtls_config.h"' -DMBEDTLS_TEST_SW_INET_PTON -I"$tls" -I"$tls/include" -I"$tls/library" -c "$source" -o "$object"
done
"$ar" rcs "$out/libpkgtls.a" "$out"/tls/*.o
# The source list consists of repository-owned relative paths without spaces.
# shellcheck disable=SC2086
link_checked apkg-core "$cc" "$@" -DMBEDTLS_CONFIG_FILE='"pkg_mbedtls_config.h"' -isystem "$tls" -isystem "$tls/include" -nostartfiles -nodefaultlibs -L "$dev/lib" -L "$APKG_AROS_TOOLS/lib/generic" "$dev/lib/startup.o" src/apkg/main.c $sources src/pkg_tls_aros.c "$out/pkg_cabundle.c" "$out/libpkgtls.a" -o "$out/apkg-core" -Wl,--start-group -lworkbench -licon -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros -lautoinit -llibinit -lutility -lamiga -larossupport -Wl,--end-group -lclang_rt.builtins-aarch64
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
# The engine's TLS glue (src/pkg_tls_aros.c) and the inherited client's
# (components/arospkg/src/tls) both define mbedtls_hardware_poll and
# mbedtls_ms_time, and SocketBase is defined in the client's net.c and in
# src/pkg_fs_posix.c. Until one of each goes, this link accepts duplicates
# and the first definition on the command line is used.
# shellcheck disable=SC2086
"$cc" "$@" -Wl,--allow-multiple-definition -DAPKG_FUSION -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>' -Icomponents/arospkg/src/tls -I"$mb/include" -nostartfiles -nodefaultlibs -L "$dev/lib" -L "$APKG_AROS_TOOLS/lib/generic" "$dev/lib/startup.o" -DLINKTEXT_EMBEDDED -Icomponents/arospkg/third_party/zunekit components/arospkg/src/libpkg/*.c components/arospkg/src/pkgmanager/*.c components/arospkg/third_party/zunekit/*.c components/arospkg/src/tls/mbedtls.c components/arospkg/src/tls/platform_aros.c components/arospkg/src/tls/entropy_getentropy.c $sources src/pkg_tls_aros.c "$out/pkg_cabundle.c" "$out/libmbedtls.a" "$dev/lib/libz.static.a" -o "$out/PkgManager" -Wl,--start-group -lmui -lmuimaster -lintuition -lgraphics -lworkbench -licon -ldatatypes -lasl -lrexxsyslib -lworkbench -licon -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros -lautoinit -llibinit -lutility -lamiga -larossupport -Wl,--end-group -lclang_rt.builtins-aarch64
"$APKG_AROS_TOOLS/bin/llvm-nm" --defined-only "$out/PkgManager" > "$out/gui-symbols.txt"
for symbol in main __startup_entry pm_common_install apkg_execute; do
    awk -v symbol="$symbol" '$NF==symbol {found=1} END {exit !found}' "$out/gui-symbols.txt" || { echo "missing GUI program symbol $symbol" >&2; exit 1; }
done
echo "built $out/apkg-core and $out/PkgManager"
