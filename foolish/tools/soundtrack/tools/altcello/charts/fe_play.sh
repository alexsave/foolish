#!/bin/bash
# Helmholtz band at beta 0.1, v_B 0.1 m/s, 32 forces, D3 and A3, for the three bodies (iter2_run.sh FINE settings), serially.
cd "$(dirname "$0")/.." || exit 1
D=/Users/alex/Music/foolish_cities_v5/alt_cello/fe_body
mkdir -p "$D"
for pair in "stat:" "a:fe=1" "b:fe=2"; do
  tag=${pair%%:*}; knobs=${pair#*:}
  for s in D3 A3; do
    ALTCELLO_KNOBS="$knobs" nice ./sweep_tool ours $s 0.1 0.03 5 32 6 "$D/play_${tag}_$s.txt" --body --tail 0.5 --vb 0.1 > "$D/play_${tag}_$s.log"
    echo "$tag $s: $(grep SUMMARY "$D/play_${tag}_$s.log" | head -1)"
  done
done
