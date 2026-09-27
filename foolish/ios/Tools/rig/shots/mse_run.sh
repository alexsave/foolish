#!/bin/bash
# mse_run.sh - build, measure N auto-collapses, and score them. ONE command per
# experiment, because an experiment that takes three commands gets run wrong.
#
#   FOOLISH_SIM=<udid> ios/Tools/rig/shots/mse_run.sh <name> [N] [--nobuild]
#
# Leaves  ~/Downloads/mse/<name>.png  (the plot),  <name>.json  (the curve) and
# the per-take CSVs under ~/Downloads/mse/film/<name>_*/edge.csv, so two runs can
# be replotted against each other without re-shooting either.
set -uo pipefail
: "${FOOLISH_SIM:?set FOOLISH_SIM}"
HERE="$(cd "$(dirname "$0")" && pwd)"; RIG="$HERE/../rig.sh"
REPO="$(cd "$HERE/../../../.." && pwd)"
NAME="${1:?name this run}"; N="${2:-20}"
export FOOLISH_OUT="${FOOLISH_OUT:-$HOME/Downloads/mse}"
if [ "${3:-}" != "--nobuild" ]; then
  "$RIG" build >/dev/null 2>&1 || { echo "BUILD FAILED"; "$RIG" build 2>&1 | tail -30; exit 1; }
fi
rm -rf "$FOOLISH_OUT/film/${NAME}_"*
"$HERE/tween_loop.sh" "$N" "$NAME" || true
python3 "$REPO/ios/Tools/rig/lib/mse.py" "$FOOLISH_OUT/film/${NAME}_"*/edge.csv \
  --label "$NAME" --plot "$FOOLISH_OUT/$NAME.png" --json "$FOOLISH_OUT/$NAME.json"
