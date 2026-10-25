#!/bin/sh
# The example scripts in examples/arexx, run on a reserved pool machine the
# way a user would run them, each one's output kept as RESULTS:<run>-ex-*.
#
#   tests/arexx/run-examples.sh RUNID STAGEDIR RESULTSDIR SHOTDIR
#
# STAGEDIR from make-stage.sh (which copies the examples in). Upgrade.rexx
# waits for a decision in the window; this script makes it by pressing
# Proceed at PROCEED. Grade with grade.py --examples.
set -e
R=$1; STAGE=$2; OUT=$3; SHOTS=$4
[ -n "$SHOTS" ] || { echo "usage: $0 RUNID STAGEDIR RESULTSDIR SHOTDIR"; exit 2; }
PROCEED=${PROCEED:-"378 401"}
mkdir -p "$SHOTS"
. "$(dirname "$0")/pool-lib.sh"
ex() { t "rx >RESULTS:$R-ex-$2.txt RAM:rxt/$1" "${3:-10}"; }

boot_staged "$STAGE"
t "Execute RAM:rxt/setup.script" 20
ex IsInstalled.rexx\ soliton isinstalled-before 8
ex Install.rexx\ soliton install 30
ex IsInstalled.rexx\ soliton isinstalled-after 8
ex IsInstalled.rexx\ nosuch isinstalled-unknown 8
ex ShowPackage.rexx\ cls show 8; shot ex-show
ex InstallWatch.rexx\ xrick\ 3 watch-cancel 120; shot ex-watch
ex InstallWatch.rexx\ cls install-watch 30
t "Copy RAM:rxt/IDX2 SYS:PkgRx/db/index.json" 3
t "Copy RAM:rxt/SOLR2 SYS:PkgRx/cache/soliton.new.zip" 3
# In the foreground: the Shell waits in WAIT while the window asks. (Run
# cannot carry a second redirection: rx would get ">RESULTS:..." as the
# script's name.)
t "rx >RESULTS:$R-ex-upgrade.txt RAM:rxt/Upgrade.rexx soliton" 25; shot ex-confirm
{ press_proceed || echo "Proceed could not be pressed; the run goes on and its checks will say what followed"; }; sleep 26; shot ex-upgraded
t "Copy RAM:rxt-pm1.log RESULTS:$R-ex-pm1.log" 10
stop_collect "$OUT"
