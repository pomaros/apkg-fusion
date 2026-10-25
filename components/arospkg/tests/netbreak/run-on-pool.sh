#!/bin/sh
# A broken download on a reserved ABIv11 pool machine, CLI and window:
#
#   tests/netbreak/run-on-pool.sh RUNID STAGEDIR RESULTSDIR SHOTDIR
#
# STAGEDIR holds apkg, PkgManager, netbreak.rexx and setup.script (see
# make-stage.sh). Reservation as for tests/arexx/run-on-pool.sh. The guest
# decides PASS and FAIL; grade with tests/netbreak/grade.py.
set -e
R=$1; STAGE=$2; OUT=$3; SHOTS=$4
[ -n "$SHOTS" ] || { echo "usage: $0 RUNID STAGEDIR RESULTSDIR SHOTDIR"; exit 2; }
mkdir -p "$SHOTS"
. "$(dirname "$0")/../arexx/pool-lib.sh"
rx() { t "rx RAM:rxt/netbreak.rexx $R $1" "${2:-20}"; }

boot_staged "$STAGE"
sleep 90                      # AROSTCP
t "Execute RAM:rxt/setup.script" 10
rx cli 420; shot 01-cli
t "Copy SYS:NbCli/db/index.json SYS:NbGui/db/index.json" 3
t "Run >NIL: RAM:rxt/PkgManager --root SYS:NbGui --break-download-at 50000 --log RAM:nb-pm.log" 10
rx gui 600; shot 02-gui
rx gone 60
t "Copy RAM:nb-pm.log RESULTS:$R-pm.log" 3
t "Copy RAM:nb-pm-after-break.log RESULTS:$R-pm-after-break.log" 3
shot 03-end
stop_collect "$OUT"
