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
BUILD_ID=$(cat "$HERE"/libpkg/*.c "$HERE"/libpkg/*.h $TLS_SRC "$HERE"/pkgmanager/*.c "$HERE"/pkgmanager/*.h \
           | shasum -a 256 | cut -c1-16)
echo "build id $BUILD_ID  (PkgManager, ABIv1 / mainline, additional)"

"$CC" -O2 -Wall -Wextra -std=gnu99 $EXTRA_CFLAGS \
    -DPKG_BUILD_ID='"'"$BUILD_ID"'"' \
    -DPKG_TOOLCHAIN='"'"$TC"'"' \
    -DPKG_ABI='"v1"' \
    -DPKG_SDK='"'"$SDK"'"' \
    -DPKG_CC="\"$("$CC" -dumpversion)\"" \
    -I"$SDK/include" $TLS_CFLAGS -DPKG_TLS='"'"$TLS"'"' \
    "$HERE"/libpkg/*.c "$HERE"/pkgmanager/*.c $TLS_SRC \
    "$SDK/lib/libz.static.a" $TLS_LIBS -L"$SDK/lib" \
    -o "$OUT"
ls -l "$OUT"
