#!/bin/sh
# One phase alone, for a build under suspicion:
#
#   tests/arexx/run-phase.sh RUNID PHASE STAGEDIR RESULTSDIR SHOTDIR
#
# rxtest.rexx runs in the background, so a WAIT that never returns -- the
# failure `rapid` exists to catch -- leaves the Shell free to copy the
# window's log out. A build whose job is handed the previous job's outcome
# shows it there as "message for job N ignored", and its rapid result file
# is never finished. `spin` against a build whose worker is starved ends
# after its 120 s limit with the install still running.
set -e
R=$1; PHASE=$2; STAGE=$3; OUT=$4; SHOTS=$5
[ -n "$SHOTS" ] || { echo "usage: $0 RUNID PHASE STAGEDIR RESULTSDIR SHOTDIR"; exit 2; }
mkdir -p "$SHOTS"
. "$(dirname "$0")/pool-lib.sh"
boot_staged "$STAGE"
t "Execute RAM:rxt/setup.script" 20
t "Run >NIL: rx RAM:rxt/rxtest.rexx $R $PHASE" 5
sleep 150; shot $PHASE
t "Copy RAM:rxt-pm1.log RESULTS:$R-pm1.log" 4
stop_collect "$OUT"
