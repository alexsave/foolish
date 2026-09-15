#!/bin/bash
# tween_loop.sh - MANY auto-collapse measurements from one setup.
#
#   FOOLISH_SIM=<udid> ios/Tools/rig/shots/tween_loop.sh [N] [NAME]
#
# `tween_autocollapse.sh` is one RUN: relaunch, stage, seed, open, expand,
# measure, tear down. Most of that is setting a board up, and the board does not
# change between measurements - so a LOOP should pay for it once.
#
# THE CYCLE, and the thing that had to be measured to get it right: only ONE of
# the two planks collapses. Good stages a move and the surface auto-collapses;
# Undo un-stages it and the surface STAYS PUT. Tried the other way round first
# and the even iterations came back `829.7 -> 829.7` with no tween at all, at
# 21.9s each - `goodtap` waits for Messages to offer Send, and an undo never
# produces one, so every second pass burned the whole poll budget proving it.
#
# So: expand, UNDO un-measured (cheap, nothing to film), then GOOD under the
# camera. One drag and two taps an iteration, one measurement out of it.
set -uo pipefail
: "${FOOLISH_SIM:?set FOOLISH_SIM}"
export FOOLISH_OUT="${FOOLISH_OUT:-$HOME/Downloads/foolish-shots}"
HERE="$(cd "$(dirname "$0")" && pwd)"; RIG="$HERE/../rig.sh"
REPO="$(cd "$HERE/../../../.." && pwd)"
N="${1:-6}"; NAME="${2:-tweenloop}"
G=$("$RIG" group)
now(){ date +%s.%N; }
T0=$(now); mark(){ printf '  %-26s %6.2fs\n' "$1" "$(echo "$(now) - $2" | bc)"; }

# ---- setup, ONCE ----------------------------------------------------------
t=$(now)
xcrun simctl terminate "$FOOLISH_SIM" com.apple.MobileSMS >/dev/null 2>&1
rm -f "$G/dev.stage" "$G/dev.staged" "$G/dev.claimed"
xcrun simctl launch "$FOOLISH_SIM" com.apple.MobileSMS >/dev/null 2>&1
"$RIG" enter >/dev/null 2>&1 || "$RIG" stage dark >/dev/null 2>&1
"$REPO/c/build/msg_wire_test" --goodwait 2 >/tmp/tl.hex 2>/dev/null
H=$(tail -1 /tmp/tl.hex | tr -d '[:space:]')
"$RIG" killappex >/dev/null 2>&1
printf '%s' "$H" > "$G/dev.fatboard"; printf '0' > "$G/dev.seat"
"$RIG" open >/dev/null 2>&1
mark "setup (once)" $t

# ---- and then just keep going ---------------------------------------------
# The plank, tapped WITHOUT waiting for a Send that is not coming.
undotap() {
  local W H y
  read -r W H < <(python3 "$REPO/ios/Tools/rig/lib/ax.py" screen)
  y=$(python3 "$REPO/ios/Tools/rig/lib/ui.py" bars | python3 -c "
import sys, ast
b = ast.literal_eval(sys.stdin.read().split('BARS ')[1]); print(b[-1][0] if b else -1)")
  [ "$y" = "-1" ] && return 1
  "$RIG" tap $((W * 4 / 5)) "$y" 0.6 >/dev/null 2>&1
}

for i in $(seq 1 "$N"); do
  t=$(now)
  "$RIG" expand >/dev/null 2>&1
  [ "$i" -gt 1 ] && undotap          # the first pass has nothing staged yet
  e=$(echo "$(now) - $t" | bc)
  out=$("$RIG" tween "${NAME}_$i" -- "$RIG" goodtap 2>&1)
  tw=$(echo "$out" | grep -E '^tween ' | sed 's/^ *//')
  if [ -z "$tw" ]; then
    printf '  %-2s reset %5.2fs | !! NO MOTION - nothing collapsed here\n' "$i" "$e"
  else
    printf '  %-2s reset %5.2fs | %s | %s\n' "$i" "$e" "$tw" \
      "$(echo "$out" | grep -E '^  TOTAL' | sed 's/^ *//;s/  */ /g')"
  fi
done
printf '  %-26s %6.2fs   (%s measurements)\n' "WHOLE LOOP" "$(echo "$(now) - $T0" | bc)" "$N"
"$RIG" killappex >/dev/null 2>&1
xcrun simctl terminate "$FOOLISH_SIM" com.apple.MobileSMS >/dev/null 2>&1
