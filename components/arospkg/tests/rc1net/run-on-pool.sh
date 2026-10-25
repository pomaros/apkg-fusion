#!/bin/sh
# PkgManager rc1 over the network, on a reserved ABIv11 pool machine:
#
#   tests/rc1net/run-on-pool.sh RUNID STAGEDIR RESULTSDIR SHOTDIR
#
# STAGEDIR comes from tests/rc1net/make-stage.sh. The machine (AROS_VM_NAME)
# must be reserved first, with AROS_VM_OWNER and AROS_VM_RESERVATION
# exported, as for tests/arexx/run-on-pool.sh; AROS names the directory with
# vm.sh and tools/vmctl.py (default ~/Work/AROS). The guest decides PASS and
# FAIL (nettest.rexx); this script starts the windows, types, waits, takes
# screenshots and collects. Grade with tests/rc1net/grade.py afterwards.
set -e
R=$1; STAGE=$2; OUT=$3; SHOTS=$4
[ -n "$SHOTS" ] || { echo "usage: $0 RUNID STAGEDIR RESULTSDIR SHOTDIR"; exit 2; }
mkdir -p "$SHOTS"
. "$(dirname "$0")/../arexx/pool-lib.sh"
rx() { t "rx RAM:rxt/nettest.rexx $R $1" "${2:-20}"; }
window() { t "Run >NIL: RAM:rxt/$1 --root $2 --log RAM:rxt-$3.log" 10; }

boot_staged "$STAGE"
t "Execute RAM:rxt/setup.script" 15

# --- the published rc1 window on the real catalogue
window PkgManager SYS:RcNet net; shot 01-net-window
rx update 120; shot 02-updated
rx archives 120
rx github 300; shot 03-installed
rx quitnet 10
rx gonenet 60

# --- a certificate the client must refuse, then a normal install
window PkgManager SYS:RcTls tls
rx tls 180; shot 04-tls-refused
rx recover 120; shot 05-recovered
rx quittls 10
rx gonetls 60

# --- the fault-injection build: no entropy, no connection
window PkgManager-entropyfail SYS:RcEnt ent
rx entfail 120; shot 06-entropy-failed
rx quitent 10
rx goneent 60

for f in net tls ent; do t "Copy RAM:rxt-$f.log RESULTS:$R-pm-$f.log" 3; done
for d in RcNet RcTls RcEnt; do t "List SYS:$d ALL >RESULTS:$R-final-$d.txt" 5; done
t "Status >RESULTS:$R-final-status.txt" 3
shot 07-end

stop_collect "$OUT"
