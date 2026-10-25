#!/bin/sh
# Build apkg for AROS i386 (ABIv0). Not released; the catalogue has no ABIv0
# packages yet. zlib is compiled from the sources the i386 build fetched,
# because that SDK has zlib.h but no library.
set -e
TC=${TC:-/Volumes/arosabiv0/toolchain-abiv0-i386}
SDK=${SDK:-/Volumes/arosabiv0/pc-i386/bin/pc-i386/AROS/Development}
ZSRC=${ZSRC:-/Volumes/arosabiv0/pc-i386/bin/pc-i386/Ports/zlib/zlib-1.2.13}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../apkg-i386}

CC="$TC/i386-aros-gcc"; AR="$TC/i386-aros-ar"; TAG=abiv0-i386
. "$HERE/tls/tls.sh"
ZOBJ="$HERE/../build/$TAG/zlib"
if [ ! -f "$ZOBJ/libz.a" ]; then
    mkdir -p "$ZOBJ"
    for f in adler32 crc32 inffast inflate inftrees zutil simd_stub; do
        $CC -O2 -w -I"$ZSRC" -c "$ZSRC/$f.c" -o "$ZOBJ/$f.o"
    done
    $AR rcs "$ZOBJ/libz.a" "$ZOBJ"/*.o
fi
BUILD_ID=$(cat "$HERE"/libpkg/*.c "$HERE"/libpkg/*.h $TLS_SRC "$HERE"/pkg/*.c "$HERE"/../third_party/aros-xterm/aros_tty_client.c \
           | shasum -a 256 | cut -c1-16)
echo "build id $BUILD_ID  (i386 / ABIv0, not released)"
$CC -O2 -Wall -Wextra -std=gnu99 $EXTRA_CFLAGS \
    -DPKG_BUILD_ID='"'"$BUILD_ID"'"' \
    -DPKG_TOOLCHAIN='"'"$TC"'"' \
    -DPKG_ABI='"v0"' \
    -DPKG_SDK='"'"$SDK"'"' \
    -DPKG_CC="\"$($CC -dumpversion)\"" \
    -I"$SDK/include" -I"$ZSRC" $TLS_CFLAGS -DPKG_TLS='"'"$TLS"'"' \
    "$HERE"/libpkg/*.c "$HERE"/pkg/*.c "$HERE"/../third_party/aros-xterm/aros_tty_client.c $TLS_SRC \
    "$ZOBJ/libz.a" $TLS_LIBS \
    -o "$OUT"
ls -l "$OUT"
