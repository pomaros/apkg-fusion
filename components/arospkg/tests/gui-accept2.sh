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

ty() { python3 tools/vmctl.py type "$1" >/dev/null 2>&1; }      # type without Return
k()  { python3 tools/vmctl.py key "$1" >/dev/null 2>&1; }
d()  { python3 tools/vmctl.py drag "$1" "$2" "$3" "$4" >/dev/null 2>&1; }
# layout of the 760x600 window at (100,40), read off a screenshot of the first launch.
# A Zune Cycle selects on RELEASE: its popup opens under the gadget while the
# button is held, 20 px per entry, so a choice is a drag, not a click.
SEARCH="200 78"; CAT="393 78"; STATE="583 78"; UPDATE="772 78"; ROW1="140 126"
INSTALL="235 601"; REMOVE="488 601"; CANCEL="740 601"; CLOSE="111 53"; TITLE="400 53"; SHELL="50 300"
clear_search() { c $SEARCH; k end; for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14; do k backspace; done; }
pick() { clear_search; ty "$1"; k ret; sleep 2; c $ROW1; sleep 3; }

until pgrep -f aros-onetest-hd.qcow2 >/dev/null && [ -S /tmp/aros-onetest-monitor.sock ]; do sleep 5; done; sleep 60
wb_up; python3 tools/vmctl.py key meta_r-w >/dev/null 2>&1; settle; echo "shell open"
sleep 75
t "Execute PAYLOAD:S/GUI"; sleep 10; shot g0
c $UPDATE; sleep 8; shot g1                        # index, slowed too
d 393 78 405 143; sleep 2; shot g2                 # category popup: game
d 583 78 595 143; sleep 2; shot g2b                # state popup: Not installed
d 583 78 595 103; sleep 1                          # back to All
pick soliton; shot g3                              # detail: requirements probed now
c $SHELL; sleep 1; ty "RAM:apkg install untangle >RAM:gui-lock.txt"; sleep 1
c $TITLE; sleep 1; c $INSTALL; sleep 2
c $SHELL; k ret; sleep 1; shot g4                  # CLI refused, window mid-download
c $TITLE; sleep 24; shot g5                        # installed; selection and filter kept
c $REMOVE; sleep 6; shot g6                        # removed
pick blips;  c $INSTALL; sleep 5; c $CANCEL; sleep 5; shot g7     # 1.4 MB at ~35 KB/s; cancel at 5 s
pick sdlpop; c $INSTALL; sleep 5; c $CLOSE;  sleep 8; shot g8     # close during the download: cancel + quit
c $SHELL; sleep 1; t "Status >RAM:gui-status1.txt"; sleep 2
t "Delete SYS:Packages/cache/#?"; sleep 2                          # so the next install downloads again
t "Run >NIL: RAM:PkgManager --slow 300 --log RAM:gui-events2.log"; sleep 10; shot g9
pick soliton; c $INSTALL; sleep 10; c $CLOSE; sleep 1; shot g10    # ~7 s download, then a 6 s pause in extract
sleep 10; shot g11
c $SHELL; sleep 1; t "Status >RAM:gui-status2.txt"; sleep 2
t "RAM:apkg --json list >RAM:gui-list.txt"; sleep 3
t "List SYS:Packages/cache >RAM:gui-cache.txt"; sleep 2
for f in gui-events.log gui-events2.log gui-lock.txt gui-status1.txt gui-status2.txt gui-list.txt gui-cache.txt; do t "RAM:putfile RAM:$f"; sleep 3; done
echo "GUI ACCEPTANCE 2 DONE $(date +%T)"
