#!/bin/sh
# The author-to-user acceptance, host half: describe a real program's drawer
# with apkg-pack, build it, submit it into a scratch copy of arospkg-index
# under a URL that is never fetched (the guest finds the archive in its
# cache, as after a download), check that copy as a pull request would be,
# generate the catalogue, then do the same for a second revision.
#
#   tests/ux/e2e-make.sh STAGEDIR
#
# The program is gmore 1.2 from AROS Archives (the archive used by tests/ux),
# repacked with a description; nothing is published anywhere.
set -e
HERE=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$1; [ -n "$OUT" ] || { echo "usage: $0 STAGEDIR"; exit 2; }
PACK="python3 $HERE/tools/apkg-pack"
W=$(mktemp -d)
rm -rf "$OUT"; mkdir -p "$OUT"
URL=https://example.invalid/uxpack/uxpack.x86_64-aros-v11.zip

curl -fsSL -o "$W/gmore.zip" https://archives.arosworld.org/share/utility/text/gmore.x86_64-aros-v11.zip
echo "65ecebd838705d29ffdb05dbd4727aa8e9c8f49342923a73e30d60acb6427131  $W/gmore.zip" | shasum -a 256 -c - >/dev/null
mkdir -p "$W/src" && (cd "$W/src" && unzip -q ../gmore.zip)
mv "$W/src/GMore1.2" "$W/src/UxPack" && mv "$W/src/GMore1.2.info" "$W/src/UxPack.info"

$PACK init "$W/src/UxPack" --non-interactive --id uxpack --version 1.2 \
    --summary "gmore 1.2 repacked by apkg-pack (acceptance test)" --category utility/text \
    --kind app --abi v11 --requires crt.library --requires stdlib.library --no-depends \
    --note "Test note from the author: written once, in the archive's manifest." \
    --note "Read it again with: apkg show uxpack" | tee "$OUT/host-init.txt"
$PACK check "$W/src/UxPack" | tee "$OUT/host-check.txt"
$PACK build "$W/src/UxPack" --output "$W/uxpack.x86_64-aros-v11.zip" | tee "$OUT/host-build.txt"

git clone -q "$HERE/../arospkg-index" "$W/index"
$PACK submit "$URL" --index "$W/index" --use-local-copy "$W/uxpack.x86_64-aros-v11.zip" | tee "$OUT/host-submit.txt"
(cd "$W/index" && git add -A && git -c user.name=t -c user.email=t@t commit -qm submit)
python3 "$HERE/tools/ci_check_index.py" "$W/index" --base HEAD~1 --use-local-copies "$W" \
    --cache "$W/cache" --out-dir "$W/index" 2>&1 | grep -v 'warning:' | tee "$OUT/host-ci.txt"
cp "$W/index/index-v2.json" "$OUT/index-r0.json"
cp "$W/uxpack.x86_64-aros-v11.zip" "$OUT/uxpack-r0.zip"
# The same catalogue without the package: the registry must still have the notes.
python3 - "$W/index/index-v2.json" "$OUT/index-without.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); d["packages"] = [p for p in d["packages"] if p["id"] != "uxpack"]
json.dump(d, open(sys.argv[2], "w"), indent=2)
PY

# Revision 1: the author raises it, builds, submits again.
sed -i '' 's/^version         = "1.2"$/version         = "1.2"\nrevision        = 1/' "$W/src/UxPack.arospkg.toml"
echo "changed for revision 1" >> "$W/src/UxPack/Readme"
mkdir -p "$W/r1"
$PACK build "$W/src/UxPack" --output "$W/r1/uxpack.x86_64-aros-v11.zip" | tee "$OUT/host-build-r1.txt"
$PACK submit "$URL" --index "$W/index" --use-local-copy "$W/r1/uxpack.x86_64-aros-v11.zip" | tee "$OUT/host-submit-r1.txt"
(cd "$W/index" && git add -A && git -c user.name=t -c user.email=t@t commit -qm r1)
python3 "$HERE/tools/ci_check_index.py" "$W/index" --base HEAD~1 --use-local-copies "$W/r1" \
    --cache "$W/cache" --out-dir "$W/index" 2>&1 | grep -v 'warning:' | tee "$OUT/host-ci-r1.txt"
cp "$W/index/index-v2.json" "$OUT/index-r1.json"
cp "$W/r1/uxpack.x86_64-aros-v11.zip" "$OUT/uxpack-r1.zip"

OUT=$OUT/apkg sh "$HERE/src/build.sh" >/dev/null 2>&1
cp "$HERE/tests/ux/e2e.script" "$OUT/"
( cd "$OUT" && shasum -a 256 apkg uxpack-r0.zip uxpack-r1.zip index-r0.json index-r1.json )
