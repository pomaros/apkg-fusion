#!/bin/sh
# Build apkg for mainline AROS x86_64 (ABIv1). An ADDITIONAL build since
# 2026-09-19: v0.1 targets ABIv11 and is built by build.sh. This one is kept
# working because mainline is where the platform fixes land, but it does not
# gate a release.
set -e
TC=${TC:-/Volumes/arosmain/toolchain-mainline}
SDK=${SDK:-/Volumes/arosmain/build/bin/pc-x86_64/AROS/Developer}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../apkg-mainline}

# libz.a in the SDK is a link stub for z1.library, which is not everywhere.
# Link the static build instead -- the parent repo's scar list has this one.
# The build id is the hash of the sources: a binary cannot hash itself while
# being built, and this is reproducible. Every run report carries it, so a
# result can never be attributed to the wrong build.
CC="$TC/x86_64-aros-gcc"; AR="$TC/x86_64-aros-ar"; TAG=abiv1-x86_64; ENTROPY=getentropy
. "$HERE/tls/tls.sh"
BUILD_ID=$(cat "$HERE"/libpkg/*.c "$HERE"/libpkg/*.h $TLS_SRC "$HERE"/pkg/*.c "$HERE"/../third_party/aros-xterm/aros_tty_client.c \
           | shasum -a 256 | cut -c1-16)
echo "build id $BUILD_ID  (ABIv1 / mainline, additional)"

"$TC/x86_64-aros-gcc" -O2 -Wall -Wextra -std=gnu99 $EXTRA_CFLAGS \
    -DPKG_BUILD_ID='"'"$BUILD_ID"'"' \
    -DPKG_TOOLCHAIN='"'"$TC"'"' \
    -DPKG_ABI='"v1"' \
    -DPKG_SDK='"'"$SDK"'"' \
    -DPKG_CC="\"$("$TC/x86_64-aros-gcc" -dumpversion)\"" \
    -I"$SDK/include" $TLS_CFLAGS -DPKG_TLS='"'"$TLS"'"' \
    "$HERE"/libpkg/*.c "$HERE"/pkg/*.c "$HERE"/../third_party/aros-xterm/aros_tty_client.c $TLS_SRC \
    "$SDK/lib/libz.static.a" $TLS_LIBS \
    -o "$OUT"
ls -l "$OUT"
