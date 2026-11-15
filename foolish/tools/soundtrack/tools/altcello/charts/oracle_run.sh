#!/bin/bash
# The oracle experiment (iteration 1): all Schelleng sweeps, serially. Usage: charts/oracle_run.sh PART   (A | B1 | B2 | B4 | WAVE)
# Output text files in /Users/alex/Music/foolish_cities_v5/alt_cello/oracle (rows of sweep_tool, summaries in *.log).
cd "$(dirname "$0")/.." || exit 1
D=/Users/alex/Music/foolish_cities_v5/alt_cello/oracle
mkdir -p "$D"
T=./sweep_tool
case "$1" in
A) # the C1 default instrument (body on, all strings), beta 0.1, v_B 0.1, 20 forces 0.02..5 N, 6 s
  for s in D3 G2 C2 A3; do nice $T ours $s 0.1 0.02 5 20 6 "$D/A_$s.txt" --body > "$D/A_$s.log"; grep SUMMARY "$D/A_$s.log"; done ;;
B1) for b in 0.05 0.10 0.20; do nice $T oracle D3 $b 0.02 5 20 6 "$D/B1_D3_b$b.txt" > "$D/B1_D3_b$b.log"; grep "SUMMARY\|^oracle" "$D/B1_D3_b$b.log"; done
  for s in G2 C2 A3; do nice $T oracle $s 0.10 0.02 5 20 6 "$D/B1_${s}_b0.10.txt" > "$D/B1_${s}_b0.10.log"; grep "SUMMARY\|^oracle" "$D/B1_${s}_b0.10.log"; done ;;
B2) # string only (no body), D3; configurations i..vi
  declare -a NAMES=(i ii iii iv v vi)
  declare -a OPTS=("" "--no-noise" "--no-torsion" "--pt-bow" "--stiff 0.1" "--no-noise --no-torsion --pt-bow --stiff 0.1")
  for b in 0.05 0.10 0.20; do for k in 0 1 2 3 4 5; do
    f="$D/B2_D3_b${b}_${NAMES[$k]}"
    nice $T ours D3 $b 0.02 5 20 6 "$f.txt" ${OPTS[$k]} > "$f.log"; grep SUMMARY "$f.log"
  done; done ;;
B4) F=${2:-0.67082}
  for m in ours oracle; do nice $T onset D3 0.1 $F 4 --model $m; done ;;
WAVE) F=${2:-0.67082}
  nice $T wave D3 0.1 $F 4 "$D/wave_oracle.txt" --model oracle
  nice $T wave D3 0.1 $F 4 "$D/wave_ours.txt" --model ours ;;
esac
