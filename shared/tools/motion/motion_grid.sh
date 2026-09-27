#!/bin/bash
# motion_grid.sh MOVIE OUT.grid [FROM TO] - a recording with no ruler (a device's, a Release build's)
# through `motion grid`: per frame the drawer's top, the board's heavy grid lines, its centre and side, and
# the centre's offset from the drawer's middle, then the largest one-frame steps (last line).
set -euo pipefail
MOV="${1:?movie}"; OUT="${2:?out.grid}"; FROM="${3:--1}"; TO="${4:-1e9}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/build/motion"
[ -x "$BIN" ] || make -s -C "$HERE" >/dev/null
read -r W H < <(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$MOV" | tr ',' ' ')
T="$(mktemp)"
ffprobe -v error -select_streams v:0 -show_entries frame=pts_time -of csv=p=0 "$MOV" | tr -d ',' | sort -g > "$T"
ffmpeg -v error -i "$MOV" -fps_mode passthrough -f rawvideo -pix_fmt rgb24 - \
  2> >(grep -v -e "non monotonically increasing dts" -e "Last message repeated" >&2) \
  | "$BIN" grid --size "${W}x${H}" --times "$T" --from "$FROM" --to "$TO" > "$OUT"
rm -f "$T"
tail -1 "$OUT"
