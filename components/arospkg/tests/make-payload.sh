#!/bin/sh
# Assemble the payload CD for the whole evidence set -- the v0.1 suites and the
# v0.2 ones -- on ONE pair of binaries, and burn the ISO. Every archive on it
# is either the published one (checked against the committed index) or made by
# a generator under tests/; nothing is hand-edited.
#
#     tests/make-payload.sh OUTDIR [ISO]
#
# Layout (AROS One reads at most 12 entries per directory off this ISO, so the
# tree is split):
#   APKG PKGMGR PUTFILE OLDPKG      the binaries; OLDPKG is the 2026-09-12 apkg
#   A/   archives                   I/  indexes
#   R/   v0.1 scripts               S/  v0.2 scripts
# OLDPKG is rebuilt from commit 87708fc unless $OLDPKG names a file; either way
# it must hash to the binary in that day's run reports.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1; ISO=$2
[ -n "$OUT" ] || { echo "usage: $0 OUTDIR [ISO]"; exit 2; }
for b in apkg PkgManager; do [ -f "$HERE/$b" ] || { echo "build $b first"; exit 1; }; done
# RELZIP: the release archive ACCEPT tests (default: the 0.3-style dist
# archive); RELDIR: the drawer it unpacks to. ACCEPT checks both hashes first.
RELZIP=${RELZIP:-$HERE/dist/arospkg.x86_64-aros-v11.zip}
RELDIR=${RELDIR:-arospkg.x86_64-aros-v11}
[ -f "$RELZIP" ] || { echo "no release archive $RELZIP"; exit 1; }
RELSHA=$(shasum -a 256 "$RELZIP" | cut -c1-64)
APKGSHA=$(unzip -p "$RELZIP" "$RELDIR/apkg" | shasum -a 256 | cut -c1-64)
[ "$APKGSHA" = "$(shasum -a 256 "$HERE/apkg" | cut -c1-64)" ] || { echo "the apkg in $RELZIP is not ./apkg"; exit 1; }
CACHE=$HERE/.cache/archives
OLD_SHA=37b2774a16afcd950e82ef29dbcb0d2fb994125406b1db0d8071ca69d14a7598

rm -rf "$OUT"; mkdir -p "$OUT/A" "$OUT/I" "$OUT/R" "$OUT/S"
cp "$HERE/apkg" "$OUT/APKG"; cp "$HERE/PkgManager" "$OUT/PKGMGR"; cp "$HERE/tests/tools/putfile" "$OUT/PUTFILE"

if [ -z "$OLDPKG" ]; then
  WT=$(mktemp -d "${TMPDIR:-/tmp}/oldpkg.XXXXXX")
  git -C "$HERE" worktree add -q --detach "$WT/src" 87708fc
  OUT_BIN="$WT/pkg" sh -c "cd '$WT/src' && OUT='$WT/pkg' sh src/build-one.sh" >/dev/null
  OLDPKG=$WT/pkg
fi
cp "$OLDPKG" "$OUT/OLDPKG"
got=$(shasum -a 256 "$OUT/OLDPKG" | cut -c1-64)
case "$got" in "$OLD_SHA"*) ;; *) echo "OLDPKG is $got, not the 2026-09-12 binary"; exit 1;; esac
[ -n "$WT" ] && { git -C "$HERE" worktree remove --force "$WT/src"; rm -rf "$WT"; }

# archives from AROS Archives, as the committed indexes pin them
pick() { # id index dest
  f=$(python3 -c "
import json,sys; p=[e for e in json.load(open('$2'))['packages'] if e['id']=='$1'][0]
print(p['url'].split('/share/',1)[1], p['sha256'])")
  set -- "$3" $f
  cp "$CACHE/$2" "$1"
  case "$(shasum -a 256 "$1" | cut -c1-64)" in "$3") ;; *) echo "$1 does not match the index"; exit 1;; esac
}
pick cls     "$HERE/tests/release-index.json" "$OUT/A/CLSZIP"
pick zaphod  "$HERE/tests/release-index.json" "$OUT/A/ZAPZIP"
cp "$CACHE/micropolis.x86_64-aros-v11.lha" "$OUT/A/MICROLHA"
cp "$RELZIP" "$OUT/A/RELZIP"

# generated fixtures
FX=$(mktemp -d "${TMPDIR:-/tmp}/pkgfx.XXXXXX")
python3 "$HERE/tests/make-upgrade-fixtures.py" "$FX/up" >/dev/null
python3 "$HERE/tests/make-real-upgrade-fixture.py" "$HERE" "$FX/sol" >/dev/null
cp "$FX/up/up-r1.zip" "$OUT/A/UPR1"; cp "$FX/up/up-r2.zip" "$OUT/A/UPR2"
pick soliton "$HERE/tests/release-index.json" "$OUT/A/SOLR1"
cp "$FX/sol/soliton-r2.zip" "$OUT/A/SOLR2"
cp "$FX/up/idx-r1.json" "$OUT/I/IDX1"; cp "$FX/up/idx-r2.json" "$OUT/I/IDX2"; cp "$FX/up/idx-v2.json" "$OUT/I/IDXV2"
cp "$FX/up/idx-vdown.json" "$OUT/I/IDXVDN"; cp "$FX/up/idx-veq.json" "$OUT/I/IDXVEQ"; cp "$FX/up/idx-vbad.json" "$OUT/I/IDXVBD"
cp "$FX/sol/idx-sol1.json" "$OUT/I/IDXS1"; cp "$FX/sol/idx-sol2.json" "$OUT/I/IDXS2"
cp "$HERE/tests/release-index.json" "$OUT/I/RELIDX"
cp "$HERE/tests/v01-candidates.json" "$OUT/I/CANDIDX"
cp "$HERE/tests/lha-index.json" "$OUT/I/LHAIDX"
cp "$FX/up/UPGRADE" "$OUT/S/UPGRADE"; cp "$FX/up/VERIFY" "$OUT/S/VERIFY"; cp "$FX/sol/SOLUP" "$OUT/S/SOLUP"
rm -rf "$FX"

T=$HERE/tests
cp "$T/gui-upgrade-launch.script" "$OUT/S/GUI"; cp "$T/gui-launch.script" "$OUT/S/GUI1"
cp "$T/json-progress.script" "$OUT/S/JSONTEST"
cp "$T/release-1.script" "$OUT/R/REL1"; cp "$T/release-1g.script" "$OUT/R/REL1G"; cp "$T/release-2.script" "$OUT/R/REL2"
for x in a b c d; do cp "$T/trial-$x.script" "$OUT/R/TRIAL$(echo $x | tr a-d A-D)"; done
sed -e "s/@RELSHA@/$RELSHA/; s/@APKGSHA@/$APKGSHA/; s/@RELDIR@/$RELDIR/g" "$T/acceptance.script" > "$OUT/R/ACCEPT"; cp "$T/lha-on-aros.script" "$OUT/R/LHATEST"

for d in "$OUT" "$OUT"/A "$OUT"/I "$OUT"/R "$OUT"/S; do
  n=$(ls "$d" | wc -l); [ "$n" -le 12 ] || { echo "$d has $n entries, more than the 12 the guest reads"; exit 1; }
done
( cd "$OUT" && find . -type f | sed 's|^\./||' | sort | while read -r f; do shasum -a 256 "$f"; done ) > "$OUT.sha256"
echo "payload in $OUT ($(cat "$OUT.sha256" | wc -l | tr -d ' ') files); binaries:"
grep -E " (APKG|PKGMGR|OLDPKG)$" "$OUT.sha256"
if [ -n "$ISO" ]; then
  rm -f "$ISO"; hdiutil makehybrid -quiet -iso -default-volume-name PAYLOAD -o "$ISO" "$OUT"
  echo "iso $ISO"
fi
