#!/bin/sh
# Stage for the install-to-folder acceptance (tests/ux/redirected.script,
# then Limpet and PkgManager by hand or by a driver):
#
#   tests/ux/make-stage.sh STAGEDIR LIMPETDIR
#
# Builds apkg and PkgManager from this tree, an apkg that calls itself
# 0.3.1-sutest (for one self-update success), takes the published 0.3.2 apkg
# (an older client reading an index with notes) and the test index made by
# tests/fixtures/make-ux-index.py. LIMPETDIR is an unpacked Limpet drawer.
# Writes STAGEDIR.sha256.
set -e
HERE=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$1; LIMPET=$2
[ -n "$LIMPET" ] || { echo "usage: $0 STAGEDIR LIMPETDIR"; exit 2; }
TC=${TC:-$HOME/Work/AROS/toolchain}
rm -rf "$OUT"; mkdir -p "$OUT"
OUT=$OUT/apkg sh "$HERE/src/build.sh" >/dev/null 2>&1
OUT=$OUT/PkgManager sh "$HERE/src/build-gui.sh" >/dev/null 2>&1
W=$(mktemp -d); for f in "$TC"/x86_64-aros-*; do ln -s "$f" "$W/"; done; rm "$W/x86_64-aros-gcc"
printf '#!/bin/sh\nexec %s %s "$@"\n' "$TC/x86_64-aros-gcc" "'-DAPKG_TEST_VERSION=\"0.3.1-sutest\"'" > "$W/x86_64-aros-gcc"; chmod +x "$W/x86_64-aros-gcc"
TC=$W OUT=$OUT/apkg-old sh "$HERE/src/build.sh" >/dev/null 2>&1
curl -fsSL -o "$W/r.zip" https://github.com/tomaszstaniak/arospkg/releases/download/v0.3.2/arospkg-0.3.2.x86_64-aros-v11.zip
unzip -p "$W/r.zip" arospkg-0.3.2.x86_64-aros-v11/apkg > "$OUT/apkg-032"
python3 "$HERE/tests/fixtures/make-ux-index.py" "$OUT/ux-index.json"
cp -R "$LIMPET" "$OUT/Limpet"
cp "$HERE/tests/ux/redirected.script" "$OUT/"
( cd "$OUT" && find . -type f | sort | xargs shasum -a 256 ) > "$OUT.sha256"
grep -E ' \./(apkg|apkg-old|apkg-032|PkgManager|ux-index.json|Limpet/Limpet)$' "$OUT.sha256"
