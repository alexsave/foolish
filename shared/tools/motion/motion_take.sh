#!/bin/bash
# motion_take.sh MOVIE OUT.tbl - every composited frame of a filmed take through
# the ruler finder (motion find), decoded once by ffmpeg and piped as raw RGB,
# with each frame's presentation time from ffprobe.
set -euo pipefail
MOV="${1:?movie}"; OUT="${2:?out.tbl}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/build/motion"
[ -x "$BIN" ] || make -s -C "$HERE" >/dev/null
read -r W H < <(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$MOV" | tr ',' ' ')
T="$(mktemp)"
ffprobe -v error -select_streams v:0 -show_entries frame=pts_time -of csv=p=0 "$MOV" | tr -d ',' | sort -g > "$T"
ffmpeg -v error -i "$MOV" -fps_mode passthrough -f rawvideo -pix_fmt rgb24 - \
  2> >(grep -v -e "non monotonically increasing dts" -e "Last message repeated" >&2) | "$BIN" find --size "${W}x${H}" --times "$T" > "$OUT"
rm -f "$T"
