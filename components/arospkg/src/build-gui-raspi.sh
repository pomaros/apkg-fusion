#!/bin/sh
# Build PkgManager, the Zune front end, for native AROS on the Raspberry Pi
# (raspi-aarch64, ABIv1): the toolchain and SDK of build-raspi.sh, the sources
# of build-gui.sh. The Pi's own Developer SDK, never the hosted Macaros one.
set -e
RPI=${RPI:-$HOME/Work/AROS/toolchains/raspi-aarch64}
SDK=${SDK:-$RPI/sdk}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../PkgManager-raspi}

CC="$RPI/bin/aarch64-aros-clang"; AR="$RPI/bin/aarch64-aros-ar"; TAG=raspi-aarch64; ENTROPY=getentropy
. "$HERE/tls/tls.sh"
BUILD_ID=$(cat "$HERE"/libpkg/*.c "$HERE"/libpkg/*.h $TLS_SRC "$HERE"/pkgmanager/*.c "$HERE"/pkgmanager/*.h \
           | shasum -a 256 | cut -c1-16)
echo "build id $BUILD_ID  (PkgManager, raspi-aarch64 / ABIv1)"

"$CC" -O2 -Wall -Wextra -std=gnu99 $EXTRA_CFLAGS \
    -DPKG_BUILD_ID='"'"$BUILD_ID"'"' \
    -DPKG_TOOLCHAIN='"'"$RPI"'"' \
    -DPKG_ABI='"v1"' \
    -DPKG_TARGET='"aarch64-aros-raspi"' \
    -DPKG_SDK='"'"$SDK"'"' \
    -DPKG_CC="\"clang $("$CC" -dumpversion)\"" \
    $TLS_CFLAGS -DPKG_TLS='"'"$TLS"'"' \
    "$HERE"/libpkg/*.c "$HERE"/pkgmanager/*.c $TLS_SRC \
    "$SDK/lib/libz.static.a" $TLS_LIBS \
    -o "$OUT"
ls -l "$OUT"
