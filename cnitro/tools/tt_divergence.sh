#!/usr/bin/env bash
# Per-game divergence rate of a smaller cordite transposition table vs a
# collision-free baseline (TT22). Answers: at CD_TT_BITS=M, in what fraction of
# games does a shipped solver bot play a DIFFERENT move sequence than it would
# with an effectively-infinite table? See docs/WASM_L1_BUDGET.md.
#
#   tools/tt_divergence.sh [bot] [opp] [pcs] [games/pc] [seed] [BITS...]
#
# Defaults: cordite vs handwritten, pc 4,8, 500 games/pc, sizes 13..6.
# Examples:
#   tools/tt_divergence.sh                                   # cordite quick look
#   tools/tt_divergence.sh octogen handwritten 2,4,6,8 4000 99 12 11 10 9 8 7
#   for b in cordite fulminate octogen semtex; do tools/tt_divergence.sh $b; done
#
# Method: two builds print a per-game FNV-1a hash of the protagonist's moves
# (GAME_SIG=1, main_eval.c). Same seeds, same production env
# (CD_BUDGET=prod CD_RACE=1 CD_RACE_C=75). A hash mismatch == that game played
# differently. Needs only a C compiler + make; no wasm toolchain.
set -euo pipefail
cd "$(dirname "$0")/.."                       # -> cnitro
BOT=${1:-cordite}; OPP=${2:-handwritten}; PCS=${3:-4,8}; GAMES=${4:-500}; SEED=${5:-222333}
if [ "$#" -gt 5 ]; then shift 5; BITS=("$@"); else BITS=(13 12 11 10 9 8 7 6); fi
CC=${CC:-cc}
CORE=$(make -s print-core)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
J=${J:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )}

echo "compiler=$CC  jobs=$J  baseline=TT22 (collision-free)  candidates=${BITS[*]}"
build() { $CC -O2 -ffast-math -Isrc -Wno-deprecated-declarations -DCD_TT_BITS="$1" $CORE src/main_eval.c -o "$WORK/eval_$1" -lm; }
# parallel builds
for b in 22 "${BITS[@]}"; do
  ( build "$b" ) & while [ "$(jobs -rp | wc -l)" -ge "$J" ]; do wait -n; done
done; wait

run() { GAME_SIG=1 CD_BUDGET=prod CD_RACE=1 CD_RACE_C=75 \
        "$WORK/eval_$1" --strategy="$BOT" --opp="$OPP" --players="$PCS" \
        --games="$GAMES" --seed-start="$SEED" 2>/dev/null | grep '^SIG'; }

# parallel runs (each eval is single-threaded)
run 22 > "$WORK/base.txt" &
for b in "${BITS[@]}"; do
  ( run "$b" > "$WORK/m_$b.txt" ) & while [ "$(jobs -rp | wc -l)" -ge "$J" ]; do wait -n; done
done; wait

tot=$(wc -l < "$WORK/base.txt")
echo "== $BOT vs $OPP  pc=$PCS  games/pc=$GAMES  seed0=$SEED  total games=$tot =="
printf "  %-6s %-9s %-16s %s\n" "bits" "entries" "diverged" "p(game differs)"
for b in "${BITS[@]}"; do
  d=$(paste "$WORK/base.txt" "$WORK/m_$b.txt" | awk '{if($3!=$7)c++} END{print c+0}')
  awk -v b="$b" -v d="$d" -v t="$tot" 'BEGIN{
    printf "  TT%-4s %-9d %-16s %.6f\n", b, 2^b, d"/"t, (t>0?d/t:0) }'
done
