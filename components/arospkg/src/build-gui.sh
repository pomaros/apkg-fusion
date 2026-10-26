#!/bin/sh
# Build PkgManager, the Zune front end, for ABIv11 -- AROS One x86_64.
# Same toolchain, SDK and libraries as build.sh; libpkg is compiled in, so the
# window and the Shell client are the same code with a different front.
set -e
TC=${TC:-$HOME/Work/AROS/toolchain}
SDK=${SDK:-$HOME/Work/AROS/sdk}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../PkgManager}

CC="$TC/x86_64-aros-gcc"; AR="$TC/x86_64-aros-ar"; TAG=abiv11-x86_64
. "$HERE/tls/tls.sh"
# APKG_FUSION opts into the common context install path. The imported default
# remains buildable independently; no host-specific SDK paths are embedded.
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
           | shasum -a 256 | cut -c1-16)
echo "build id $BUILD_ID  (PkgManager, ABIv11 / AROS One)"

"$TC/x86_64-aros-gcc" -O2 -Wall -Wextra -std=gnu99 $EXTRA_CFLAGS $FUSION_FLAGS \
    -DPKG_BUILD_ID='"'"$BUILD_ID"'"' \
    -DPKG_TOOLCHAIN='"'"$TC"'"' \
    -DPKG_SDK='"'"$SDK"'"' \
    -DPKG_ABI='"v11"' \
    -DPKG_CC="\"$("$TC/x86_64-aros-gcc" -dumpversion)\"" \
    -I"$SDK/include" $TLS_CFLAGS -DPKG_TLS='"'"$TLS"'"' \
    "$HERE"/libpkg/*.c "$HERE"/pkgmanager/*.c "$@" $TLS_SRC \
    "$SDK/lib/libz.static.a" $TLS_LIBS -L"$SDK/lib" \
    -o "$OUT"
ls -l "$OUT"
