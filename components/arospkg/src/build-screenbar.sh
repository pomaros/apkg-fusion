#!/bin/sh
# Build PkgUpdates, the optional ScreenBar item (src/screenbar/pkgupdates.c).
#
# The ScreenBar provider SDK is not copied into this repository: its header
# and its provider.c are compiled from a ScreenBar checkout, by default the
# public one beside this repository (override with SCREENBAR_DIR). Only the
# libpkg files the updates model needs are linked: no TLS, no network code.
#
#   sh src/build-screenbar.sh            x86_64 ABIv11 (toolchain as build.sh)
#   TARGET=raspi sh src/build-screenbar.sh   native Raspberry Pi, aarch64 ABIv1
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SCREENBAR_DIR=${SCREENBAR_DIR:-$HERE/../../aros-screenbar/aros-screenbar-public}
[ -f "$SCREENBAR_DIR/include/screenbar.h" ] && [ -f "$SCREENBAR_DIR/src/provider.c" ] || {
    echo "No ScreenBar SDK at $SCREENBAR_DIR: set SCREENBAR_DIR to a ScreenBar checkout" >&2; exit 1; }
L=$HERE/libpkg
SRC="$HERE/screenbar/pkgupdates.c $SCREENBAR_DIR/src/provider.c $L/updates.c $L/updates_io.c $L/util.c
     $L/entries.c $L/app.c $L/listing.c $L/json.c $L/req.c $L/upgrade.c $L/err.c $L/build.c $L/sha256.c $L/bigstack.c"
case "${TARGET:-abiv11}" in
abiv11)
    TC=${TC:-$HOME/Work/AROS/toolchain}; SDK=${SDK:-$HOME/Work/AROS/sdk}
    OUT=${OUT:-$HERE/../build/abiv11/PkgUpdates}
    mkdir -p "$(dirname "$OUT")"
    "$TC/x86_64-aros-gcc" -O2 -Wall -Wextra -std=gnu99 -DPKG_ABI='"v11"' \
        -I"$SDK/include" -I"$SCREENBAR_DIR/include" $SRC -o "$OUT" ;;
raspi)
    RPI=${RPI:-$HOME/Work/AROS/toolchains/raspi-aarch64}
    OUT=${OUT:-$HERE/../build/raspi-aarch64/PkgUpdates}
    mkdir -p "$(dirname "$OUT")"
    "$RPI/bin/aarch64-aros-clang" -O2 -Wall -Wextra -std=gnu99 -DPKG_ABI='"v1"' \
        -DPKG_TARGET='"aarch64-aros-raspi"' -I"$SCREENBAR_DIR/include" $SRC -o "$OUT" ;;
*) echo "TARGET is abiv11 or raspi" >&2; exit 2 ;;
esac
ls -l "$OUT"
