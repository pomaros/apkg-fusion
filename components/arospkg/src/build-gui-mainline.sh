#!/bin/sh
# Build PkgManager for mainline AROS x86_64 (ABIv1). The same sources as
# build-gui.sh, against the mainline toolchain and SDK (Zune and the
# mainline C library are in the SDK; see build-mainline.sh for what else
# differs between the two ABIs). Additional; does not gate a release.
set -e
TC=${TC:-/Volumes/arosmain/toolchain-mainline}
SDK=${SDK:-/Volumes/arosmain/build/bin/pc-x86_64/AROS/Developer}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../PkgManager-mainline}
[ -x "$TC/x86_64-aros-gcc" ] || { echo "no mainline toolchain at $TC (mount /Volumes/arosmain)"; exit 1; }

CC="$TC/x86_64-aros-gcc"; AR="$TC/x86_64-aros-ar"; TAG=abiv1-x86_64; ENTROPY=getentropy
. "$HERE/tls/tls.sh"
# APKG_FUSION=1 builds the common-engine client, as in build-gui.sh.
set --
FUSION_FLAGS=""
if [ "${APKG_FUSION:-0}" = 1 ]; then
    FUSION_ROOT=$(cd "$HERE/../../.." && pwd)
    for source in $(make -s -C "$FUSION_ROOT" print-apkg-sources); do
        set -- "$@" "$FUSION_ROOT/$source"
    done
    FUSION_FLAGS="-DAPKG_FUSION -DPKG_FS_LOCAL_ONLY"
    set -- "$@" "-I$FUSION_ROOT/include" "-I$FUSION_ROOT/src/apkg"
fi
BUILD_ID=$(cat "$HERE"/libpkg/*.c "$HERE"/libpkg/*.h $TLS_SRC "$HERE"/pkgmanager/*.c "$HERE"/pkgmanager/*.h \
               "$HERE"/../third_party/zunekit/*.c "$HERE"/../third_party/zunekit/*.h \
           | shasum -a 256 | cut -c1-16)
echo "build id $BUILD_ID  (PkgManager, ABIv1 / mainline, additional)"

"$CC" -O2 -Wall -Wextra -std=gnu99 $EXTRA_CFLAGS $FUSION_FLAGS \
    -DPKG_BUILD_ID='"'"$BUILD_ID"'"' \
    -DPKG_TOOLCHAIN='"'"$(echo "$TC" | sed "s|^$HOME|~|")"'"' \
    -DPKG_ABI='"v1"' \
    -DPKG_SDK='"'"$(echo "$SDK" | sed "s|^$HOME|~|")"'"' \
    -DPKG_CC="\"$("$CC" -dumpversion)\"" \
    -I"$SDK/include" $TLS_CFLAGS -DPKG_TLS='"'"$TLS"'"' \
    -DLINKTEXT_EMBEDDED -I"$HERE/../third_party/zunekit" \
    "$HERE"/libpkg/*.c "$HERE"/pkgmanager/*.c "$HERE"/../third_party/zunekit/*.c "$@" $TLS_SRC \
    "$SDK/lib/libz.static.a" $TLS_LIBS -L"$SDK/lib" \
    -o "$OUT"
ls -l "$OUT"
