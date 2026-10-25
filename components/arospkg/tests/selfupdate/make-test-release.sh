#!/bin/sh
# A clearly labelled test release of arospkg for the self-update tests: the
# x86_64 ABIv11 archive laid out as tools/make-rc.sh lays out a release, from
# the working tree, with the version a test needs.
#
#   tests/selfupdate/make-test-release.sh VERSION OUTDIR [BASE]
#
# VERSION must carry a "-sutest" label (0.7.0-sutest2), so a test build can
# never pass for a release. BASE, when given, is compiled in as the default
# release source (SU_TEST_BASE): a PkgManager started from its icon takes no
# arguments, so this is how one is pointed at tests/selfupdate/relserver.py.
# OUTDIR gets the archive and a SHA256SUMS, as a GitHub release has them.
set -e
V=${1:?usage: make-test-release.sh VERSION OUTDIR [BASE]}
OUT=${2:?usage: make-test-release.sh VERSION OUTDIR [BASE]}
BASE=$3
case $V in *-sutest*) ;; *) echo "test versions carry -sutest: $V"; exit 2;; esac
HERE=$(cd "$(dirname "$0")/../.." && pwd)
T=x86_64-aros-v11
B=$(mktemp -d)
FLAGS="-DAPKG_TEST_VERSION=\"\\\"$V\\\"\" -DPM_TEST_VERSION=\"\\\"$V\\\"\""
[ -n "$BASE" ] && FLAGS="$FLAGS -DSU_TEST_BASE=\"\\\"$BASE\\\"\""
D="$B/arospkg-$V.$T"; mkdir -p "$D/licenses" "$OUT"
EXTRA_CFLAGS=$(eval echo "$FLAGS") OUT="$D/apkg" sh "$HERE/src/build.sh" >/dev/null 2>&1
EXTRA_CFLAGS=$(eval echo "$FLAGS") OUT="$D/PkgManager" sh "$HERE/src/build-gui.sh" >/dev/null 2>&1
strings -a "$D/apkg" | grep -q "^\$VER: apkg $V " || { echo "apkg does not say $V"; exit 1; }
strings -a "$D/PkgManager" | grep -q "^\$VER: PkgManager $V " || { echo "PkgManager does not say $V"; exit 1; }
cp "$HERE/release/README-RC.txt" "$D/README.txt"
cp "$HERE/release/LICENSE" "$D/"
cp "$HERE/release/icons/PkgManager.info" "$D/"
python3 "$HERE/tools/make-demo-store.py" "$D/Demo" >/dev/null
python3 -c "import json,sys; sys.path.insert(0,sys.argv[1]); import mkindex
m={'id':'arospkg','version':sys.argv[2],'arch':'x86_64','abi':'v11',
   'requires_system':[{'type':'library','id':l} for l in ('crt.library','stdlib.library','muimaster.library')],
   'launch':{'executable':'PkgManager','mode':'workbench'}}
open(sys.argv[3],'w').write(json.dumps(mkindex.app_description(m), indent=2)+'\n')" "$HERE/tools" "$V" "$D/application.json"
{ echo "arospkg $V, target $T (TEST BUILD, not a release)"; echo "commit $(git -C "$HERE" rev-parse HEAD) + working tree"; echo;
  (cd "$D" && shasum -a 256 apkg PkgManager); } > "$D/BUILD.txt"
rm -f "$OUT/arospkg-$V.$T.zip"
(cd "$B" && zip -qr "$OUT/arospkg-$V.$T.zip" "arospkg-$V.$T")
(cd "$OUT" && shasum -a 256 arospkg-*.zip > SHA256SUMS && cat SHA256SUMS)
(cd "$D" && shasum -a 256 apkg PkgManager PkgManager.info BUILD.txt application.json)
rm -rf "$B"
