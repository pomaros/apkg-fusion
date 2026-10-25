#!/bin/sh
# Assemble what run-on-pool.sh stages: the published rc1 apkg and
# PkgManager (fetch-rc1.sh), the fault-injection PkgManager from bin/, the
# TLS test index, the guest scripts. Writes STAGEDIR.sha256.
#
#   tests/rc1net/make-stage.sh STAGEDIR
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1
[ -n "$OUT" ] || { echo "usage: $0 STAGEDIR"; exit 2; }
EF_SHA=f81b0e9c1ea3b622d3c1b5cef3fbe9448755b027f446c0d72c39bfcac9a4bba5
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -c1-64; }

rm -rf "$OUT"; mkdir -p "$OUT"
sh "$HERE/fetch-rc1.sh" "$OUT.rc1" >/dev/null
cp "$OUT.rc1/apkg" "$OUT.rc1/PkgManager" "$OUT/"
[ "$(sha "$HERE/bin/PkgManager-entropyfail")" = $EF_SHA ] || { echo "PkgManager-entropyfail: wrong SHA-256"; exit 1; }
cp "$HERE/bin/PkgManager-entropyfail" "$OUT/"
python3 "$HERE/make-tls-index.py" "$OUT/TLSIDX"
cp "$HERE/nettest.rexx" "$HERE/setup.script" "$OUT/"
( cd "$OUT" && for f in *; do echo "$(sha "$f")  $f"; done ) > "$OUT.sha256"
cat "$OUT.sha256"
