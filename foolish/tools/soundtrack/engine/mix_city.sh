#!/bin/bash
# mix_city.sh: render one city song from its cue sheet and check it.
#   mix_city.sh /path/<root>/cues/<Name>.cue.txt
# Builds splice into <root>/work/bin when it is older than its sources, then writes
#   <root>/wav/<Name>.wav  <root>/cues/<Name>.timeline.tsv  <root>/work/<Name>.seams.tsv  <root>/work/<Name>.render.log
#   <root>/<Name>.m4a (AAC 256 kb/s)
# and prints one line each for the wav and the decoded m4a: duration, LUFS, true peak.
# Exits non-zero if either file is not -16 +-0.3 LUFS, the wav is over -1.3 dBTP, the m4a is over -1.0 dBTP,
# or either file is longer than 900.0 s.
#   mix_city.sh --check-duration SECONDS   only the length check (exit 0 when SECONDS <= 900.0)
set -u
MAX_SECONDS=900.0

within_length() { awk -v d="$1" -v m="$MAX_SECONDS" 'BEGIN { exit !(d <= m) }'; }

if [ "${1:-}" = "--check-duration" ]; then
    within_length "${2:?seconds}"; exit $?
fi

CUE="${1:?usage: mix_city.sh /path/<root>/cues/<Name>.cue.txt}"
[ -f "$CUE" ] || { echo "mix_city: no cue sheet $CUE" >&2; exit 2; }
CUE="$(cd "$(dirname "$CUE")" && pwd)/$(basename "$CUE")"
NAME="$(basename "$CUE" .cue.txt)"
ROOT="$(dirname "$(dirname "$CUE")")"
E="$(cd "$(dirname "$0")" && pwd)"
SHARED="$E/../../../../shared/c"
mkdir -p "$ROOT/work/bin" "$ROOT/wav"
BIN="$ROOT/work/bin/splice"
LOG="$ROOT/work/$NAME.render.log"

stale=0
[ -x "$BIN" ] || stale=1
for f in "$E/splice.c" "$E/mix.h" "$E/dsp.h" "$SHARED/rng.h"; do [ "$f" -nt "$BIN" ] && stale=1; done
if [ "$stale" = 1 ]; then
    clang -O2 -Wall -o "$BIN" "$E/splice.c" -framework AudioToolbox -framework CoreFoundation || { echo "mix_city: build failed" >&2; exit 1; }
fi

WAV="$ROOT/wav/$NAME.wav"; TL="$ROOT/cues/$NAME.timeline.tsv"; SEAMS="$ROOT/work/$NAME.seams.tsv"; M4A="$ROOT/$NAME.m4a"
rm -f "$WAV" "$M4A"
"$BIN" "$CUE" "$WAV" "$TL" "$SEAMS" 2> "$LOG" || { echo "mix_city: splice failed, see $LOG" >&2; tail -5 "$LOG" >&2; exit 1; }
afconvert -f m4af -d aac -b 256000 "$WAV" "$M4A" || { echo "mix_city: afconvert failed" >&2; exit 1; }

# measure FILE LABEL: prints one line, sets M_DUR M_I M_TP
measure() {
    local out
    out="$(ffmpeg -nostats -nostdin -i "$1" -af ebur128=peak=true -f null - 2>&1)"
    M_I="$(printf '%s\n' "$out" | awk '/^ *I: .*LUFS/ {v=$2} END {print v}')"
    M_TP="$(printf '%s\n' "$out" | awk '/^ *Peak: .*dBFS/ {v=$2} END {print v}')"
    M_DUR="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$1")"
    [ -n "$M_I" ] && [ -n "$M_TP" ] && [ -n "$M_DUR" ] || { echo "mix_city: could not measure $1" >&2; exit 1; }
    printf '%s  duration %.3f s  %s LUFS  %s dBTP\n' "$2" "$M_DUR" "$M_I" "$M_TP"
}
fail=0
measure "$WAV" wav; WI=$M_I; WT=$M_TP; WD=$M_DUR
measure "$M4A" m4a; MI=$M_I; MT=$M_TP; MD=$M_DUR
awk -v i="$WI" 'BEGIN { exit !(i >= -16.3 && i <= -15.7) }' || { echo "mix_city: wav is $WI LUFS, not -16 +-0.3" >&2; fail=1; }
awk -v i="$MI" 'BEGIN { exit !(i >= -16.3 && i <= -15.7) }' || { echo "mix_city: m4a is $MI LUFS, not -16 +-0.3" >&2; fail=1; }
awk -v t="$WT" 'BEGIN { exit !(t <= -1.3) }' || { echo "mix_city: wav true peak $WT dBTP is over -1.3" >&2; fail=1; }
awk -v t="$MT" 'BEGIN { exit !(t <= -1.0) }' || { echo "mix_city: m4a true peak $MT dBTP is over -1.0" >&2; fail=1; }
within_length "$WD" || { echo "mix_city: wav is $WD s, over $MAX_SECONDS" >&2; fail=1; }
within_length "$MD" || { echo "mix_city: m4a is $MD s, over $MAX_SECONDS" >&2; fail=1; }
exit $fail
