#!/bin/sh
# The tests that do not need AROS. Pure logic only -- everything platform-
# dependent is verified on a guest and reported with a run id and a binary
# hash, because a host pass says nothing about AROS.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../src/libpkg
OUT=${OUT:-/tmp/arospkg-host-tests}
CC=${CC:-cc}
mkdir -p "$OUT"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_sha256" \
    "$HERE/test_sha256.c" "$SRC/sha256.c"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_req" \
    "$HERE/test_req.c" "$SRC/req.c" "$SRC/json.c" "$SRC/err.c"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_app" \
    "$HERE/test_app.c" "$SRC/app.c" "$SRC/json.c" "$SRC/req.c" "$SRC/err.c"
"$OUT/test_app"

python3 "$HERE/../tools/make-demo-store.py" "$OUT/demostore" >/dev/null
$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_linktext" \
    "$HERE/test_linktext.c" "$HERE/../src/pkgmanager/linktext.c"
"$OUT/test_linktext"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_demo" \
    "$HERE/test_demo.c" "$HERE/../src/pkgmanager/demo.c" "$SRC/json.c"
"$OUT/test_demo" "$OUT/demostore"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_icon" \
    "$HERE/test_icon.c" "$SRC/icon.c"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_listing" \
    "$HERE/test_listing.c" "$SRC/listing.c"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_entries" \
    "$HERE/test_entries.c" "$SRC/entries.c" "$SRC/json.c" "$SRC/listing.c" "$SRC/app.c" "$SRC/req.c" "$SRC/err.c"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_upgrade" \
    "$HERE/test_upgrade.c" "$SRC/upgrade.c"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_verify" \
    "$HERE/test_verify.c" "$SRC/verify.c"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_zip" \
    "$HERE/test_zip.c" "$SRC/zip.c" "$HERE/host_util.c" -lz

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_rexxcmd" \
    "$HERE/test_rexxcmd.c" "$HERE/../src/pkgmanager/rexxcmd.c" "$SRC/updates.c" "$SRC/entries.c" "$SRC/app.c" \
    "$SRC/json.c" "$SRC/listing.c" "$SRC/req.c" "$SRC/upgrade.c" "$SRC/err.c"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_release" "$HERE/test_release.c" "$SRC/release.c"
"$OUT/test_release"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_selfup" \
    "$HERE/test_selfup.c" "$SRC/selfup.c" "$SRC/json.c" "$SRC/release.c"
"$OUT/test_selfup"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_updates" \
    "$HERE/test_updates.c" "$SRC/updates.c" "$SRC/entries.c" "$SRC/app.c" "$SRC/json.c" "$SRC/listing.c" \
    "$SRC/req.c" "$SRC/upgrade.c" "$SRC/err.c"
"$OUT/test_updates" "$OUT/updates.json"
python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['ready']==3 and len(d['packages'])==12, d" "$OUT/updates.json"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_news" \
    "$HERE/test_news.c" "$SRC/news.c" "$SRC/entries.c" "$SRC/json.c" "$SRC/listing.c" "$SRC/app.c" \
    "$SRC/req.c" "$SRC/err.c"
"$OUT/test_news"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_relations" \
    "$HERE/test_relations.c" "$SRC/relations.c" "$SRC/entries.c" "$SRC/json.c" "$SRC/listing.c" "$SRC/app.c" \
    "$SRC/req.c" "$SRC/err.c" "$SRC/upgrade.c"
"$OUT/test_relations"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_hint" \
    "$HERE/test_hint.c" "$HERE/../src/pkg/hint.c"
"$OUT/test_hint"

# The JSON reader past the old 8192-value array: generated catalogues of the
# size of AROS Archives (~2000) and with headroom (5000), and its failures.
for f in json entries; do
    $CC -O1 -Wall -Wextra -std=gnu99 -Dmalloc=t_malloc -c -o "$OUT/big-$f.o" "$SRC/$f.c"
done
$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_bigindex" \
    "$HERE/test_bigindex.c" "$OUT/big-json.o" "$OUT/big-entries.o" "$SRC/listing.c" "$SRC/app.c" "$SRC/req.c" "$SRC/err.c"
"$OUT/test_bigindex" --errors
for n in 170 2000 5000; do
    python3 "$HERE/fixtures/make-big-index.py" $n "$OUT/big-$n.json"
    "$OUT/test_bigindex" "$OUT/big-$n.json" $n
done

# test_zip reads archives made here, shaped like the ones that failed on AROS
python3 - <<'PY2'
import zipfile, os
with zipfile.ZipFile('/tmp/arospkg-test.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('Game/Big', os.urandom(2 * 1024 * 1024))
    z.writestr('Game/Game', b'x' * 100)
    z.writestr('Game.info', b'I' * 64)
with zipfile.ZipFile('/tmp/arospkg-unsafe.zip', 'w') as z:
    z.writestr('Game.info', b'I' * 64)
    z.writestr('LIBS:evil.library', b'e')
PY2

python3 "$HERE/make-zip-limits.py" "$OUT/zip-limits"
$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_zip_limits" \
    "$HERE/test_zip_limits.c" "$SRC/zip.c" "$HERE/host_util.c" -lz

python3 "$HERE/make-lha-fixtures.py"

$CC -O1 -Wall -Wextra -std=gnu99 -o "$OUT/test_lha" \
    "$HERE/test_lha.c" "$SRC/arc.c" "$SRC/lha.c" "$SRC/zip.c" "$HERE/host_util.c" -lz

fail=0
for t in test_sha256 test_req test_icon test_listing test_entries test_upgrade test_verify test_zip test_lha test_rexxcmd; do
    echo "== $t"
    "$OUT/$t" || fail=1
done
echo "== test_relations.py"
python3 "$HERE/test_relations.py" || fail=1
echo "== test_review"
python3 "$HERE/test_review.py" || fail=1
for t in test_zip_limits; do
    echo "== $t"
    "$OUT/$t" "$OUT/zip-limits" || fail=1
    rm -rf "$OUT"/zip-limits/out-*
done
echo "== test_mkindex"
python3 "$HERE/test_mkindex.py" || fail=1
python3 "$HERE/test_apkg_pack.py" || fail=1
python3 "$HERE/test_ci_check.py" || fail=1
python3 "$HERE/test_publish_github.py" || fail=1
echo "== test_doc_examples"
python3 "$HERE/test_doc_examples.py" || fail=1
echo "== test_presentation"
python3 "$HERE/test_presentation.py" || fail=1
echo "== test_help"
python3 "$HERE/test_help.py" || fail=1
[ $fail -eq 0 ] && echo "ALL HOST TESTS PASS" || { echo "HOST TESTS FAILED"; exit 1; }
