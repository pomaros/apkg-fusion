#!/bin/sh
# The published 0.3.1-rc1 binaries for x86_64 ABIv11, checked, into DIR:
#
#   tests/rc1net/fetch-rc1.sh DIR
#
# Downloads the release archive unless DIR already holds it, and refuses to
# go on unless the archive, apkg and PkgManager have the published hashes.
set -e
DIR=$1
[ -n "$DIR" ] || { echo "usage: $0 DIR"; exit 2; }
ZIP=arospkg-0.3.1-rc1.x86_64-aros-v11.zip
URL=https://github.com/tomaszstaniak/arospkg/releases/download/v0.3.1-rc1/$ZIP
ZIP_SHA=01d5bdd904fe52a8e24c1abc17fa0cb591998bc6e672be68c7e4314c8d0f325f
APKG_SHA=e4583d6aeb20af0de62b96827cc5c9c43984f4abf5519dbcc7eb12283f2b5aae
PM_SHA=732ecd9b356c200e3436e5f3bf49240c30b5698050e9aea3c6464eed360070ff
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -c1-64; }

mkdir -p "$DIR"
[ -f "$DIR/$ZIP" ] || curl -fsSL -o "$DIR/$ZIP" "$URL"
[ "$(sha "$DIR/$ZIP")" = $ZIP_SHA ] || { echo "$ZIP: wrong SHA-256"; exit 1; }
rm -rf "$DIR/x"; mkdir "$DIR/x"
unzip -q "$DIR/$ZIP" -d "$DIR/x"
cp "$DIR/x/arospkg-0.3.1-rc1.x86_64-aros-v11/apkg" "$DIR/apkg"
cp "$DIR/x/arospkg-0.3.1-rc1.x86_64-aros-v11/PkgManager" "$DIR/PkgManager"
rm -rf "$DIR/x"
[ "$(sha "$DIR/apkg")" = $APKG_SHA ] || { echo "apkg: wrong SHA-256"; exit 1; }
[ "$(sha "$DIR/PkgManager")" = $PM_SHA ] || { echo "PkgManager: wrong SHA-256"; exit 1; }
echo "rc1 binaries checked in $DIR"
