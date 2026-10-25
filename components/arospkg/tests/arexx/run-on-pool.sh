#!/bin/sh
# The ARexx acceptance run on a reserved pool machine, start to finish:
#
#   tests/arexx/run-on-pool.sh RUNID STAGEDIR RESULTSDIR SHOTDIR
#
# STAGEDIR comes from tests/arexx/make-stage.sh. The machine (AROS_VM_NAME,
# default v11-2) must be reserved first: AROS_VM_OWNER and
# AROS_VM_RESERVATION exported, as ~/Work/AROS/documentation/vm-pool.md
# describes. The guest decides PASS and FAIL (rxtest.rexx); this script only
# types, presses what a user would press, waits, and collects. Grade with
# tests/arexx/grade.py afterwards.
#
# PROCEED="x y" is where the confirmation window's Proceed button is; the
# window opens at a fixed place (main.c), so this is measured once.
# PROCEED=ask stops at the first confirmation until SHOTDIR/proceed.xy holds
# the coordinates read off 05-confirm.png.
set -e
R=$1; STAGE=$2; OUT=$3; SHOTS=$4
[ -n "$SHOTS" ] || { echo "usage: $0 RUNID STAGEDIR RESULTSDIR SHOTDIR"; exit 2; }
PROCEED=${PROCEED:-"378 401"}
mkdir -p "$SHOTS"
. "$(dirname "$0")/pool-lib.sh"
rx() { t "rx RAM:rxt/rxtest.rexx $R $1" "${2:-20}"; }

boot_staged "$STAGE"
t "Execute RAM:rxt/setup.script" 20
shot 01-start

# --- port, queries, SHOW
for p in env parse stress queries0; do rx $p; done
rx show 8; shot 02-show

# --- install and remove from ARexx; a second client reads the outcome
rx installstart 30; shot 03-installing
rx installcheck 50
rx remove 40

# --- jobs back to back, through the gap between a worker's end and its report
rx rapid 90

# --- a script polling without a pause: the job finishes, the window answers
# While it polls, search for "cls" and clear the search again, in the window.
# (Not the cycle gadgets: a click opens their menu and changes nothing.)
t "rx RAM:rxt/rxtest.rexx $R spin" 5
c 200 78; sleep 1; vm type cls; vm key ret; sleep 2
c 200 78; sleep 1; for k in 1 2 3; do vm key backspace; done; vm key ret
sleep 30

# --- the window starts an install; ARexx is refused while it runs
rx select 8
c 175 601; sleep 1
rx guibusy 40; shot 04-window-job-done

# --- the CLI holds the root; an ARexx operation fails on the lock
rx clilock 45

# --- upgrade: waits for the user; declined by CANCEL; then confirmed by a click
rx upgradestart 90; shot 05-confirm
if [ "$PROCEED" = ask ]; then
  echo "waiting for $SHOTS/proceed.xy"
  until [ -s "$SHOTS/proceed.xy" ]; do sleep 5; done
  PROCEED=$(cat "$SHOTS/proceed.xy")
fi
c 806 78; sleep 2        # Update index while confirming: must do nothing
t "Run >NIL: rx RAM:rxt/rxtest.rexx $R waitjob" 4
{ press_proceed || echo "Proceed could not be pressed; the run goes on and its checks will say what followed"; }; sleep 26; shot 06-upgraded
rx upgradecheck 15

# --- rollback, confirmed the same way
rx rollbackstart 90; shot 07-rollback-confirm
{ press_proceed || echo "Proceed could not be pressed; the run goes on and its checks will say what followed"; }; sleep 26
rx rollbackcheck 20

# --- cancel while downloading (needs the network, up by now)
rx cancel 150; shot 08-cancelled

# --- QUIT while an install runs: leaves only after the job; WAIT sees the end
rx quitstart 20
sleep 15
rx gone 30

# --- two windows: the second has no port
t "Run >NIL: RAM:rxt/PkgManager --root SYS:PkgRx --slow 500 --log RAM:rxt-pm2.log" 10
t "Run >NIL: RAM:rxt/PkgManager --root SYS:PkgRx --slow 500 --log RAM:rxt-pm3.log" 10
shot 09-two-windows
rx collide 10
c 111 53; sleep 4        # close the second (front) window

# --- the close gadget during an install started from ARexx
# The phase removes cls first and only then starts the install the click is
# for; how long the removal takes depends on the machine, so the click waits
# until the window shows the install running, on two screens in a row: the
# window with the port in front (its title tab "PkgManager", nothing right of
# it), the list filled (cls in the first row, soliton in the second), Update
# index ghosted (a job runs) and the State of cls empty (removed, not yet
# installed again). A removal still shows cls installed; a refill shows no
# rows. If that never shows, the click still happens, to collect what the
# window does, but the run is marked as not synchronised and grade.py fails it.
installing() {
  mon "screendump $SHOTS/.close.ppm" && python3 -c "
import sys
d=open('$SHOTS/.close.ppm','rb').read().split(b'\n',3); w=int(d[1].split()[0]); px=d[3]
dark=lambda x0,y0,x1,y1: sum(1 for y in range(y0,y1) for x in range(x0,x1) if min(px[3*(y*w+x):3*(y*w+x)+3])<90)
ok = (150 <= dark(120,44,235,60) <= 175 and dark(235,44,420,60) == 0
      and 33 <= dark(116,115,168,129) <= 43 and 85 <= dark(116,130,168,144) <= 100
      and dark(760,71,850,86) == 0 and dark(223,115,271,129) < 20)
sys.exit(0 if ok else 1)" 2>/dev/null
}
t "rx RAM:rxt/rxtest.rexx $R closestart" 0
n=0; seen=0; SYNC=no
while [ $n -lt 150 ]; do
  if installing; then seen=$((seen + 1)); else seen=0; fi
  [ $seen -lt 2 ] || { SYNC=yes; break; }
  n=$((n + 1)); sleep 0.2
done
[ $SYNC = yes ] || echo "closestart: the install never showed in the window $(date +%T); clicking anyway, the run will fail"
c 111 53; echo "close clicked $(date +%T)"; sleep 2; shot 10-closing
sleep 20
rx gone2 30

# --- no rexxsyslib: the window works without a port
t "Run >NIL: RAM:rxt/PkgManager --root SYS:PkgRx --rexxlib nosuch.library --log RAM:rxt-pm4.log" 10
shot 11-no-arexx
rx norexx 10
c 140 122; sleep 3; c 574 601; sleep 10; shot 12-removed-from-window
c 111 53; sleep 4

# --- allocation failures: the final message, and long answers
t "Run >NIL: RAM:rxt/PkgManager --root SYS:PkgRx --test-nomem start --test-nomem result --log RAM:rxt-pm6.log" 10
rx nomem 20
c 111 53; sleep 4
t "Run >NIL: RAM:rxt/PkgManager --root SYS:PkgRx --test-nomem prepare --log RAM:rxt-pm7.log" 10
rx nomem2 20
c 111 53; sleep 4

# --- UPDATE to the end: the public catalogue replaces the test index
t "Run >NIL: RAM:rxt/PkgManager --root SYS:PkgRx --log RAM:rxt-pm5.log" 10
rx update 90; shot 13-updated
c 111 53; sleep 4

for f in pm1 pm2 pm3 pm4 pm5 pm6 pm7; do t "Copy RAM:rxt-$f.log RESULTS:$R-$f.log" 3; done
t "Copy RAM:rxt-cli-hold RESULTS:$R-cli-hold.txt" 3
t "RAM:rxt/apkg --root SYS:PkgRx list >RESULTS:$R-final-apkg-list.txt" 5
t "List SYS:PkgRx ALL >RESULTS:$R-final-root.txt" 5
shot 14-end

stop_collect "$OUT"
echo "closestart click synchronised with the install: $SYNC" > "$OUT/$R-host-closestart.txt"
