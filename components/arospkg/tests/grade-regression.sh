#!/bin/sh
# Grade every suite of a tests/run-regression-on-pool.sh run.
#   tests/grade-regression.sh PAYLOADDIR EVIDENCEDIR APKG_SHA256
# R/TRIALB has no expectations file (its record is the console and the
# screenshots); the window flow is read from gui-events.log by hand.
P=$1 E=$2 M=$3
g() { echo "### $1"; shift; python3 "$(dirname "$0")/../tools/grade.py" "$@" --main $M 2>&1 | tail -4; }
g S/UPGRADE $P/S/UPGRADE $E/up-reports.txt $E/up-console.txt
g S/SOLUP $P/S/SOLUP $E/sol-reports.txt $E/sol-console.txt
g S/VERIFY $P/S/VERIFY $E/vf-reports.txt $E/vf-console.txt
g S/JSONTEST $P/S/JSONTEST $E/json-reports.txt $E/json-console.txt
g R/REL1 $P/R/REL1 $E/rel1-reports.txt $E/rel1-console.txt
g R/REL2 $P/R/REL2 $E/rel2-reports.txt $E/rel2-console.txt
g R/REL1G $P/R/REL1G $E/rel1g-reports.txt $E/rel1g-console.txt --binary g2a=37b2774a16afcd950e82ef29dbcb0d2fb994125406b1db0d8071ca69d14a7598
g R/LHATEST $P/R/LHATEST $E/lha-reports.txt $E/lha-console.txt
g R/TRIALA $P/R/TRIALA $E/triala-reports.txt $E/triala-console.txt
g R/TRIALC $P/R/TRIALC $E/trialc-reports.txt $E/trialc-console.txt
g R/TRIALD $P/R/TRIALD $E/triald-reports.txt $E/triald-console.txt
g R/ACCEPT $P/R/ACCEPT $E/accept-reports.txt $E/accept-console.txt
