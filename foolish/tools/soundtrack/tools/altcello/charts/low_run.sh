#!/bin/bash
# the one-factor runs of the low strings study, serial (charts/low_study.py); knobs apply to C2 and G2 only
cd "$(dirname "$0")/.."
PY=/Users/alex/Dev/soundtrack-tools/venv/bin/python
run() { nice $PY -I charts/low_study.py "$@"; }
mkdir -p out/low_study
rm -f out/low_study/*.txt
run base ""
run q200 "q2k.CG=200"
run q120 "q2k.CG=120"
run q80 "q2k.CG=80"
run zeta0.08 "zeta.CG=0.08"
run zeta0.12 "zeta.CG=0.12"
run zt5 "zt.CG=5"
run zt8 "zt.CG=8"
echo DONE
