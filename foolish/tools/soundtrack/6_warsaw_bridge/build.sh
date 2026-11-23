#!/bin/bash
# build.sh <score.txt> <out.wav> : compile the cello bridge renderer if needed, render one score (TEMPO 60, part CELLO), write the wav
# The score format is in ../engine/scoretxt.h; an example is example_score.txt (a rising line under an 8-bar piano introduction).
set -e
B="$(cd "$(dirname "$0")" && pwd)"
IN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
OUT="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
cd "$B"
[ -x score ] && [ score -nt score.c ] || nice clang -O2 -march=native -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -o score score.c
export ENGINE="$B/../engine"
[ "$IN" = "$B/score.txt" ] || cp "$IN" score.txt
rm -rf out; mkdir -p out
nice ./score full out > out/render.log 2>&1 || { tail -20 out/render.log; exit 1; }
cp out/full.wav "$OUT"
