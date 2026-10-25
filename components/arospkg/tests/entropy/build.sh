#!/bin/sh
# Build the jitterentropy probe for ABIv11 x86_64, with the same library and
# flags as apkg (src/tls/tls.sh).
set -e
TC=${TC:-$HOME/Work/AROS/toolchain}; SDK=${SDK:-$HOME/Work/AROS/sdk}
HERE=$(cd "$(dirname "$0")/../../src" && pwd)
CC=${CC:-$TC/x86_64-aros-gcc}; AR=${AR:-$TC/x86_64-aros-ar}; TAG=${TAG:-abiv11-x86_64}; ENTROPY=jent
. "$HERE/tls/tls.sh"
$CC -O2 -std=gnu99 -Wall -I"$SDK/include" $TLS_CFLAGS \
    "$HERE/../tests/entropy/jent-probe.c" "$HERE/tls/entropy_jent.c" $TLS_LIBS -L"$SDK/lib" \
    -o "${OUT:-$HERE/../build/jent-probe}"
