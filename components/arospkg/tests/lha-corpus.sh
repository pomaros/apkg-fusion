#!/bin/sh
# Extract every real LHA we hold with libpkg's reader and with the system `lha`,
# and compare every file. This is the evidence that the decompressor is right:
# the unit test covers the path rules, this covers the bit stream, against an
# independent implementation on archives nobody wrote for us.
#
#   tests/lha-corpus.sh [extra-archive ...]
#
# Needs a system `lha` (extract-only is enough) and the downloaded archives in
# .cache/archives; tools/fetch.py puts them there.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/arospkg-lha-corpus}
CC=${CC:-cc}
command -v lha >/dev/null || { echo "no system lha to compare against"; exit 2; }
mkdir -p "$OUT"
$CC -w -O1 -o "$OUT/probe" -x c - "$HERE/src/libpkg/lha.c" "$HERE/tests/host_util.c" <<'PROBE'
#include <stdio.h>
int lha_extract(const char*, const char*, const char*, long, long, void*, void*, char*);
int main(int argc, char **argv)
{
    char why[160] = "";
    int rc = lha_extract(argv[1], argv[2], "", 64L*1024*1024, 512L*1024*1024, 0, 0, why);
    if (rc) printf("%s\n", why);
    return rc != 0;
}
PROBE

pass=0; fail=0; refused=0
for a in $(find "$HERE/.cache/archives" -name '*.lha') "$@"; do
    [ -f "$a" ] || continue
    t=$(mktemp -d); mkdir -p "$t/ours" "$t/ref"
    if ! "$OUT/probe" "$a" "$t/ours" >"$t/why" 2>&1; then
        echo "refused  $(basename "$a"): $(cat "$t/why")"
        refused=$((refused+1)); rm -rf "$t"; continue
    fi
    ( cd "$t/ref" && lha xq "$a" >/dev/null 2>&1 )
    ( cd "$t/ours" && find . -type f | sort | while read -r f; do shasum -a 256 "$f"; done ) > "$t/a"
    ( cd "$t/ref"  && find . -type f | sort | while read -r f; do shasum -a 256 "$f"; done ) > "$t/b"
    if diff -q "$t/a" "$t/b" >/dev/null; then
        echo "ok       $(basename "$a"): $(wc -l < "$t/b" | tr -d ' ') files identical to the system lha"
        pass=$((pass+1))
    else
        echo "DIFFERS  $(basename "$a"):"; diff "$t/a" "$t/b" | head -5; fail=$((fail+1))
    fi
    rm -rf "$t"
done
echo
echo "$pass archive(s) identical, $fail differing, $refused refused"
[ "$fail" -eq 0 ] || exit 1
