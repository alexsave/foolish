#!/bin/bash
# the three one-at-a-time variants of the uncertain constants (xi 3, b_tau 0.11, sigma0 2e6), serially: BAND GRID REV, then the expert notes and the scale.
# Usage: charts/thermal_variants.sh [NAME ...]   (default: xi3 b0.11 s2e6)
cd "$(dirname "$0")/.." || exit 1
declare -A K=( [xi3]="th_xi=3" [b0.11]="th_b=0.11" [s2e6]="th_s0=2e6" )
names=("$@"); [ ${#names[@]} -eq 0 ] && names=(xi3 b0.11 s2e6)
for n in "${names[@]}"; do
  export MODELS=$n TAG=$n EXTRA=${K[$n]}
  charts/thermal_run.sh BAND
  charts/thermal_run.sh GRID
  charts/thermal_run.sh REV > out/thermal/rev_$n.txt
  unset MODELS
  charts/thermal_run2.sh "$n" "bow=1,${K[$n]}"
done
echo VARIANTS DONE
