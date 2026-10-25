#!/bin/sh
# Copy one run's result files into a report directory, and only those.
#   tools/report-add.sh RUNID SRCDIR REPORTDIR
# A results disk is shared between runs and sessions; a file is taken only if
# its name starts with RUNID. Text files lose their carriage returns (Latin-1
# is kept as it is); everything taken is listed in REPORTDIR/MANIFEST.sha256,
# which tools/check-reports.py compares with the directory before a push.
set -e
RUN=$1; SRC=$2; DST=$3
[ -n "$DST" ] || { echo "usage: $0 RUNID SRCDIR REPORTDIR"; exit 2; }
mkdir -p "$DST"
n=0
for f in "$SRC/$RUN"*; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    case "$b" in
    *.txt|*.log|*.json) python3 -c "import sys;open(sys.argv[2],'wb').write(open(sys.argv[1],'rb').read().replace(b'\r',b''))" "$f" "$DST/$b" ;;
    *) cp "$f" "$DST/$b" ;;
    esac
    n=$((n + 1))
done
[ $n -gt 0 ] || { echo "no file in $SRC starts with $RUN"; exit 1; }
(cd "$DST" && ls | grep -vx -e MANIFEST.sha256 -e README.md | xargs shasum -a 256 > MANIFEST.sha256)
echo "$n file(s) from run $RUN into $DST"
