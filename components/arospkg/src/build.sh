#!/bin/sh
# Build apkg for ABIv11 -- AROS One x86_64. THE DEFAULT BUILD.
#
# v0.1 targets ABIv11 (decided 2026-09-19): that is where the catalogue is --
# 171 ABIv11 candidates against 7 for mainline. build-mainline.sh stays as an
# additional build and does not gate a release.
#
# A second binary, not a flag: a program built for one ABI does not start on the
# other. See docs/reference/abi.md. Everything about the two builds that differs
# is here, and it is more than the compiler path:
#
#   * OpenSSL is 1.1.0h (2018) rather than 4.0.1, so the hostname check uses
#     SSL_set1_host() -- net.c picks by OPENSSL_VERSION_NUMBER.
#   * The C library is one libcrt.a rather than mainline's split
#     libposixc/libstdcio/libstdc.
#   * zlib is linked STATICALLY, as in the mainline build. The SDK's libz.a is
#     a stub that opens z1.library at startup. AROS One 1.3 does ship
#     z1.library, so the stub worked -- but a release binary with a runtime
#     library dependency nobody documented is the thing requires_system exists
#     to prevent, so the dependency is removed rather than documented.
set -e
TC=${TC:-$HOME/Work/AROS/toolchain}
SDK=${SDK:-$HOME/Work/AROS/sdk}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../apkg}

CC="$TC/x86_64-aros-gcc"; AR="$TC/x86_64-aros-ar"; TAG=abiv11-x86_64
. "$HERE/tls/tls.sh"
BUILD_ID=$(cat "$HERE"/libpkg/*.c "$HERE"/libpkg/*.h $TLS_SRC "$HERE"/pkg/*.c "$HERE"/../third_party/aros-xterm/aros_tty_client.c \
           | shasum -a 256 | cut -c1-16)
echo "build id $BUILD_ID  (ABIv11 / AROS One, default)"

"$TC/x86_64-aros-gcc" -O2 -Wall -Wextra -std=gnu99 $EXTRA_CFLAGS \
    -DPKG_BUILD_ID='"'"$BUILD_ID"'"' \
    -DPKG_TOOLCHAIN='"'"$TC"'"' \
    -DPKG_SDK='"'"$SDK"'"' \
    -DPKG_ABI='"v11"' \
    -DPKG_CC="\"$("$TC/x86_64-aros-gcc" -dumpversion)\"" \
    -I"$SDK/include" $TLS_CFLAGS -DPKG_TLS='"'"$TLS"'"' \
    "$HERE"/libpkg/*.c "$HERE"/pkg/*.c "$HERE"/../third_party/aros-xterm/aros_tty_client.c $TLS_SRC \
    "$SDK/lib/libz.static.a" $TLS_LIBS -L"$SDK/lib" \
    -o "$OUT"
ls -l "$OUT"
