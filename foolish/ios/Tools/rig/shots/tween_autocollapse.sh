#!/bin/bash
# tween_autocollapse.sh - the auto-collapse, measured end to end.
#
#   FOOLISH_SIM=<udid> ios/Tools/rig/shots/tween_autocollapse.sh [NAME]
#
# The auto-collapse fires when a move is STAGED, and only from the expanded
# presentation (MessagesViewController: `presentationStyle != .expanded` stages
# and returns). A seeded board comes up COMPACT - only the new-game and
# name-gate paths ask the host for expanded - so this opens, drags it up, and
# taps the board's own action plank, which is what stages.
#
# The autocollapse tween, from a CLEAN Messages every time - same discipline as
# the marketing flow, and for the same reason: a stale staged bubble or a drawer
# left in the wrong presentation silently changes what is being measured.
set -uo pipefail
: "${FOOLISH_SIM:?set FOOLISH_SIM}"
export FOOLISH_OUT="${FOOLISH_OUT:-$HOME/Downloads/foolish-shots}"
HERE="$(cd "$(dirname "$0")" && pwd)"; RIG="$HERE/../rig.sh"; REPO="$(cd "$HERE/../../../.." && pwd)"
G=$($RIG group); NAME="${1:-autocollapse}"
now(){ date +%s.%N; }; T0=$(now)
mark(){ printf '  %-26s %6.2fs\n' "$1" "$(echo "$(now) - $2" | bc)"; }

t=$(now)
xcrun simctl terminate "$FOOLISH_SIM" com.apple.MobileSMS >/dev/null 2>&1
rm -f "$G/dev.stage" "$G/dev.staged" "$G/dev.claimed"      # nothing pre-staged
# RELAUNCH, AND ONLY STAGE IF THAT IS NOT ENOUGH.
#
# A full `stage` sets a 9:41 status bar, an appearance, and hunts Apple's
# first-run sheets - none of which a MEASUREMENT cares about, and the sheets
# appear once in a simulator's life. What this loop actually needs is Messages
# running and a thread it can enter, so try exactly that and fall back to the
# real thing when it fails. `enter` VERIFIES it reached the thread (`here_is`),
# so a sheet swallowing the tap surfaces here as a failure rather than as a
# quietly wrong take.
xcrun simctl launch "$FOOLISH_SIM" com.apple.MobileSMS >/dev/null 2>&1
if ! $RIG enter >/dev/null 2>&1; then
  echo "  (thread would not open - falling back to a full stage)"
  $RIG stage dark >/dev/null 2>&1
fi
mark "relaunch + enter" $t

t=$(now)
$REPO/c/build/msg_wire_test --goodwait 2 >/tmp/gw.hex 2>/dev/null
H=$(tail -1 /tmp/gw.hex | tr -d '[:space:]')
$RIG killappex >/dev/null 2>&1
printf '%s' "$H" > "$G/dev.fatboard"; printf '0' > "$G/dev.seat"
$RIG open >/dev/null 2>&1
mark "seed + open (compact)" $t

# The drag. A seeded board comes up COMPACT (only the new-game and name-gate
# paths ask the host for expanded) and the auto-collapse is expanded-only, so
# there is nothing to measure without this. It costs ~0.5s now that `expand`
# settles on the drawer instead of sleeping a flat 3s.
t=$(now); $RIG expand >/dev/null 2>&1; mark "expand" $t
echo "  drawer $(python3 "$REPO/ios/Tools/rig/lib/"ui.py top)  bars $(python3 "$REPO/ios/Tools/rig/lib/"ui.py bars)"

$RIG tween "$NAME" -- $RIG goodtap
# TEAR DOWN. A measurement run leaves an appex holding a seeded board and a
# Messages full of state that the NEXT run would have to undo anyway - and an
# extension left running is CPU contending with whatever is measured next.
t=$(now)
$RIG killappex >/dev/null 2>&1
xcrun simctl terminate "$FOOLISH_SIM" com.apple.MobileSMS >/dev/null 2>&1
mark "teardown" $t
printf '  %-26s %6.2fs\n' "WHOLE LOOP" "$(echo "$(now) - $T0" | bc)"
