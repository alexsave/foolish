#!/bin/bash
# build.sh <city_dir> [full|loop|both]   compiles the score, renders, analyses, plots
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
D="$ROOT/$1"; MODE="${2:-both}"
NAME="$(basename "$D")"
cd "$D"; mkdir -p out
nice clang -O2 -march=native -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -o score score.c
export ENGINE="$ROOT/engine"
ANAL_ARGS="$(cat anal_args.txt 2>/dev/null || true)"
for m in full loop; do
  if [ "$MODE" = both ] || [ "$MODE" = $m ]; then
    nice ./score $m out 2>&1 | tee out/render_$m.log | grep -v "^rendered" || true
    extra=""; [ $m = loop ] && extra="--loop"
    secs=""; [ -f sections_$m.txt ] && secs="$(cat sections_$m.txt)"
    nice "$ROOT/engine/anal" out/$m.wav out/an_$m $ANAL_ARGS --sec "$secs" $extra > /dev/null
    nice python3 -I "$ROOT/engine/plot.py" out/an_$m "$NAME $m" "$secs" > /dev/null
  fi
done
