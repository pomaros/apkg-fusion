# Sourced by run-on-pool.sh and run-examples.sh: boot a reserved pool
# machine with a staged payload, drive it, collect. Needs AROS_VM_OWNER and
# AROS_VM_RESERVATION exported (~/Work/AROS/documentation/vm-pool.md).
AROS=${AROS:-$HOME/Work/AROS}
M=${AROS_VM_NAME:-v11-2}
cd "$AROS"

# One driver per machine. A second one stops the machine under the first
# and both runs go to waste; it happened once, so it is refused here.
LOCK=${TMPDIR:-/tmp}/arexx-driver-$M.lock
mkdir "$LOCK" 2>/dev/null || { echo "another driver is using $M ($LOCK)"; exit 3; }
trap 'rmdir "$LOCK"' EXIT

vmenv() { ./vm.sh env $M 2>&1 | grep '^export' > "$SHOTS/.vm.env"; . "$SHOTS/.vm.env"; }
mon() { printf '%s\n' "$1" | nc -w 2 -U "$AROS_QEMU_MONITOR" >/dev/null 2>&1; }
# QMP has been seen to time out on its handshake, once, in the middle of a
# run; vmctl then fails before sending anything, and a retry is safe. Each
# retry is reported, so a doubled keystroke would at least be explained.
vm() {
  for try in 1 2 3; do
    python3 tools/vmctl.py "$@" >/dev/null 2>"$SHOTS/.vmctl.err" && return 0
    echo "vmctl $1 failed ($(tail -1 "$SHOTS/.vmctl.err")), retry $try $(date +%T)"; sleep 3
  done
  return 1
}
shot() { vm shot "$SHOTS/$1.png"; echo "shot $1 $(date +%T)"; }
c() { vm click "$1" "$2"; }
# The Shell sits left of PkgManager's window; a click there makes it the
# window that receives the keys, whatever a command brought to the front.
t() { c 50 300; sleep 1; vm type "$1"; vm key ret; sleep "${2:-2}"; }
stopped() { while pgrep -f "aros-pool-$M" >/dev/null; do sleep 2; done; }

boot_staged() { # STAGEDIR
  state=$(./vm.sh status 2>/dev/null | python3 -c "import json,sys; print(json.load(sys.stdin)['machines']['$M']['state'])")
  [ "$state" = stopped ] || { ./vm.sh stop $M >/dev/null 2>&1 || true; }
  stopped
  ./vm.sh stage $M "$1" >/dev/null
  vmenv
  ./vm.sh start $M >/dev/null
  # Workbench is up when the screen's title bar is drawn
  until mon "screendump $SHOTS/.boot.ppm" && python3 -c "
import sys
d=open('$SHOTS/.boot.ppm','rb').read().split(b'\n',3); w,h=map(int,d[1].split()); px=d[3]
row=6*w*3; xs=range(w//3,2*w//3,20)
sys.exit(0 if (w,h)==(1024,768) and sum(1 for x in xs if min(px[row+3*x:row+3*x+3])>225)>len(xs)*0.8 else 1)" 2>/dev/null; do
    # GRUB's menu (800x600) sometimes waits instead of counting down, and once
    # it stood in the entry editor, where Return only adds a line: Esc leaves
    # the editor (and does nothing in the menu), then Return boots.
    python3 -c "
import sys
d=open('$SHOTS/.boot.ppm','rb').read().split(b'\n',3); w,h=map(int,d[1].split())
sys.exit(0 if (w,h)==(800,600) else 1)" 2>/dev/null && { mon "sendkey esc"; sleep 1; mon "sendkey ret"; }
    sleep 5
  done
  sleep 25
  vm key meta_r-w; sleep 6
  echo "booted $(date +%T)"
  t "C:UnZip >RAM:unzip-log POOLINPUT:payload.zip -d RAM:rxt" 8
}

stop_collect() { # RESULTSDIR
  ./vm.sh stop $M >/dev/null 2>&1 || true
  stopped
  ./vm.sh collect $M "$1" >/dev/null
  echo "collected $1 $(date +%T)"
}

# The confirmation window is open when its Cancel button shows at (520,401)
# (main.c places the window at 260,180): button grey there, the list's white
# if not. Not the title bar, whose colour depends on which window is active.
confirm_open() {
  rm -f "$SHOTS/.cf.ppm"
  mon "screendump $SHOTS/.cf.ppm"; sleep 2
  python3 -c "
import sys
d=open('$SHOTS/.cf.ppm','rb').read().split(b'\n',3); w,h=map(int,d[1].split()); px=d[3]
o=(401*w+520)*3; r,g,b=px[o:o+3]
sys.exit(0 if 200 < min(r,g,b) and max(r,g,b) < 240 else 1)" 2>/dev/null
}
wait_confirm() { for i in $(seq 1 30); do confirm_open && return 0; sleep 2; done; echo "no confirmation window"; return 1; }
# An injected click is sometimes lost when it also has to activate the
# window; press again until the window has gone, and say so each time.
press_proceed() {
  wait_confirm || return 1
  for try in 1 2 3; do
    c $PROCEED; sleep 4
    confirm_open || return 0
    echo "Proceed not taken, press again ($try) $(date +%T)"
  done
  return 1
}
