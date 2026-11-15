#!/bin/bash
# render_one.sh ENGINE_DIR OUTDIR INST LOOPMODE SEND REL NOTE VEL [extra clang flags]: one held note of 5 s alone.
# ENGINE_DIR holds orch.h. Writes OUTDIR/full.wav and OUTDIR/trace.txt (stderr TRACE lines).
set -e
E="$1"; O="$2"; shift 2
mkdir -p "$O"; cd "$O"
INSTV=$1; LM=$2; SEND=$3; REL=$4; NOTE=$5; VEL=$6; shift 6
cp /Users/alex/Dev/foolish-wt-city-songs-v5/foolish/tools/soundtrack/lyria_instruments_demo/loop_check/one_note.c .
nice clang -O2 -w -DENGINE_ORCH="\"$E/orch.h\"" "$@" -o one one_note.c
INST=$INSTV LOOPMODE=$LM SEND=$SEND REL=$REL NOTE=$NOTE VEL=$VEL SAMPLES=/Users/alex/Dev/soundtrack-samples ENGINE="$E" nice ./one full out 2> trace.txt >/dev/null
cp out/full.wav full.wav
