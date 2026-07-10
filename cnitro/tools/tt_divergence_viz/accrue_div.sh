#!/usr/bin/env bash
# Accumulating, seed-keyed divergence measurement for ONE cell (default octogen
# pc2 — the binding TT consumer). Each batch replays a fresh set of seeds through
# a TT22 baseline and each candidate CD_TT_BITS, diffs the per-game move hash
# (GAME_SIG), and APPENDS one seed-keyed record per (seed, bits) — deduped on
# seed, so batches accumulate without ever double-counting. Then it recomputes
# the per-bits divergence rate, writes data/divergence.json, and rebuilds the HTML.
#
#   tools/tt_divergence_viz/accrue_div.sh [games] [BITS...]
#   tools/tt_divergence_viz/accrue_div.sh 400            # +400 seeds, default bits
#   BOT=octogen PC=2 tools/tt_divergence_viz/accrue_div.sh 400 10 11 12 13 14
#
# Small batches are the point: run it again and again, the curve tightens.
set -euo pipefail
trap 'echo "[error] line $LINENO: \"$BASH_COMMAND\" exited $?" >&2' ERR
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CNITRO="$(cd "$HERE/../.." && pwd)"
CC=${CC:-cc}
BOT=${BOT:-octogen}; PC=${PC:-2}; BASE=${BASE:-22}
SEED0=${SEED0:-500000}
J=${J:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )}
N=${1:-400}; [ "$#" -gt 0 ] && shift || true
if [ "$#" -gt 0 ]; then BITS=("$@"); else BITS=(8 9 10 11 12 13 14); fi
DIV="$HERE/data/div"; mkdir -p "$DIV"
throttle() { while [ "$(jobs -rp | wc -l)" -ge "$J" ]; do wait "$(jobs -rp | head -1)"; done; }

echo "[div] cell=${BOT}_pc${PC} baseline=TT${BASE} candidates=${BITS[*]} +${N} seeds  jobs=$J"

# --- binaries (GAME_SIG move-hash) for baseline + candidates, cached in tool dir
build_bin() { local b=$1 out="$HERE/eval_sig${b}"
  [ -x "$out" ] && [ "$out" -nt "$CNITRO/src/cordite_sim.c" ] && return
  local CORE; CORE=$(cd "$CNITRO" && make -s print-core)
  ( cd "$CNITRO" && $CC -O2 -ffast-math -Isrc -Wno-deprecated-declarations \
      -DCD_TT_BITS="$b" $CORE src/main_eval.c -o "$out" -lm )
}
echo "[div] building binaries..."
for b in "$BASE" "${BITS[@]}"; do build_bin "$b" & throttle; done; wait

# --- fresh, disjoint seeds: continue past the max seed any accumulator has used
maxseed=$( { cat "$DIV/${BOT}_pc${PC}_tt"*.div 2>/dev/null || true; } | awk '{if($1>m)m=$1}END{print m+0}')
start=$(( maxseed >= SEED0 ? maxseed + 1 : SEED0 ))
per=$(( N / J )); rem=$(( N - per*J ))
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# one binary over the whole batch, seed-sharded -> seed<TAB>hash, sorted by seed
run_bin() { local b=$1 i off g
  off=$start
  for (( i=0; i<J; i++ )); do
    g=$per; [ "$i" -lt "$rem" ] && g=$(( per+1 )); [ "$g" -gt 0 ] || { continue; }
    GAME_SIG=1 CD_BUDGET=prod CD_RACE=1 CD_RACE_C=75 "$HERE/eval_sig${b}" \
      --strategy="$BOT" --opp=handwritten --players="$PC" --games="$g" --seed-start="$off" \
      2>/dev/null | grep '^SIG' | awk '{print $2"\t"$3}' > "$work/h_${b}_${i}.txt" &
    off=$(( off+g ))
    throttle
  done
  wait
  sort -n -k1,1 "$work"/h_${b}_*.txt > "$work/hash_${b}.txt"
}
echo "[div] measuring $(( ${#BITS[@]}+1 )) x $N games from seed $start ..."
t0=$SECONDS
run_bin "$BASE"
for b in "${BITS[@]}"; do run_bin "$b"; done
echo "[div] runs done ($(( SECONDS-t0 ))s); comparing + appending"

# --- per bits: diverged = (candidate hash != baseline hash), keyed by seed
for b in "${BITS[@]}"; do
  acc="$DIV/${BOT}_pc${PC}_tt${b}.div"; touch "$acc"
  join -t$'\t' "$work/hash_${BASE}.txt" "$work/hash_${b}.txt" \
    | awk -F'\t' '{print $1" "($2!=$3?1:0)}' >> "$acc"
  # dedup on seed (deterministic game)
  sort -n -k1,1 -u "$acc" -o "$acc"
  n=$(wc -l < "$acc"); d=$(awk '$2==1{c++}END{print c+0}' "$acc")
  printf "  TT%-3s %6d/%-7d  p=%.5f\n" "$b" "$d" "$n" "$(awk -v d=$d -v n=$n 'BEGIN{print (n?d/n:0)}')"
done

node "$HERE/div.mjs" > "$HERE/data/divergence.json"
node "$HERE/build.mjs" >/dev/null
echo "[div] wrote data/divergence.json + rebuilt HTML — refresh the browser"
