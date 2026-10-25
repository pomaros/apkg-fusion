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
rm -f "$EV/up-reports.txt" "$EV/up-console.txt" "$EV/sol-reports.txt" "$EV/sol-console.txt"
run UPGRADE up-reports.txt
until [ -f "$EV/up-console.txt" ]; do sleep 3; done
run SOLUP sol-reports.txt
until [ -f "$EV/sol-console.txt" ]; do sleep 3; done
t "Execute PAYLOAD:S/GUI"; sleep 12; shot g0
echo "CLI RUNS DONE, GUI LAUNCHED $(date +%T)"
