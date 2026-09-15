#!/bin/bash
# mse_sweep.sh - one knob, several values, the MSE of each.
#
#   FOOLISH_SIM=<udid> ios/Tools/rig/shots/mse_sweep.sh lead 6 0.000 0.004 0.010 0.016
#   FOOLISH_KNOBS="lead=0.004" ios/Tools/rig/shots/mse_sweep.sh hz 6 60 120 240
#
# FOOLISH_KNOBS holds the OTHER knobs steady: `dev.collapse` is the whole file,
# so sweeping `hz` without it silently puts `lead` back to its shipping value
# and compares two things at once.
#
# `dev.collapse` is read ONCE per appex process, so a point costs a whole
# setup - which is why this writes the file and then lets tween_loop.sh do its
# own kill/open, rather than trying to change a knob under a live appex.
set -uo pipefail
: "${FOOLISH_SIM:?set FOOLISH_SIM}"
HERE="$(cd "$(dirname "$0")" && pwd)"; RIG="$HERE/../rig.sh"
REPO="$(cd "$HERE/../../../.." && pwd)"
export FOOLISH_OUT="${FOOLISH_OUT:-$HOME/Downloads/mse}"
KNOB="${1:?knob name: lead | hz | resp}"; N="${2:?takes per point}"; shift 2
G=$("$RIG" group)
for v in "$@"; do
  tag="${KNOB}_${v}"
  printf '%s %s=%s ' "${FOOLISH_KNOBS:-}" "$KNOB" "$v" > "$G/dev.collapse"
  echo "=== $KNOB = $v ==============================================="
  rm -rf "$FOOLISH_OUT/film/${tag}_"*
  "$HERE/tween_loop.sh" "$N" "$tag" 2>&1 | grep -E 'tween|NO MOTION|WHOLE'
  python3 "$REPO/ios/Tools/rig/lib/mse.py" "$FOOLISH_OUT/film/${tag}_"*/edge.csv \
    --label "$tag" --json "$FOOLISH_OUT/$tag.json" 2>&1 | grep -E 'MSE|judder|peak|rest'
done
rm -f "$G/dev.collapse"
