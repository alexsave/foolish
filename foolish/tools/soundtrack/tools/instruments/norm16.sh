#!/bin/bash
# norm16.sh IN.wav START SECONDS OUT.m4a : cuts [START, START+SECONDS) of IN, measures integrated loudness (ffmpeg ebur128), applies the
# gain that makes it -16 LUFS (plus a peak limiter at -1 dBFS), writes AAC 192k and prints the measured result.
set -e
IN=$1; S=$2; D=$3; OUT=$4
T=$(mktemp -d)
ffmpeg -v error -y -ss "$S" -t "$D" -i "$IN" "$T/cut.wav"
I=$(ffmpeg -nostats -i "$T/cut.wav" -af ebur128 -f null - 2>&1 | grep -A0 " I:" | tail -1 | awk '{print $2}')
G=$(python3 -c "print(-16.0-($I))")
ffmpeg -v error -y -i "$T/cut.wav" -af "volume=${G}dB,alimiter=limit=0.89:level=disabled" -c:a aac -b:a 192k "$OUT"
R=$(ffmpeg -nostats -i "$OUT" -af ebur128 -f null - 2>&1 | grep " I:" | tail -1 | awk '{print $2}')
echo "$OUT: measured before $I, after $R LUFS"
rm -rf "$T"
