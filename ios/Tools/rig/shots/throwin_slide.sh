#!/bin/bash
# throwin_slide.sh - does a throw-in SLIDE the table, or jump it? Measured.
#
#   FOOLISH_SIM=<udid> ios/Tools/rig/shots/throwin_slide.sh [NAME] [SEATS]
#
# LOCAL ONLY, NEVER CI. It needs a booted simulator, a DEBUG build installed
# (`rig.sh build`) and Messages; run it by hand, e.g. weekly, to catch the
# table jumping again. Exit 0 smooth, 1 a jump, 2 nothing measured.
#
# Owner, on build 71: "when I throw in, sometimes the cards on the table jump to
# the position that they're gonna be in instead of smoothly transitioning." The
# board draws a coloured square at the centre of every table pair under
# `dev.ruler` (CollapseRuler `tableSquare`), and `lib/tablesquares.py` follows
# each one through every composited frame of the throw-in.
#
# The board: `--goodwait`, which is two covered pairs with every attacker but us
# having said good - so we are the attacker, and the searcher guarantees we hold
# a card whose value is on the table; `rig.sh throwin select` finds it on screen.
set -uo pipefail
: "${FOOLISH_SIM:?set FOOLISH_SIM}"
export FOOLISH_OUT="${FOOLISH_OUT:-$HOME/Downloads/foolish-shots}"
HERE="$(cd "$(dirname "$0")" && pwd)"; RIG="$HERE/../rig.sh"; LIB="$HERE/../lib"
REPO="$(cd "$HERE/../../../.." && pwd)"
NAME="${1:-throwin}"; SEATS="${2:-2}"
G=$("$RIG" group)
D="$FOOLISH_OUT/film/$NAME"; rm -rf "$D"; mkdir -p "$D"

# ---- the board ------------------------------------------------------------
"$RIG" ruler on >/dev/null
rm -f "$G/dev.stage" "$G/dev.staged" "$G/dev.claimed"
# A CLEAN Messages, as tween_autocollapse.sh does. A running Messages keeps the
# extension's bundle path from before the last `rig.sh build`, and a reinstall
# moves it: the drawer then opens blank ("LaunchServices cannot find plugin")
# and nothing ever claims the seed. The transcript is lost, which a measurement
# does not need.
xcrun simctl terminate "$FOOLISH_SIM" com.apple.MobileSMS >/dev/null 2>&1
xcrun simctl launch "$FOOLISH_SIM" com.apple.MobileSMS >/dev/null 2>&1
"$RIG" enter >/dev/null 2>&1 || { echo "could not enter a conversation" >&2; exit 2; }
# The last run's throw-in is still STAGED in the compose field - this script
# leaves one behind every time - and with the keyboard up the app menu has no
# Foolish in it to open.
"$RIG" clearstage >/dev/null 2>&1 || true
SEAT=0 "$RIG" seed goodwait "$SEATS" | tail -1
"$RIG" killappex >/dev/null 2>&1
"$RIG" open >/dev/null 2>&1 || { echo "could not open the extension" >&2; exit 2; }

# ---- choose the throw-in, unpressed ----------------------------------------
read -r PX PY < <("$RIG" throwin select) || { echo "no throw-in on this board" >&2; exit 2; }

# ---- film the throw-in ----------------------------------------------------
xcrun simctl io "$FOOLISH_SIM" recordVideo --codec h264 --force "$D/take.mp4" >/dev/null 2>&1 &
rec=$!
sleep 0.6
"$RIG" tap "$PX" "$PY" 0 >/dev/null 2>&1
sleep 2.2
kill -INT $rec 2>/dev/null || true
i=0
while [ $i -lt 60 ]; do
  a=$(stat -f%z "$D/take.mp4" 2>/dev/null || echo 0); sleep 0.15
  b=$(stat -f%z "$D/take.mp4" 2>/dev/null || echo 0)
  [ "$a" = "$b" ] && [ "$a" != 0 ] && break
  i=$((i + 1))
done
# The whole width: the table is centred, not at the leading edge.
"$LIB/window.sh" "$D/take.mp4" "$D" 0.3 2.6 "$(ffprobe -v error -select_streams v:0 \
  -show_entries stream=width -of csv=p=0 "$D/take.mp4")" || exit 2

# ---- measure --------------------------------------------------------------
python3 "$LIB/tablesquares.py" "$D" --csv "$D/squares.csv"
rc=$?
"$RIG" clearstage >/dev/null 2>&1 || true
echo "$D"
exit $rc
