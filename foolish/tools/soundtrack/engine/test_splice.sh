#!/bin/bash
# test_splice.sh: splice joins two tones with an equal-power crossfade, level-matches them, and masters to -16 LUFS.
# Passes when the level through the join stays within 1 dB of the level either side, the length is right, and the
# master reads -16.0 +-0.1 LUFS under -1.3 dBTP. (Mutation checked: with the fade-out removed the join rises 3 dB and this fails; with the snap search replaced by the asked time the snap check fails.)
set -e
D="$(cd "$(dirname "$0")" && pwd)"; T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
clang -O2 -w -o "$T/splice" "$D/splice.c"
ffmpeg -v error -f lavfi -i "sine=f=440:r=48000:d=10" -af "volume=-6dB" -ac 2 -c:a pcm_s24le "$T/a.wav"
ffmpeg -v error -f lavfi -i "sine=f=657:r=48000:d=10" -af "volume=-20dB" -ac 2 -c:a pcm_s24le "$T/b.wav"
printf 'SRC a %s\nSRC b %s\nCLIP a 0 10\nCLIP b 0 10 OVER 2\n' "$T/a.wav" "$T/b.wav" > "$T/e.txt"
"$T/splice" "$T/o.wav" "$T/e.txt" "$T/s.txt" 2> "$T/log"
rms() { ffmpeg -v info -ss "$1" -t "$2" -i "$T/o.wav" -af astats=measure_overall=RMS_level:measure_perchannel=0 -f null - 2>&1 | sed -n 's/.*RMS level dB: //p' | tail -1; }
A=$(rms 3 2); M=$(rms 8.5 1); B=$(rms 13 2)
DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$T/o.wav")
I=$(ffmpeg -nostats -i "$T/o.wav" -af ebur128=peak=true -f null - 2>&1 | sed -n 's/^ *I: *\(-[0-9.]*\) LUFS/\1/p' | tail -1)
TP=$(ffmpeg -nostats -i "$T/o.wav" -af ebur128=peak=true -f null - 2>&1 | sed -n 's/^ *Peak: *\(-[0-9.]*\) dBFS/\1/p' | tail -1)
# SNAP: a click track (one click every 0.5 s); cuts asked for 80 ms off a click must land 4 ms before it
ffmpeg -v error -f lavfi -i "aevalsrc='0.8*sin(2*PI*90*t)*exp(-40*mod(t,0.5))':s=48000:d=10" -ac 2 -c:a pcm_s24le "$T/c.wav"
printf 'SRC c %s\nCLIP c 2.08 5.93 SNAPIN 0.15 SNAPOUT 0.15\n' "$T/c.wav" > "$T/e2.txt"
"$T/splice" "$T/o2.wav" "$T/e2.txt" 2> "$T/log2"
SI=$(sed -n 's/^snap in .* -> //p' "$T/log2"); SO=$(sed -n 's/^snap out .* -> //p' "$T/log2")
echo "before $A dB, join $M dB, after $B dB, length $DUR s, $I LUFS, $TP dBTP, snap in $SI out $SO"
python3 -I -c "
import sys
a,m,b,d,i,tp,si,so=map(float,sys.argv[1:])
ok = abs(m-a)<1.0 and abs(m-b)<1.0 and abs(a-b)<1.0 and abs(d-18.3)<0.1 and abs(i+16)<0.1 and tp<=-1.3 and abs(si-1.996)<0.004 and abs(so-5.996)<0.004
print('PASS' if ok else 'FAIL'); sys.exit(0 if ok else 1)" "$A" "$M" "$B" "$DUR" "$I" "$TP" "$SI" "$SO"
