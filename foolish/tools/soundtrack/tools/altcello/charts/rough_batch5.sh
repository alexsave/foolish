#!/bin/bash
cd "$(dirname "$0")/.."
PY="/Users/alex/Dev/soundtrack-tools/venv/bin/python -I"
r() { nice $PY charts/rough_run.py "$@"; }
r base "" "D2,G2c,D3,A3" ""
r c_n4 "th_n=4" "D3,A3" ""
r c_n4hp1 "th_n=4,bn_hp.C=1000" "D2,G2c" ""
r c_n4vb10 "th_n=4" "D2,G2c" "--vb 0.10 0.0001"
r c_n4b115 "th_n=4" "D2,G2c" "--beta0 0.115"
