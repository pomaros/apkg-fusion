#!/bin/sh
# Stage for run-on-pool.sh: apkg and PkgManager from BINDIR (default the
# repository root), the guest scripts. Writes STAGEDIR.sha256.
#
#   tests/netbreak/make-stage.sh STAGEDIR
set -e
HERE=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$1; BIN=${BINDIR:-$HERE}
[ -n "$OUT" ] || { echo "usage: $0 STAGEDIR"; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$BIN/apkg" "$BIN/PkgManager" "$HERE/tests/netbreak/netbreak.rexx" "$HERE/tests/netbreak/setup.script" "$OUT/"
( cd "$OUT" && shasum -a 256 * ) > "$OUT.sha256"
cat "$OUT.sha256"
