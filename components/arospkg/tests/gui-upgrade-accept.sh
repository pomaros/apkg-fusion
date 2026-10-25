#!/bin/sh
# The whole v0.1 evidence set, in order, on one binary. Reboots where the
# state under test requires one, and where programs left running would
# swallow the keystrokes that start the next script.
set -e
cd /Users/tumash/Work/AROS
export AROS_QEMU_MONITOR=/tmp/aros-onetest-monitor.sock AROS_QEMU_QMP=/tmp/aros-onetest-qmp.sock
EV=/Users/tumash/Work/AROS-dev/arospkg/docs/reports/2026-09-19-v01
M=$AROS_QEMU_MONITOR

settle() { prev=""; same=0; while [ $same -lt 3 ]; do printf 'screendump /tmp/fr.ppm\n' | nc -w 2 -U $M >/dev/null 2>&1; cur=$(shasum /tmp/fr.ppm 2>/dev/null | cut -c1-40); [ "$cur" = "$prev" ] && same=$((same+1)) || { same=0; prev=$cur; }; sleep 3; done; }
wb_up() { until printf 'screendump /tmp/fr.ppm\n' | nc -w 2 -U $M >/dev/null 2>&1 && python3 -c "
import sys
d=open('/tmp/fr.ppm','rb').read().split(b'\n',3); w,h=map(int,d[1].split()); px=d[3]
row=6*w*3; xs=range(w//3,2*w//3,20)
sys.exit(0 if sum(1 for x in xs if min(px[row+3*x:row+3*x+3])>225)>len(xs)*0.8 else 1)"; do sleep 10; done; settle; }
reboot_guest() { printf 'system_reset\n' | nc -w 2 -U $M >/dev/null; sleep 5; wb_up; python3 tools/vmctl.py key meta_r-w >/dev/null 2>&1; settle; echo "  rebooted, shell open"; }
run() { echo "== $1 ($(date +%T))"; rm -f "$EV/$2"; python3 tools/vmctl.py type "Execute PAYLOAD:S/$1" >/dev/null 2>&1; python3 tools/vmctl.py key ret >/dev/null 2>&1; until [ -f "$EV/$2" ]; do sleep 5; done; echo "   done $(date +%T)"; }

shot() { printf "screendump /tmp/$1.ppm\n" | nc -w 2 -U $M >/dev/null 2>&1; sleep 1; sips -s format jpeg /tmp/$1.ppm --out /tmp/$1.jpg >/dev/null 2>&1; echo "shot $1 $(date +%T)"; }
c() { python3 tools/vmctl.py click "$1" "$2" >/dev/null 2>&1; }
t() { python3 tools/vmctl.py type "$1" >/dev/null 2>&1; python3 tools/vmctl.py key ret >/dev/null 2>&1; }


until pgrep -f aros-onetest-hd.qcow2 >/dev/null && [ -S /tmp/aros-onetest-monitor.sock ]; do sleep 5; done; sleep 60
wb_up; python3 tools/vmctl.py key meta_r-w >/dev/null 2>&1; settle; echo "shell open"
sleep 75
ty() { python3 tools/vmctl.py type "$1" >/dev/null 2>&1; }
k()  { python3 tools/vmctl.py key "$1" >/dev/null 2>&1; }
d()  { python3 tools/vmctl.py drag "$1" "$2" "$3" "$4" >/dev/null 2>&1; }
# 800x600 window at (100,40): six buttons at y=601 (Open folder fifth); Soliton opens at (200,185) with its close gadget at (208,193)
ROW1="140 122"; INSTALL="175 601"; UPGRADE="308 601"; ROLLBACK="441 601"; REMOVE="574 601"
CLOSE="111 53"; TITLE="500 53"; SHELL="50 300"; SOLCLOSE="208 193"
# A cycle notifies only on a CHANGE: "Not installed" and back to "All" forces a refill.
refresh() { d 620 78 630 143; sleep 2; d 620 78 630 103; sleep 2; c $ROW1; sleep 3; }
runsol() {   # start Soliton from the Shell; it takes the focus, so click the Shell again before Status
  c $SHELL; sleep 1; t "CD SYS:PkgSol/soliton"; t "Run >NIL: <NIL: Soliton"; sleep 12; shot run$1
  c $SHELL; sleep 1; t "Status >RAM:gui-run$1.txt"; sleep 2; t "CD SYS:"; sleep 1
  c $SOLCLOSE; sleep 3; c $TITLE; sleep 1
}
c $SHELL; sleep 1; t "Execute PAYLOAD:S/GUI"; sleep 12; shot g0      # a fresh root, the window up
c $ROW1; sleep 3; shot g1
c $INSTALL; sleep 16; shot g2                           # installed
runsol 1
c $SHELL; sleep 1; t "Copy RAM:idx-sol2.json SYS:PkgSol/db/index.json"; t "Copy RAM:soliton-r2.zip SYS:PkgSol/cache/soliton.new.zip"; sleep 2; c $TITLE; sleep 1
refresh; shot g3                                        # "upgrade available: 2.2 revision 0 -> 2"
c $UPGRADE; sleep 26; shot g4                           # preview, auto-yes, upgrade
runsol 2
c $ROLLBACK; sleep 20; shot g5                          # preview, auto-yes, rollback
runsol 3
c $SHELL; sleep 1; t "Echo >>SYS:PkgSol/soliton/Soliton.guide \"local change\""; t "Copy RAM:soliton-r2.zip SYS:PkgSol/cache/soliton.new.zip"; sleep 2; c $TITLE; sleep 1
refresh
c $UPGRADE; sleep 12; shot g6                           # conflict: refused, nothing changed
c $SHELL; sleep 1; t "Delete SYS:PkgSol/soliton/Soliton.guide"; sleep 1; c $TITLE; sleep 1
c $UPGRADE; sleep 26; shot g7                           # upgraded
c $SHELL; sleep 1; t "Delete SYS:PkgSol/cache/previous/soliton.zip"; sleep 1; c $TITLE; sleep 1
refresh; shot g8                                        # "rollback unavailable", button greyed
c $CLOSE; sleep 4; shot g9
c $SHELL; sleep 1
t "Status >RAM:gui-status.txt"; sleep 2
t "RAM:apkg --root SYS:PkgSol --json list >RAM:gui-list.txt"; sleep 3
t "RAM:apkg --root SYS:PkgSol info soliton >RAM:gui-info.txt"; sleep 2
for f in gui-events.log gui-run1.txt gui-run2.txt gui-run3.txt gui-list.txt gui-info.txt gui-status.txt; do t "RAM:putfile RAM:$f"; sleep 3; done
echo "GUI UPGRADE FLOW 3 DONE $(date +%T)"
