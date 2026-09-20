#!/bin/bash
# THE EXTRACTION WINDOW of a tween take: its frames, 1:1, and one time per frame.
#
#   window.sh <movie> <outdir> <ss> <t> <crop-width>
#
# `rig.sh tween` measures only [ss, ss+t) of the movie, cropped to the left
# <crop-width> pixels; see `cmd_tween` for why each number is what it is. It
# lives here, apart from rig.sh, so `test_window.py` runs this exact code on a
# movie whose every frame says what time it is.
#
# THE TIMES COME FROM THE SAME RUN THAT WRITES THE FRAMES. They used to be
# ffprobe's times for the WHOLE movie, lined up with the window's frames from
# the END - right only when the window reaches the end, which this one never
# does, so every frame of every tween was placed ~315ms late. Picking the
# window's times back out of the movie's is no better: where `-ss`/`-t` cut a
# variable-rate movie with B-frames is not a rule worth re-deriving. So the cut
# is `trim` (start inclusive, end exclusive, on the movie's own timestamps -
# `-copyts` keeps them, and `-ss` still seeks so the lead is not decoded), and
# `showinfo` after it logs the time of exactly the frames that are written.
set -euo pipefail
mov="$1" d="$2" ss="$3" t="$4" crop="$5"
end=$(echo "$ss + $t" | bc)
ffmpeg -hide_banner -nostats -v info -ss "$ss" -copyts -i "$mov" \
       -vf "trim=start=$ss:end=$end,crop=$crop:ih:0:0,showinfo" \
       -fps_mode passthrough "$d/f%05d.ppm" 2>"$d/ffmpeg.err" || {
  cat "$d/ffmpeg.err" >&2; exit 1; }
grep -o 'showinfo.* pts_time:[^ ]*' "$d/ffmpeg.err" | sed 's/.*pts_time://' > "$d/times.txt"
n=$(find "$d" -maxdepth 1 -name 'f[0-9]*.ppm' | wc -l | tr -d ' ')
m=$(wc -l < "$d/times.txt" | tr -d ' ')
[ "$n" = "$m" ] || { echo "window.sh: $n frames written but $m times logged" >&2; exit 1; }
