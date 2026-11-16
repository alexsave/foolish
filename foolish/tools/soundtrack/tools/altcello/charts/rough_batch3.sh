#!/bin/bash
# combinations on D2 and G2c: charts/rough_batch3.sh
cd "$(dirname "$0")/.."
PY="/Users/alex/Dev/soundtrack-tools/venv/bin/python -I"
r() { nice $PY charts/rough_run.py "$@"; }
r c_n4 "th_n=4" "D2,G2c" ""
r c_hp3 "bn_hp.C=3000" "D2,G2c" ""
r c_hp1 "bn_hp.C=1000" "D2,G2c" ""
r c_n4hp3 "th_n=4,bn_hp.C=3000" "D2,G2c" ""
r c_n4s5 "th_n=4,bn_scale.C=0.5" "D2,G2c" ""
r c_n4b121 "th_n=4" "D2,G2c" "--beta0 0.121"
r c_n4zt5 "th_n=4,zt.C=5" "D2,G2c" ""
