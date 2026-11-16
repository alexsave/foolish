#!/bin/bash
# Measurements of the thermal bow model against the friction curve, serially (one heavy job at a time). Usage: charts/thermal_run.sh PART [KNOBS-extra]
#   BAND   contiguous Helmholtz band at beta 0.1, v_B 0.1, 32 forces 0.1-8 N, 6 s, body on, C2 open, G2 on C, D3 open, A3 open
#   GRID   Guettler cells (onset_tool grid, the charts/onset grid) for C2 open, G2 on C, D3 on G, A3 on D
#   REV    bow reversal on the same note (onset_tool fing bow), D3 on G and G2 on C, 3 seeds
# MODEL names: curve (bow=0) and thermal (bow=1). TAG and EXTRA (more knobs, comma separated, for the constant variants) via the environment.
cd "$(dirname "$0")/.." || exit 1
D=out/thermal
mkdir -p $D
TAG=${TAG:-thermal}
EXTRA=${EXTRA:-}
models() { if [ -n "$MODELS" ]; then echo $MODELS; else echo "curve $TAG"; fi; }
knobs() { # model name -> ALTCELLO_KNOBS
  if [ "$1" = curve ]; then echo "bow=0"; else echo "bow=1${EXTRA:+,$EXTRA}"; fi
}
case "$1" in
BAND)
  for m in $(models); do
    export ALTCELLO_KNOBS=$(knobs $m)
    nice ./sweep_tool ours C2 0.1 0.1 8 32 6 $D/band_${m}_C2_0.txt --vb 0.1 --body > $D/band_${m}_C2_0.log
    nice ./sweep_tool ours C2 0.1 0.1 8 32 6 $D/band_${m}_C2_7.txt --vb 0.1 --body --semis 7 > $D/band_${m}_C2_7.log
    nice ./sweep_tool ours D3 0.1 0.1 8 32 6 $D/band_${m}_D3_0.txt --vb 0.1 --body > $D/band_${m}_D3_0.log
    nice ./sweep_tool ours A3 0.1 0.1 8 32 6 $D/band_${m}_A3_0.txt --vb 0.1 --body > $D/band_${m}_A3_0.log
    for c in C2_0 C2_7 D3_0 A3_0; do echo "$m $c: $(grep 'SUMMARY ours' $D/band_${m}_$c.log | sed 's/SUMMARY ours [A-Z0-9]* beta [0-9.]* //')"; done
  done ;;
GRID)
  for m in $(models); do
    export ALTCELLO_KNOBS=$(knobs $m)
    nice ./onset_tool grid C2 0 ours $D/grid_${m}_C2_0.txt
    nice ./onset_tool grid C2 7 ours $D/grid_${m}_C2_7.txt
    nice ./onset_tool grid G2 7 ours $D/grid_${m}_G2_7.txt
    nice ./onset_tool grid D3 7 ours $D/grid_${m}_D3_7.txt
    echo "grid $m done"
  done ;;
REV)
  for m in $(models); do
    export ALTCELLO_KNOBS=$(knobs $m)
    for s in G2 C2; do for sd in 1 2 3; do echo "$m $s seed $sd: $(nice ./onset_tool fing bow $s --seed $sd)"; done; done
  done ;;
esac
