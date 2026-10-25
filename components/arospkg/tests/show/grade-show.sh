#!/bin/sh
# grade-show.sh RUNID RESULTSDIR STAGE.sha256: both phases reported, none failed,
# and the window that answered is the staged PkgManager.
R=$1; RES=$2; SHA=$3; bad=0
want=$(awk '$2=="PkgManager"{print $1}' "$SHA")
for p in noindex cli rexx; do
  f="$RES/$R-show-$p.txt"
  if [ ! -f "$f" ]; then echo "MISSING $p"; bad=1; continue; fi
  # Regina's lineout ends lines with CR LF on AROS
  tr -d '\r' < "$f" | /usr/bin/grep -h '^SUMMARY\|^FAIL'
  tr -d '\r' < "$f" | /usr/bin/grep -q "^SUMMARY $p pass [0-9]* fail 0$" || bad=1
done
/usr/bin/grep -q "sha256 $want" "$RES/$R-show-pm.log" || { echo "window log does not name PkgManager $want"; bad=1; }
[ $bad = 0 ] && echo "Everything holds." || echo "PROBLEMS"
exit $bad
