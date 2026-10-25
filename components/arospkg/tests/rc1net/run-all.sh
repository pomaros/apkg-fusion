#!/bin/sh
# The whole PkgManager rc1 acceptance on one reserved ABIv11 pool machine,
# three runs, each from a fresh boot:
#
#   tests/rc1net/run-all.sh WORKDIR
#
#   1. the ARexx suite (tests/arexx/run-on-pool.sh) on the published rc1
#      binaries;
#   2. the ARexx examples (tests/arexx/run-examples.sh), same binaries;
#   3. the network acceptance (tests/rc1net/run-on-pool.sh): the published
#      window, then the fault-injection build.
#
# WORKDIR must not exist; everything of this run goes there. The run id is
# rc1g<date-time>, so result files never collide with an earlier run's on
# the machine's results disk. Grades go to WORKDIR/*.md; the script exits 0
# only when all three pass. A pass that did not finish can be run again on
# its own with the lines printed at the start, with a new run id.
set -e
HERE=$(cd "$(dirname "$0")/../.." && pwd)
W=$1
[ -n "$W" ] || { echo "usage: $0 WORKDIR"; exit 2; }
[ ! -e "$W" ] || { echo "$W exists; give a new directory"; exit 2; }
[ -n "$AROS_VM_OWNER" ] && [ -n "$AROS_VM_RESERVATION" ] && [ -n "$AROS_VM_NAME" ] \
  || { echo "reserve an ABIv11 machine first and export AROS_VM_OWNER, AROS_VM_RESERVATION, AROS_VM_NAME"; exit 2; }
mkdir -p "$W"; W=$(cd "$W" && pwd)
R=rc1g$(date +%m%d%H%M)
echo "run id $R, machine $AROS_VM_NAME, tests at $(git -C "$HERE" rev-parse HEAD)" | tee "$W/RUN.txt"

sh "$HERE/tests/rc1net/fetch-rc1.sh" "$W/rc1"
BINDIR="$W/rc1" sh "$HERE/tests/arexx/make-stage.sh" "$W/stage-arexx" >/dev/null
sh "$HERE/tests/rc1net/make-stage.sh" "$W/stage-net" >/dev/null
cat "$W/stage-arexx.sha256" "$W/stage-net.sha256" >> "$W/RUN.txt"

rc=0
sh "$HERE/tests/arexx/run-on-pool.sh" "${R}a" "$W/stage-arexx" "$W/res-arexx" "$W/shots-arexx" > "$W/driver-arexx.log" 2>&1 || true
python3 "$HERE/tests/arexx/grade.py" "${R}a" "$W/res-arexx" "$W/stage-arexx.sha256" "$W/grade-arexx.md" >/dev/null || rc=1

sh "$HERE/tests/arexx/run-examples.sh" "${R}e" "$W/stage-arexx" "$W/res-examples" "$W/shots-examples" > "$W/driver-examples.log" 2>&1 || true
python3 "$HERE/tests/arexx/grade.py" --examples "${R}e" "$W/res-examples" "$W/stage-arexx.sha256" "$W/grade-examples.md" >/dev/null || rc=1

sh "$HERE/tests/rc1net/run-on-pool.sh" "${R}n" "$W/stage-net" "$W/res-net" "$W/shots-net" > "$W/driver-net.log" 2>&1 || true
python3 "$HERE/tests/rc1net/grade.py" "${R}n" "$W/res-net" "$W/stage-net.sha256" "$W/grade-net.md" >/dev/null || rc=1

for g in arexx examples net; do
  printf '%-9s %s\n' "$g" "$(grep -q -e 'Everything holds' -e 'Every phase passed' "$W/grade-$g.md" 2>/dev/null && echo PASS || echo 'FAIL or NOT RUN, see grade-'$g'.md')"
done | tee -a "$W/RUN.txt"
exit $rc
