#!/bin/sh
# The whole CLI and GUI evidence set on one pair of binaries, on a reserved
# pool machine:
#
#   tests/run-regression-on-pool.sh PAYLOADDIR EVIDENCEDIR SHOTDIR [trials]
#
# With "trials" it starts from a fresh boot at trial A, for a run whose first
# part (upgrade to the LHA suite) is already in EVIDENCEDIR. With "triald"
# it runs trial D alone, from a fresh boot: D installs, starts and removes
# its own three packages and needs nothing from A to C.
# PAYLOADDIR comes from tests/make-payload.sh. It is staged, unzipped to
# RAM:rxt and assigned as PAYLOAD:, so the suites run unchanged from the
# paths they were written for. Their files come back by putfile to
# tests/tools/recv.py, started here on the host (QEMU maps the guest's
# 10.0.2.2 to it).
#
# What earlier runs taught (see docs/reports/2026-09-25-v02):
# - AROSTCP needs about two minutes after a boot, so each boot waits;
# - the first CLI suite runs at the 40960-byte stack a plain Shell gives;
# - trials A, B and C run without a reboot between them, because C relies
#   on what A left in RAM:;
# - the programs trial B starts swallow keystrokes, so the reboot comes
#   after trial D;
# - the window flow runs after a reboot of its own;
# - the LHA suite leaves Micropolis running, and its window covers the
#   Shell this script types into: it is quit (Esc) before trial A.
# - trial B leaves programs with screens of their own in front, and windows
#   over the Shell: Workbench is brought back and a new Shell opened on top
#   before trial C.
# A "reboot" here is a power cycle through the pool: stop, stage, start.
set -e
PAYLOAD=$1; EV=$2; SHOTS=$3; FROM=${4:-start}
[ -n "$SHOTS" ] || { echo "usage: $0 PAYLOADDIR EVIDENCEDIR SHOTDIR"; exit 2; }
HERE=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$EV" "$SHOTS"
. "$HERE/tests/arexx/pool-lib.sh"
# One receiver on the port, ours: an old one still listening would take the
# files into its own directory and this run would wait for them forever.
if lsof -nP -iTCP:8765 -sTCP:LISTEN >/dev/null 2>&1; then
  echo "port 8765 is already taken:"; lsof -nP -iTCP:8765 -sTCP:LISTEN; rmdir "$LOCK"; exit 3
fi
python3 "$HERE/tests/tools/recv.py" "$EV" 8765 >"$SHOTS/recv.log" 2>&1 &
RECV=$!
sleep 1; kill -0 $RECV 2>/dev/null || { echo "recv.py did not start"; cat "$SHOTS/recv.log"; rmdir "$LOCK"; exit 3; }
trap 'kill $RECV 2>/dev/null; rmdir "$LOCK"' EXIT

# Some boots come up with no working network, and then the first download
# waits forever. So the network is proved after each boot: a file sent with
# putfile must reach the host. If it does not, the machine is power-cycled,
# up to three times.
boot() {
  for try in 1 2 3; do
    boot_staged "$PAYLOAD"
    t "Assign PAYLOAD: RAM:rxt" 3
    sleep 120
    probe=netprobe-$(date +%H%M%S)
    t "Copy PAYLOAD:PUTFILE RAM:putfile" 3
    t "Protect RAM:putfile +e" 2
    t "Echo >RAM:$probe network" 2
    t "RAM:putfile RAM:$probe" 2
    n=0; while [ ! -f "$EV/$probe" ] && [ $n -lt 8 ]; do sleep 5; n=$((n + 1)); done
    if [ -f "$EV/$probe" ]; then
      rm -f "$EV/$probe"
      echo "  booted, network proved $(date +%T)"
      return 0
    fi
    echo "  booted without a working network (try $try) $(date +%T)"
    shot "boot-no-network-$(date +%H%M%S)"
    ./vm.sh stop $M >/dev/null 2>&1 || true
    stopped
  done
  echo "  no network after three boots"; exit 4
}
# Before each suite, the Shell must be the one taking the keys: a program
# left running from an earlier suite has taken them more than once. A line
# typed now writes a file named for this run and this suite and sends it;
# only when it arrives is the suite started. If it does not, Workbench is
# brought to the front and it is tried once more; after that the suite is
# recorded as not run.
fresh_shell() { vm key meta_l-n; sleep 3; vm key meta_r-w; sleep 6; }
ack() {
  a=ack-$(echo "$1" | tr '/' '-')-$(date +%H%M%S)
  t "Echo >RAM:$a $a" 2
  t "RAM:putfile RAM:$a" 2
  n=0; while [ ! -f "$EV/$a" ] && [ $n -lt 8 ]; do sleep 5; n=$((n + 1)); done
  [ -f "$EV/$a" ] && { rm -f "$EV/$a"; return 0; }
  return 1
}
# run SCRIPT RESULTFILE [MINUTES]: the suite is over when its last file arrives
run() {
  echo "== $1 ($(date +%T))"
  if ! ack "$1"; then
    fresh_shell
    if ! ack "$1"; then
      # A program that ignores Ctrl-C can hold the screen (Micropolis after
      # the LHA suite, 2026-09-29): clicking it away is not reliable, a power
      # cycle is. Only a suite that needs nothing from the ones before it
      # may be recovered this way; trials B to D rely on trial A's RAM:.
      shot "no-shell-before-$(echo "$1" | tr '/' '-')"
      case "$1" in R/TRIALB|R/TRIALC) echo "   NOT RUN: the Shell did not answer, and this suite needs the one before it"; return 0 ;; esac
      echo "   the Shell did not answer; power-cycling before $1"
      boot
      t "Stack 262144" 1
      ack "$1" || { echo "   NOT RUN: the Shell did not answer after a power cycle"; return 0; }
    fi
  fi
  t "Execute PAYLOAD:$1" 2
  n=0; lim=$(( ${3:-40} * 12 ))
  until [ -f "$EV/$2" ]; do
    n=$((n + 1)); [ $n -gt $lim ] && { echo "   TIMEOUT waiting for $2"; shot "timeout-$(echo "$1" | tr '/' '-')"; return 0; }
    [ $((n % 60)) -eq 0 ] && shot "during-$(echo "$1" | tr '/' '-')-$n"
    sleep 5
  done
  sleep 5; echo "   done $(date +%T)"
}
d() { vm drag "$1" "$2" "$3" "$4"; }

if [ "$FROM" = start ]; then
boot
t "Stack 40960" 1; t "Stack" 1; shot stack-cli
run S/UPGRADE up-reports.txt
t "Stack 262144" 1
run S/SOLUP sol-reports.txt
run S/VERIFY vf-reports.txt
run S/JSONTEST json-reports.txt
run R/REL1 rel1-reports.txt
boot
run R/REL2 rel2-reports.txt
run R/REL1G rel1g-reports.txt
run R/LHATEST lha-reports.txt
shot lha-micropolis
c 500 400; sleep 1; vm key esc; sleep 8; shot lha-after-esc
elif [ "$FROM" = triald ]; then
boot
t "Stack 262144" 1
run R/TRIALD triald-reports.txt
echo "TRIAL D DONE $(date +%T)"
./vm.sh stop $M >/dev/null 2>&1 || true
stopped
exit 0
else
boot
t "Stack 262144" 1
fi
run R/TRIALA triala-reports.txt 60
run R/TRIALB trialb-console.txt
shot trialb
# Some of the programs trial B starts open screens of their own in front of
# Workbench, and their windows cover the Shell. Amiga+N brings Workbench
# back, and a new Shell (Amiga+W) opens on top of the windows.
fresh_shell; shot trialb-workbench
run R/TRIALC trialc-reports.txt
run R/TRIALD triald-reports.txt
boot
run R/ACCEPT accept-reports.txt
echo "CLI DONE $(date +%T)"

boot
# 800x600 window at (100,40): six buttons at y=601 (Open folder fifth); Soliton opens at
# (200,185) with its close gadget at (208,193)
ROW1="140 122"; INSTALL="175 601"; UPGRADE="308 601"; ROLLBACK="441 601"
CLOSE="111 53"; TITLE="500 53"; SOLCLOSE="208 193"
refresh() { d 620 78 630 143; sleep 2; d 620 78 630 103; sleep 2; c $ROW1; sleep 3; }
runsol() { t "CD SYS:PkgSol/soliton" 1; t "Run >NIL: <NIL: Soliton" 12; shot run$1
  t "Status >RAM:gui-run$1.txt" 2; t "CD SYS:" 1; c $SOLCLOSE; sleep 3; c $TITLE; sleep 1; }
t "Execute PAYLOAD:S/GUI" 12; shot g0
c $ROW1; sleep 3; shot g1
c $INSTALL; sleep 16; shot g2
runsol 1
t "Copy RAM:idx-sol2.json SYS:PkgSol/db/index.json" 2
t "Copy RAM:soliton-r2.zip SYS:PkgSol/cache/soliton.new.zip" 2; c $TITLE; sleep 1
refresh; shot g3
c $UPGRADE; sleep 26; shot g4
runsol 2
c $ROLLBACK; sleep 20; shot g5
runsol 3
t "Echo >>SYS:PkgSol/soliton/Soliton.guide \"local change\"" 2
t "Copy RAM:soliton-r2.zip SYS:PkgSol/cache/soliton.new.zip" 2; c $TITLE; sleep 1
refresh
c $UPGRADE; sleep 12; shot g6
t "Delete SYS:PkgSol/soliton/Soliton.guide" 1; c $TITLE; sleep 3; c $ROW1; sleep 2
c $UPGRADE; sleep 26; shot g7
t "Delete SYS:PkgSol/cache/previous/soliton.zip" 1; c $TITLE; sleep 1
refresh; shot g8
c $CLOSE; sleep 4; shot g9
t "Status >RAM:gui-status.txt" 2
t "RAM:apkg --root SYS:PkgSol --json list >RAM:gui-list.txt" 3
t "RAM:apkg --root SYS:PkgSol info soliton >RAM:gui-info.txt" 2
for f in gui-events.log gui-run1.txt gui-run2.txt gui-run3.txt gui-list.txt gui-info.txt gui-status.txt; do
  t "RAM:putfile RAM:$f" 4
done
sleep 10
echo "ALL DONE $(date +%T)"
./vm.sh stop $M >/dev/null 2>&1 || true
stopped
