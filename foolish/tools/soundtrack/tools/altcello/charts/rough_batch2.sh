#!/bin/bash
# F3 distributed contact, then combinations: charts/rough_batch2.sh
cd "$(dirname "$0")/.."
PY="/Users/alex/Dev/soundtrack-tools/venv/bin/python -I"
r() { nice $PY charts/rough_run.py "$@"; }
r f3_n2 "th_n=2" "D2,G2c" ""; r f3_n3 "th_n=3" "D2,G2c" ""; r f3_n4 "th_n=4" "D2,G2c" ""; r f3_n3t "th_n=3,th_tilt=0.5" "D2,G2c" ""
