#!/bin/bash
# wa03_build.sh NOTES.tsv OUTDIR [cello=lyria|lyria_vib|vsco|vsco_vib|none] [piano=salamander]
# Builds replica/wa03_play.c once, writes OUTDIR/score.txt from the notes and renders OUTDIR/full.wav (-16 LUFS).  Env: HALL_RT HALL_SEND HALL_DB CELLO_DB PIANO_DB.
set -e
T="$(cd "$(dirname "$0")/../.." && pwd)"
NOTES=$1; OUT=$2; shift 2
for a in "$@"; do case $a in cello=*) export CELLO=${a#cello=};; piano=*) export PIANO=${a#piano=};; esac; done
# defaults fitted on the clip (wa03_eqloop.py, third-octave long-term spectrum, lts_mad_db 4.0 to 1.2): the Lyria cello set carries hiss above 6 kHz,
# and the master curve takes the 3 to 8 kHz excess of the Salamander piano and Lyria cello out
export CELLO_EQ=${CELLO_EQ-1:6000:0.707:0}
export PIANO_EQ=${PIANO_EQ-}
export MASTER_EQ=${MASTER_EQ-2:40:0.7:0,3:3150:2.2:-6.42,3:4000:2.2:-7.76,3:5000:2.2:-4.71,3:6300:2.2:-11.04,3:8000:2.2:-6.58,3:12500:2.2:0.67}
mkdir -p "$OUT"
nice clang -O2 -w -DMANDIR="\"$T/samples/manifests/\"" -o "$OUT/wa03_play" "$T/replica/wa03_play.c" -lm
python3 -I "$T/tools/replica/wa03_events.py" "$NOTES" "$OUT/score.txt"
(cd "$OUT" && SAMPLES=${SAMPLES:-/Users/alex/Dev/soundtrack-samples} ENGINE="$T/engine" nice ./wa03_play full . 2>&1 | tail -4)
