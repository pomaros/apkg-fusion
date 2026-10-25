#!/bin/sh
# Build apkg for AROS aarch64 (hosted Macaros or native), with the Macaros
# workspace's AROS build and clang cross tools. Not released. -ffixed-x18 is
# mandatory on this target; zlib is compiled from source because the SDK has
# no library of it.
set -e
. "$HOME/Work/AROS-dev/macaros/env.sh" >/dev/null 2>&1 || true
SDKROOT=${SDKROOT:-/Volumes/macaros/build/bin/darwin-aarch64}
XT=${XT:-/Volumes/macaros/crosstools}
DEV="$SDKROOT/AROS/Developer"
ZSRC=${ZSRC:-/Volumes/arosabiv0/pc-i386/bin/pc-i386/Ports/zlib/zlib-1.2.13}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../apkg-aarch64}
FLAGS="--target=aarch64-unknown-aros -mcmodel=large -ffixed-x18"
INC="-isystem $DEV/include -isystem $SDKROOT/gen/include -isystem $SDKROOT/gen/include/aros/posixc -isystem $DEV/include/aros/stdc"
export COMPILER_PATH="$SDKROOT/tools:$XT/bin"

CC="$XT/bin/clang $FLAGS $INC"; AR="$XT/bin/llvm-ar"; TAG=aarch64-aros
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
echo "build id $BUILD_ID  (aarch64, not released)"
$CC -O2 -std=gnu11 -Wall -Wextra \
    -DPKG_BUILD_ID='"'"$BUILD_ID"'"' -DPKG_TOOLCHAIN='"'"$XT"'"' \
    -DPKG_ABI='"v1"' -DPKG_SDK='"'"$SDKROOT"'"' -DPKG_CC='"clang"' \
    -I"$ZSRC" $TLS_CFLAGS -DPKG_TLS='"'"$TLS"'"' \
    -nostartfiles -nodefaultlibs -L"$DEV/lib" -L"$XT/lib/generic" \
    "$DEV/lib/startup.o" \
    "$HERE"/libpkg/*.c "$HERE"/pkg/*.c "$HERE"/../third_party/aros-xterm/aros_tty_client.c $TLS_SRC \
    "$ZOBJ/libz.a" $TLS_LIBS \
    -o "$OUT" \
    -Wl,--allow-multiple-definition -Wl,--start-group \
    -lnet -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros \
    -lautoinit -llibinit -lutility -lamiga -larossupport \
    -Wl,--end-group -lclang_rt.builtins-aarch64
ls -l "$OUT"
