#!/bin/bash
# Iteration 2 sweeps, serially (one heavy job at a time). Usage: charts/iter2_run.sh PART
#  NOISE : D3 band vs bow-noise level (body on)           TORS : D3 band vs Z_t, c_t, zeta_t (body on, the chosen noise)
#  GRID  : per string 3x3 beta x v_B at 20 forces        NOISE_ARGS / TORS_ARGS can be set in the environment
# Rows go to /Users/alex/Music/foolish_cities_v5/alt_cello/iter2/*.txt, summaries to *.log.
cd "$(dirname "$0")/.." || exit 1
D=${ITER2_DIR:-/Users/alex/Music/foolish_cities_v5/alt_cello/iter2}
mkdir -p "$D"
T=./sweep_tool
G="0.03 5 32 6"
run() { # tag string beta vb nf extra...
  local tag=$1 s=$2 b=$3 vb=$4 nf=$5; shift 5
  nice $T ours "$s" "$b" 0.03 5 "$nf" 6 "$D/$tag.txt" --body --tail 0.5 --vb "$vb" "$@" > "$D/$tag.log"
  echo "$tag: $(grep SUMMARY "$D/$tag.log" | sed 's/SUMMARY ours [A-Z0-9]* beta [0-9.]* //')"
}
case "$1" in
NOISE) for n in 0 0.005 0.01 0.02 0.03; do run "noise_D3_$n" D3 0.1 0.1 32 --noise-level $n; done ;;
TORS)
  N=${NOISE_ARGS:-}
  run "tors_D3_base_zt3_ct5_zeta0.01" D3 0.1 0.1 32 $N
  for z in 2 5 8; do run "zt_D3_$z" D3 0.1 0.1 32 --zt $z $N; done
  for c in 3 7; do run "ct_D3_$c" D3 0.1 0.1 32 --ct $c $N; done
  for e in 0.03 0.05; do run "zeta_D3_$e" D3 0.1 0.1 32 --zeta $e $N; done ;;
COMBO) # joint torsion choices at zeta 0.05 (after the one-at-a-time sweeps)
  N=${NOISE_ARGS:-}
  run combo_D3_zt5_ct5_z0.05 D3 0.1 0.1 32 --zt 5 --zeta 0.05 $N
  run combo_D3_zt8_ct5_z0.05 D3 0.1 0.1 32 --zt 8 --zeta 0.05 $N
  run combo_D3_zt5_ct7_z0.05 D3 0.1 0.1 32 --zt 5 --ct 7 --zeta 0.05 $N
  run combo_D3_zt8_ct7_z0.05 D3 0.1 0.1 32 --zt 8 --ct 7 --zeta 0.05 $N ;;
FINE) # the new defaults, beta 0.1, v_B 0.1, 32 forces, all four strings
  for s in C2 G2 D3 A3; do run "fine_${s}" $s 0.1 0.1 32; done ;;
GRID)
  NF=${NF:-20}
  for s in C2 G2 D3 A3; do for b in 0.07 0.1 0.14; do for v in 0.05 0.1 0.2; do run "grid_${s}_b${b}_v${v}" $s $b $v $NF; done; done; done ;;
esac
