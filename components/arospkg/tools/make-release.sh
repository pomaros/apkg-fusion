#!/bin/sh
# Stage the release archive (v0.3: apkg, PkgManager and the ARexx examples). Nothing here is uploaded: publication is a
# separate decision. Everything it puts in the archive is either built by
# src/build.sh or checked in under release/.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
VER=${VER:-0.3}
OUT=$HERE/dist
NAME=arospkg.x86_64-aros-v11
STAGE=$OUT/$NAME

[ -f "$HERE/apkg" ] || { echo "build it first: src/build.sh"; exit 1; }
[ -f "$HERE/PkgManager" ] || { echo "build it first: src/build-gui.sh"; exit 1; }
# Both binaries must be the ABIv11 ones: a mainline build would install here
# unnoticed and then not start on the machine this release targets.
for b in apkg PkgManager; do
  case "$("$HERE/tools/abi-of.sh" "$HERE/$b" 2>/dev/null || echo unknown)" in
    v11) ;;
    *) echo "$b is not an ABIv11 build"; exit 1 ;;
  esac
done

rm -rf "$STAGE"; mkdir -p "$STAGE/licenses"
cp "$HERE/apkg" "$STAGE/apkg"
cp "$HERE/PkgManager" "$STAGE/PkgManager"
cp "$HERE/release/README" "$HERE/release/LICENSE" "$STAGE/"
cp "$HERE/release"/licenses/* "$STAGE/licenses/"
mkdir -p "$STAGE/Examples/ARexx"
cp "$HERE"/examples/arexx/*.rexx "$STAGE/Examples/ARexx/"

# SHA256SUMS last, over everything else, and it names the archive's own layout.
( cd "$STAGE" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort |
  while read -r f; do shasum -a 256 "$f"; done > SHA256SUMS )

( cd "$OUT" && rm -f "$NAME.zip" && zip -q -r "$NAME.zip" "$NAME" )
echo "archive $OUT/$NAME.zip"
shasum -a 256 "$OUT/$NAME.zip"
echo
echo "contents:"; ( cd "$OUT" && unzip -l "$NAME.zip" | sed -n '4,$p' | head -12 )
echo
echo "the binaries in the archive:"; shasum -a 256 "$STAGE/apkg" "$STAGE/PkgManager"
