#!/bin/sh
# Package apkg and PkgManager for native AROS on the Raspberry Pi
# (aarch64-aros-raspi, ABIv1) into one local archive under dist/raspi-gui/.
# A sibling of make-rc.sh's raspi target, which ships apkg only. Nothing is
# uploaded.   tools/make-raspi-gui.sh 0.4.0-pigui1
set -e
L=${1:?usage: make-raspi-gui.sh LABEL}
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=aarch64-aros-raspi
OUT="$HERE/dist/raspi-gui/$L"; B="$HERE/build/raspi-gui/$L"
[ -z "$(git -C "$HERE" status --porcelain -- src third_party release tools/make-raspi-gui.sh)" ] || { echo "uncommitted changes in sources"; exit 1; }
COMMIT=$(git -C "$HERE" rev-parse HEAD)
rm -rf "$OUT" "$B"; mkdir -p "$OUT" "$B"
D="$B/arospkg-$L.$T"; mkdir -p "$D/licenses"
OUT=$D/apkg       sh "$HERE/src/build-raspi.sh"     > "$B/build-apkg.log" 2>&1
OUT=$D/PkgManager sh "$HERE/src/build-gui-raspi.sh" > "$B/build-gui.log" 2>&1
cp "$HERE/release/icons/PkgManager.info" "$D/"
cp "$HERE/release/README-raspi-gui.txt" "$D/README.txt"
cp "$HERE/release/LICENSE" "$D/"
cp "$HERE/release/licenses/zlib-LICENSE" "$D/licenses/"
cp "$HERE/third_party/mbedtls/LICENSE" "$D/licenses/MbedTLS-3.6.7-LICENSE"
cp "$HERE/third_party/aros-xterm/LICENSE" "$D/licenses/aros-xterm-client-LICENSE"
{ echo "arospkg $L, target $T (local build, not a release)"; echo "commit $COMMIT"; } > "$D/BUILD.txt"
(cd "$D" && shasum -a 256 apkg PkgManager PkgManager.info README.txt > SHA256SUMS)
(cd "$B" && zip -qrX "$OUT/arospkg-$L.$T.zip" "arospkg-$L.$T")
(cd "$OUT" && shasum -a 256 *.zip > SHA256SUMS && cat SHA256SUMS && ls -l)
cat "$D/SHA256SUMS"; echo "commit $COMMIT"
