#!/bin/sh
# Build and package a release candidate for every target from the current
# commit, one archive per CPU/ABI, into dist/rc/. Nothing is uploaded.
#   tools/make-rc.sh 0.3.1-rc1
# A release proper uses the full README: README=release/README tools/make-rc.sh 0.3.1
# TARGETS limits it to some targets: TARGETS=x86_64-aros-v11 tools/make-rc.sh 0.4.0
# Targets with PkgManager (x86_64-aros-v11, x86_64-aros-v1, aarch64-aros-raspi)
# also get its icon, the demonstration storefront (Demo/, built by
# tools/make-demo-store.py) and application.json, so the drawer starts
# PkgManager with `apkg run` even when it was unpacked by hand.
set -e
V=${1:?usage: make-rc.sh VERSION}
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT="$HERE/dist/rc/$V"; B="$HERE/build/rc/$V"
[ -z "$(git -C "$HERE" status --porcelain -- src third_party)" ] || { echo "src/ or third_party/ has uncommitted changes"; exit 1; }
COMMIT=$(git -C "$HERE" rev-parse HEAD)
rm -rf "$OUT" "$B"; mkdir -p "$OUT" "$B"
grep -q "\"apkg $V\"" "$HERE/src/pkg/main.c" || { echo "src/pkg/main.c does not say apkg $V"; exit 1; }

OUT_=$B/x86_64-aros-v11; mkdir -p $OUT_
OUT=$OUT_/apkg sh "$HERE/src/build.sh" >/dev/null 2>&1
OUT=$OUT_/PkgManager sh "$HERE/src/build-gui.sh" >/dev/null 2>&1
TARGETS=${TARGETS:-x86_64-aros-v11 x86_64-aros-v1 i386-aros-v0 aarch64-aros}
want() { case " $TARGETS " in *" $1 "*) return 0;; esac; return 1; }
want x86_64-aros-v1 && { mkdir -p $B/x86_64-aros-v1; OUT=$B/x86_64-aros-v1/apkg sh "$HERE/src/build-mainline.sh" >/dev/null 2>&1; }
want i386-aros-v0   && { mkdir -p $B/i386-aros-v0;   OUT=$B/i386-aros-v0/apkg   sh "$HERE/src/build-i386.sh"     >/dev/null 2>&1; }
want aarch64-aros   && { mkdir -p $B/aarch64-aros;   OUT=$B/aarch64-aros/apkg   sh "$HERE/src/build-aarch64.sh"  >/dev/null 2>&1; }
want aarch64-aros-raspi && { mkdir -p $B/aarch64-aros-raspi; OUT=$B/aarch64-aros-raspi/apkg sh "$HERE/src/build-raspi.sh" >/dev/null 2>&1;
                             OUT=$B/aarch64-aros-raspi/PkgManager sh "$HERE/src/build-gui-raspi.sh" >/dev/null 2>&1; }
want x86_64-aros-v1 && { OUT=$B/x86_64-aros-v1/PkgManager sh "$HERE/src/build-gui-mainline.sh" >/dev/null 2>&1; }
python3 "$HERE/tools/make-demo-store.py" "$B/Demo" >/dev/null

for t in $TARGETS; do
    D="$B/pkg/arospkg-$V.$t"; mkdir -p "$D/licenses"
    cp "$B/$t"/* "$D/"
    cp "$HERE/${README:-release/README-RC.txt}" "$D/README.txt"
    cp "$HERE/release/LICENSE" "$D/"
    cp "$HERE/release/licenses/zlib-LICENSE" "$D/licenses/"
    cp "$HERE/third_party/mbedtls/LICENSE" "$D/licenses/MbedTLS-3.6.7-LICENSE"
    cp "$HERE/third_party/aros-xterm/LICENSE" "$D/licenses/aros-xterm-client-LICENSE"
    # jitterentropy is in the builds without a system getentropy()
    if [ "$t" != x86_64-aros-v1 ] && [ "$t" != aarch64-aros-raspi ]; then
        cp "$HERE/third_party/jitterentropy/LICENSE" "$D/licenses/jitterentropy-3.7.0-LICENSE"
        cp "$HERE/third_party/jitterentropy/LICENSE.bsd" "$D/licenses/jitterentropy-3.7.0-LICENSE.bsd"
    fi
    [ "$t" = x86_64-aros-v11 ] && mkdir -p "$D/Examples/ARexx" && cp "$HERE"/examples/arexx/*.rexx "$D/Examples/ARexx/"
    if [ -f "$D/PkgManager" ]; then
        cp "$HERE/release/icons/PkgManager.info" "$D/"
        cp -R "$B/Demo" "$D/Demo"
        # The libraries the catalogue entry names for this target.
        case $t in x86_64-aros-v11) a=x86_64 abi=v11 libs=crt.library,stdlib.library,muimaster.library;;
                   x86_64-aros-v1)  a=x86_64 abi=v1  libs=muimaster.library;;
                   *)               a=aarch64 abi=v1 libs=muimaster.library;; esac
        python3 -c "import json,sys; sys.path.insert(0,sys.argv[1]); import mkindex
m={'id':'arospkg','version':sys.argv[2],'arch':sys.argv[3],'abi':sys.argv[4],
   'requires_system':[{'type':'library','id':l} for l in sys.argv[6].split(',')],
   'launch':{'executable':'PkgManager','mode':'workbench'}}
open(sys.argv[5],'w').write(json.dumps(mkindex.app_description(m), indent=2)+'\n')" \
            "$HERE/tools" "$V" "$a" "$abi" "$D/application.json" "$libs"
    fi
    { echo "arospkg $V, target $t"; echo "commit $COMMIT"; echo;
      (cd "$D" && for f in apkg PkgManager; do [ -f "$f" ] && shasum -a 256 "$f"; done; true); } > "$D/BUILD.txt"
    (cd "$B/pkg" && zip -qr "$OUT/arospkg-$V.$t.zip" "arospkg-$V.$t")
done
(cd "$OUT" && shasum -a 256 *.zip > SHA256SUMS && cat SHA256SUMS)
for t in $TARGETS; do
    (cd "$B/pkg/arospkg-$V.$t" && echo "$t:" && for f in apkg PkgManager; do [ -f "$f" ] && shasum -a 256 "$f"; done; true)
done
echo "commit $COMMIT"
