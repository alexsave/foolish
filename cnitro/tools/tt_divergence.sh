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
#   S=8 tools/tt_divergence.sh octogen handwritten 2 8000 99 16 13   # 1 pc, fast
#   for b in cordite fulminate octogen semtex; do tools/tt_divergence.sh $b; done
#
# Method: two builds print a per-game FNV-1a hash of the protagonist's moves
# (GAME_SIG=1, main_eval.c). Same seeds, same production env
# (CD_BUDGET=prod CD_RACE=1 CD_RACE_C=75). A hash mismatch == that game played
# differently. Needs only a C compiler + make; no wasm toolchain.
#
# Parallelism: seeds are independent, so each table size's games are sharded by
# seed across cores (S shards/size), and base + every candidate use the SAME
# seed partition so the per-game comparison still lines up. This fills every core
# even when you test a single player count / single size. Portable to macOS's
# stock /bin/bash 3.2 (no `wait -n`).
set -euo pipefail
trap 'echo "[error] line $LINENO: \"$BASH_COMMAND\" exited $?" >&2' ERR
cd "$(dirname "$0")/.."                       # -> cnitro
BOT=${1:-cordite}; OPP=${2:-handwritten}; PCS=${3:-4,8}; GAMES=${4:-500}; SEED=${5:-222333}
if [ "$#" -gt 5 ]; then shift 5; BITS=("$@"); else BITS=(13 12 11 10 9 8 7 6); fi
CC=${CC:-cc}
# BASE=<bits> overrides the reference build. Default 22 = effectively collision-
# free (the "infinite table"). Set BASE=16 to compare against TODAY'S PRODUCTION
# table instead — octogen has an irreducible TT-size sensitivity (rare huge-
# endgame games collide even at TT16-19), so TT16 is the decision-relevant
# baseline, not an infinite one.
BASE=${BASE:-22}
CORE=$(make -s print-core)
WORK=$(mktemp -d)
J=${J:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )}
# Seed shards per size. Default: spread ~J jobs across all (size) runs, so a
# single-size run still shards wide. Override with S=<n>.
NSIZES=$(( ${#BITS[@]} + 1 ))
S=${S:-$(( J/NSIZES > 1 ? J/NSIZES : 1 ))}

echo "compiler=$CC  jobs=$J  shards/size=$S  baseline=TT$BASE  candidates=${BITS[*]}"
build() { $CC -O2 -ffast-math -Isrc -Wno-deprecated-declarations -DCD_TT_BITS="$1" $CORE src/main_eval.c -o "$WORK/eval_$1" -lm; }
# portable job cap: no `wait -n` (needs bash>=4.3; absent on macOS /bin/bash 3.2)
throttle() { while [ "$(jobs -rp | wc -l)" -ge "$J" ]; do wait "$(jobs -rp | head -1)"; done; }
t0=$SECONDS
echo "[build] compiling $NSIZES binaries..."
for b in "$BASE" "${BITS[@]}"; do
  throttle
  { build "$b" && echo "[build] TT$b done ($(( SECONDS - t0 ))s elapsed)"; } &
done; wait
echo "[build] all binaries ready ($(( SECONDS - t0 ))s total)"

# one seed shard: $1=bit $2=shard-index $3=games $4=seed-start
run_shard() {
  GAME_SIG=1 CD_BUDGET=prod CD_RACE=1 CD_RACE_C=75 \
    "$WORK/eval_$1" --strategy="$BOT" --opp="$OPP" --players="$PCS" \
    --games="$3" --seed-start="$4" 2>/dev/null | grep --line-buffered '^SIG' \
    > "$WORK/p_${1}_${2}.txt"
}

# heartbeat: total games completed so far across every in-progress shard
heartbeat() {
  while :; do
    sleep 5
    n=$(cat "$WORK"/p_*.txt 2>/dev/null | wc -l | tr -d ' ')
    echo "[run] ${n} games completed so far ($(( SECONDS - t0 ))s elapsed)"
  done
}

per=$(( GAMES / S )); rem=$(( GAMES - per*S ))
t0=$SECONDS
echo "[run] launching $NSIZES sizes x $S seed-shards, $GAMES games/pc each..."
heartbeat & HB_PID=$!
disown
trap 'kill "$HB_PID" 2>/dev/null || true; rm -rf "$WORK"' EXIT
# launch every (size, shard) job; base and candidates share the seed partition
for b in "$BASE" "${BITS[@]}"; do
  off=$SEED
  for (( i=0; i<S; i++ )); do
    g=$per; [ "$i" -lt "$rem" ] && g=$(( per + 1 ))
    [ "$g" -gt 0 ] || { off=$off; continue; }
    throttle
    run_shard "$b" "$i" "$g" "$off" &
    off=$(( off + g ))
  done
done; wait
kill "$HB_PID" 2>/dev/null || true
wait "$HB_PID" 2>/dev/null || true
echo "[run] all runs finished ($(( SECONDS - t0 ))s total)"

# reassemble each size from its shards, in shard order (identical for base +
# candidate, so paste() lines still correspond game-for-game)
reassemble() { local b=$1 i; : > "$WORK/out_$b.txt"
  for (( i=0; i<S; i++ )); do cat "$WORK/p_${b}_${i}.txt" 2>/dev/null >> "$WORK/out_$b.txt" || true; done; }
reassemble "$BASE"
for b in "${BITS[@]}"; do reassemble "$b"; done

tot=$(wc -l < "$WORK/out_$BASE.txt")
echo "== $BOT vs $OPP  pc=$PCS  games/pc=$GAMES  seed0=$SEED  total games=$tot =="
printf "  %-6s %-9s %-16s %s\n" "bits" "entries" "diverged" "p(game differs)"
for b in "${BITS[@]}"; do
  d=$(paste "$WORK/out_$BASE.txt" "$WORK/out_$b.txt" | awk '{if($3!=$7)c++} END{print c+0}')
  awk -v b="$b" -v d="$d" -v t="$tot" 'BEGIN{
    printf "  TT%-4s %-9d %-16s %.6f\n", b, 2^b, d"/"t, (t>0?d/t:0) }'
done
