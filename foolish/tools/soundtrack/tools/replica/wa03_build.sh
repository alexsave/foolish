#!/bin/bash
# wa03_build.sh NOTES.tsv OUTDIR [cello=lyria|lyria_vib|vsco|vsco_vib|none] [piano=salamander]
# Builds replica/wa03_play.c once, writes OUTDIR/score.txt from the notes and renders OUTDIR/full.wav (-16 LUFS).  Env: HALL_RT HALL_SEND HALL_DB CELLO_DB PIANO_DB.
set -e
T="$(cd "$(dirname "$0")/../.." && pwd)"
NOTES=$1; OUT=$2; shift 2
for a in "$@"; do case $a in cello=*) export CELLO=${a#cello=};; piano=*) export PIANO=${a#piano=};; esac; done
mkdir -p "$OUT"
nice clang -O2 -w -DMANDIR="\"$T/samples/manifests/\"" -o "$OUT/wa03_play" "$T/replica/wa03_play.c" -lm
python3 -I "$T/tools/replica/wa03_events.py" "$NOTES" "$OUT/score.txt"
(cd "$OUT" && SAMPLES=${SAMPLES:-/Users/alex/Dev/soundtrack-samples} ENGINE="$T/engine" nice ./wa03_play full . 2>&1 | tail -4)
