#!/bin/bash
# the one-factor runs of the sympathetic ringing study, serial (charts/ring_study.py)
cd "$(dirname "$0")/.."
PY=/Users/alex/Dev/soundtrack-tools/venv/bin/python
run() { nice $PY -I charts/ring_study.py "$@"; }
rm -f out/ring_study/*.txt
mkdir -p out/ring_study
run base "" 
run lead1.5 "lead_mass=1.5"
run lead2 "lead_mass=2"
run stat2 "stat_mass=2"
run s0_2.4 "s0=2.4"
echo DONE
