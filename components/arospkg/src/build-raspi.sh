#!/bin/sh
# Build apkg for native AROS on the Raspberry Pi (raspi-aarch64, ABIv1), with
# the toolchain in ~/Work/AROS/toolchains/raspi-aarch64: Clang 20 over the
# Pi's own Developer SDK, never the hosted Macaros one. Its archive is named
# aarch64-aros-raspi (PKG_TARGET) until the Pi and hosted aarch64 builds are
# known to be interchangeable.
set -e
RPI=${RPI:-$HOME/Work/AROS/toolchains/raspi-aarch64}
SDK=${SDK:-$RPI/sdk}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../apkg-raspi}

# The wrapper adds the target, the SDK sysroot, -mcmodel=large, -ffixed-x18,
# -fno-pic and -fno-pie. Like mainline, the Pi SDK has getentropy() in posixc
# and a static zlib; its libz.a is the z1.library stub.
CC="$RPI/bin/aarch64-aros-clang"; AR="$RPI/bin/aarch64-aros-ar"; TAG=raspi-aarch64; ENTROPY=getentropy
. "$HERE/tls/tls.sh"
BUILD_ID=$(cat "$HERE"/libpkg/*.c "$HERE"/libpkg/*.h $TLS_SRC "$HERE"/pkg/*.c "$HERE"/../third_party/aros-xterm/aros_tty_client.c \
           | shasum -a 256 | cut -c1-16)
echo "build id $BUILD_ID  (raspi-aarch64 / ABIv1)"

"$CC" -O2 -Wall -Wextra -std=gnu99 $EXTRA_CFLAGS \
    -DPKG_BUILD_ID='"'"$BUILD_ID"'"' \
    -DPKG_TOOLCHAIN='"'"$RPI"'"' \
    -DPKG_ABI='"v1"' \
    -DPKG_TARGET='"aarch64-aros-raspi"' \
    -DPKG_SDK='"'"$SDK"'"' \
    -DPKG_CC="\"clang $("$CC" -dumpversion)\"" \
    $TLS_CFLAGS -DPKG_TLS='"'"$TLS"'"' \
    "$HERE"/libpkg/*.c "$HERE"/pkg/*.c "$HERE"/../third_party/aros-xterm/aros_tty_client.c $TLS_SRC \
    "$SDK/lib/libz.static.a" $TLS_LIBS \
    -o "$OUT"
ls -l "$OUT"
