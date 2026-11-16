#!/bin/bash
# Guettler maps for the wedge attack (iteration 2): 12 log forces 0.2-5 N, a in {1,2,4,8,16}, beta 0.1, target speed 0.12, force landed 20 ms before the motion.
# Rows: F a t_audio t_slip e28 onset steady lev20 lev50 lev100 (onset_tool grid).   usage: charts/wedge_run.sh   -> out/wedge/map_*.txt
cd "$(dirname "$0")/.." || exit 1
mkdir -p out/wedge
run() { nice ./onset_tool grid "$1" "$2" ours "out/wedge/map_$3.txt" --nf 12 --frange 0.2,5 --accs 1,2,4,8,16 --vend 0.12 --secs 1.5 --tm 0.025 --seed "${4:-0}"; }
# the notes the player really plays (single_notes.txt, vibrato on: an open pitch is stopped on the string below): G2 on C, D3 on G, A3 on D, C2 open;
# the open G2 and A3 maps are the same sweep for the open strings (the scale with vibrato off)
for m in ${MAPS:-G2onC D3onG A3onD C2open G2open A3open}; do
  case $m in
    G2onC) run C2 7 $m;; D3onG) run G2 7 $m;; A3onD) run D3 7 $m;; C2open) run C2 0 $m;; G2open) run G2 0 $m;; A3open) run A3 0 $m;;
  esac
done
