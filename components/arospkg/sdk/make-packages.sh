#!/bin/sh
# Build the developer packages (static libraries) for AROS x86_64 ABIv11:
#   MbedTLS-3.6.7-aros1.x86_64-aros-v11.zip
#   JitterEntropy-3.7.0-aros1.x86_64-aros-v11.zip
# from third_party/ and src/tls/ of this repository, with the toolchain and
# SDK given by TC and SDK. Output in dist/sdk/. Nothing is uploaded.
set -e
TC=${TC:-$HOME/Work/AROS/toolchain}
SDK=${SDK:-$HOME/Work/AROS/sdk}
HERE=$(cd "$(dirname "$0")/.." && pwd)
CC="$TC/x86_64-aros-gcc"; AR="$TC/x86_64-aros-ar"
MB="$HERE/third_party/mbedtls"; JE="$HERE/third_party/jitterentropy"
OUT="$HERE/dist/sdk"; W="$HERE/build/sdk"
MBV=3.6.7; JEV=3.7.0; REV=aros2; TAG=x86_64-aros-v11
# What BUILD.txt says about the compiler: where it came from, not where it
# happens to be installed on the machine that built the packages.
TC_ID=${TC_ID:-"x86_64-aros cross toolchain, make crosstools of deadwood2/AROS 5376f09bc1ed (2026-08-17; contained in tags ABIv11_20250418-1-U4 and ABIv11_2026.09)"}
# BUILD.txt names the commit the packages were built from, so that commit has
# to be the whole story: an earlier revision was packed from uncommitted
# files and named a commit that did not contain them.
if [ -n "$(git -C "$HERE" status --porcelain -- sdk src/tls third_party)" ]; then
    echo "sdk/, src/tls/ or third_party/ has uncommitted changes; commit them first"; exit 1
fi
MBD="MbedTLS-$MBV-$REV"; JED="JitterEntropy-$JEV-$REV"
rm -rf "$W"; mkdir -p "$W/obj" "$OUT"

# --- Mbed TLS: the three upstream libraries, split as upstream's CMake does
mkdir -p "$W/$MBD/lib" "$W/$MBD/include" "$W/$MBD/examples" "$W/$MBD/src"
cp -R "$MB/include/mbedtls" "$MB/include/psa" "$W/$MBD/include/"
cp "$HERE/src/tls/mbedtls_config.h" "$W/$MBD/include/mbedtls_aros_config.h"
CFG='-DMBEDTLS_CONFIG_FILE=<mbedtls_aros_config.h>'
for part in crypto x509 tls; do
    mkdir -p "$W/obj/$part"
    for c in $(sed -n "s/^src_$part //p" "$HERE/sdk/mbedtls-sources.txt"); do
        $CC -O2 -std=gnu99 -w "$CFG" -DMBEDTLS_TEST_SW_INET_PTON \
            -I"$W/$MBD/include" -I"$MB/library" -I"$SDK/include" \
            -c "$MB/library/$c" -o "$W/obj/$part/${c%.c}.o"
    done
    "$AR" rcs "$W/$MBD/lib/libmbed$part.a" "$W/obj/$part"/*.o
done
# the AROS platform functions the configuration asks for (time)
$CC -O2 -std=gnu99 -Wall "$CFG" -I"$W/$MBD/include" -I"$SDK/include" \
    -c "$HERE/src/tls/platform_aros.c" -o "$W/obj/platform_aros.o"
"$AR" rcs "$W/$MBD/lib/libmbedtls_aros.a" "$W/obj/platform_aros.o"
cp "$MB/LICENSE" "$W/$MBD/LICENSE"
cp "$HERE/LICENSE" "$W/$MBD/LICENSE.port"
cp "$HERE/src/tls/platform_aros.c" "$HERE/sdk/mbedtls-sources.txt" "$HERE/sdk/make-packages.sh" "$W/$MBD/src/"
cp "$HERE/sdk/examples/https_get.c" "$W/$MBD/examples/"
cp "$HERE/sdk/README-mbedtls.txt" "$W/$MBD/README.txt"

# --- jitterentropy: the library (-O0, as upstream requires) and the adapter
mkdir -p "$W/$JED/lib" "$W/$JED/include" "$W/$JED/examples" "$W/$JED/src" "$W/obj/jent"
cp "$JE/jitterentropy.h" "$HERE/src/tls/jent-aros/jitterentropy-base-user.h" "$W/$JED/include/"
for c in "$JE"/src/*.c; do
    $CC -O0 -std=gnu99 -fwrapv -w -I"$W/$JED/include" -I"$JE/src" -I"$SDK/include" \
        -c "$c" -o "$W/obj/jent/$(basename "$c" .c).o"
done
"$AR" rcs "$W/$JED/lib/libjitterentropy.a" "$W/obj/jent"/*.o
$CC -O2 -std=gnu99 -Wall -I"$W/$JED/include" -I"$SDK/include" \
    -c "$HERE/src/tls/entropy_jent.c" -o "$W/obj/entropy_jent.o"
"$AR" rcs "$W/$JED/lib/libjitterentropy_mbedtls.a" "$W/obj/entropy_jent.o"
cp "$JE/LICENSE" "$JE/LICENSE.bsd" "$JE/LICENSE.gplv2" "$W/$JED/"
cp "$HERE/LICENSE" "$W/$JED/LICENSE.port"
cp "$HERE/src/tls/entropy_jent.c" "$HERE/src/tls/jent-aros/jitterentropy-base-user.h" "$HERE/sdk/make-packages.sh" "$W/$JED/src/"
cp "$HERE/sdk/examples/jent_read.c" "$W/$JED/examples/"
cp "$HERE/sdk/README-jitterentropy.txt" "$W/$JED/README.txt"

# --- identity of the build, written into both packages
for d in "$MBD" "$JED"; do
    {
        echo "target      x86_64, AROS ABIv11 (AROS One and other current distributions)"
        echo "compiler    GCC $($CC -dumpversion), $TC_ID"
        echo "c library   SDK lib/libcrt.a, SHA-256 $(shasum -a 256 "$SDK/lib/libcrt.a" | cut -c1-64)"
        echo "port source https://github.com/tomaszstaniak/arospkg, commit $(git -C "$HERE" rev-parse HEAD)"
        echo "built       $(date -u +%Y-%m-%dT%H:%MZ)"
        echo
        (cd "$W/$d" && find lib -type f -exec shasum -a 256 {} \;)
    } > "$W/$d/BUILD.txt"
done
(cd "$W" && rm -f "$OUT/$MBD.$TAG.zip" "$OUT/$JED.$TAG.zip" && \
    zip -qr "$OUT/$MBD.$TAG.zip" "$MBD" && zip -qr "$OUT/$JED.$TAG.zip" "$JED")
(cd "$OUT" && shasum -a 256 "$MBD.$TAG.zip" "$JED.$TAG.zip")
