#!/bin/bash
# one factor at a time on D2 and G2c (3 seeds): charts/rough_batch.sh FACTOR
cd "$(dirname "$0")/.."
PY="/Users/alex/Dev/soundtrack-tools/venv/bin/python -I"
r() { nice $PY charts/rough_run.py "$@"; }
case "$1" in
 F1) for s in 0 0.25 0.5; do r "f1_$s" "bn_scale.C=$s" "D2,G2c" ""; done ;;
 F2) for w in 0.005 0.015 0.02; do r "f2_$w" "bw=$w" "D2,G2c" ""; done ;;
 F4) r f4_z1 "zeta.C=0.1" "D2,G2c" ""; r f4_zt5 "zt.C=5" "D2,G2c" ""; r f4_both "zeta.C=0.1,zt.C=5" "D2,G2c" "" ;;
 F5) for f in 0.3 0.4 0.5; do r "f5_frac$f" "" "D2,G2c" "--frac $f 0.0001"; done
     for b in 0.101 0.121 0.141; do r "f5_beta$b" "" "D2,G2c" "--beta0 $b"; done
     for v in 0.08 0.10 0.14; do r "f5_vb$v" "" "D2,G2c" "--vb $v 0.0001"; done ;;
 F5b) for f in 0.65 0.8; do r "f5_frac$f" "" "D2,G2c" "--frac $f 0.0001"; done ;;
 F6) r f6_v5 "" "D2,G2c" "" vib _v5 ;;
 F3) r f3_n2 "th_n=2" "D2,G2c" ""; r f3_n3 "th_n=3" "D2,G2c" ""; r f3_n4 "th_n=4" "D2,G2c" ""; r f3_n3t "th_n=3,th_tilt=0.5" "D2,G2c" "" ;;
esac
