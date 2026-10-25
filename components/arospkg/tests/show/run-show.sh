#!/bin/sh
# `apkg show` on a reserved pool machine:
#
#   tests/show/run-show.sh RUNID RESULTSDIR SHOTDIR
#
# Builds its own stage from the binaries as built. The guest decides PASS and
# FAIL (showtest.rexx); grade with tests/show/grade-show.sh afterwards.
set -e
R=$1; OUT=$2; SHOTS=$3
[ -n "$SHOTS" ] || { echo "usage: $0 RUNID RESULTSDIR SHOTDIR"; exit 2; }
HERE=$(cd "$(dirname "$0")/../.." && pwd)
STAGE=$SHOTS/stage
rm -rf "$STAGE"; mkdir -p "$STAGE"
python3 "$HERE/tests/show/make-show-fixtures.py" "$HERE" "$STAGE" >/dev/null
cp "$HERE/apkg" "$HERE/PkgManager" "$HERE/tests/show/showtest.rexx" "$HERE/tests/show/setup.script" "$STAGE/"
( cd "$STAGE" && shasum -a 256 * ) > "$SHOTS/stage.sha256"
. "$HERE/tests/arexx/pool-lib.sh"
boot_staged "$STAGE"
t "Execute RAM:rxt/setup.script" 10
t "Run >NIL: RAM:rxt/PkgManager --root SYS:PkgEmpty --log RAM:show-empty.log" 12
t "rx RAM:rxt/showtest.rexx $R noindex" 25
t "rx RAM:rxt/showtest.rexx $R cli" 60
t "Run >NIL: RAM:rxt/PkgManager --root SYS:PkgShow --log RAM:show-pm.log" 12
t "rx RAM:rxt/showtest.rexx $R rexx" 20
shot show-panel
t "Copy RAM:show-pm.log RESULTS:$R-show-pm.log" 8
t "Copy RAM:show-empty.log RESULTS:$R-show-empty.log" 8
# For the EmuV0 record (docs/spikes/emuv0): the system's emulator and the
# 32-bit libraries it would load, read without starting it.
t "Copy C:EmuV0 RESULTS:$R-emuv0-110.bin" 8
t "List LIBSV0: FILES LFORMAT=\"%n %l\" >RESULTS:$R-libsv0.txt" 8
stop_collect "$OUT"
