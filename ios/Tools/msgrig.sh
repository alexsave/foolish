#!/bin/bash
# msgrig.sh - drive the REAL Messages app in the simulator, deterministically.
#
# Why this exists: the iMessage board's collapse defects only reproduce in the
# host app (the harness animates its fake drawer, the real one does not), and
# hand-driven runs were costing an hour each - taps drifted with the layout,
# the deal handed us the defender seat at random, and `cliclick` wedged the
# simulator's touch state. This rig removes all three.
#
#   ios/tools/msgrig.sh setup        fresh seeded game, board open and EXPANDED
#   ios/tools/msgrig.sh move         select a card, tap Attack, film the collapse
#   ios/tools/msgrig.sh film NAME S  record S seconds into NAME.mp4 + frames
#   ios/tools/msgrig.sh shot         one screenshot, downscaled to points
#
# Requirements (one-time):
#   brew install facebook/fb/idb-companion
#   python3.12 -m venv <dir> && <dir>/bin/pip install fb-idb   # 3.14 is broken
#   pip install pillow numpy   (in that venv or system python3)
#
# NEVER use cliclick here: it cannot drag the drawer's grabber, needs window
# focus, and a lost mouse-up leaves a touch stuck down until the sim reboots.
# idb injects HID directly, in DEVICE POINTS (screenshot px / 3 on iPhone 17).
set -euo pipefail

SIM="${FOOLISH_SIM:-EFB2FD39-DD17-4284-9C46-013142226F6F}"
IDB="${FOOLISH_IDB:?set FOOLISH_IDB to the fb-idb binary, e.g. /path/venv/bin/idb}"
WORK="${FOOLISH_WORK:-/tmp/msgrig}"
SEED="${FOOLISH_SEED:-3}"          # 3 deals the CREATOR the first attack at 2p
UI="$(cd "$(dirname "$0")" && pwd)/msgui.py"
mkdir -p "$WORK"

tap()   { "$IDB" ui tap --udid "$SIM" "$1" "$2"; sleep "${3:-1}"; }
swipe() { "$IDB" ui swipe --udid "$SIM" --duration "$1" "$2" "$3" "$4" "$5"; sleep "${6:-1}"; }
shot()  { xcrun simctl io "$SIM" screenshot "$WORK/shot.png" >/dev/null 2>&1; }

# The App Group container holds the dev flags. Its UUID changes on reinstall.
group_dir() {
  for d in ~/Library/Developer/CoreSimulator/Devices/"$SIM"/data/Containers/Shared/AppGroup/*/; do
    id=$(plutil -extract MCMMetadataIdentifier raw "$d/.com.apple.mobile_container_manager.metadata.plist" 2>/dev/null || true)
    [ "$id" = "group.cards.foolish.msg" ] && { echo "$d"; return; }
  done
}

# Wooden buttons are found by COLOUR, never by hard-coded y: the lobby moves
# with the player count, locale and presentation style, and a stale constant
# lands in the Settings gear (which silently switches the app's language).
bar_y() { python3 "$UI" bars | python3 -c "
import sys,ast; b=ast.literal_eval(sys.stdin.read().split('BARS ')[1])
print(b[${1:-0}][0] if b else -1)"; }
drawer_top() { python3 "$UI" top | awk '{print $2}'; }

cmd_setup() {
  xcrun simctl shutdown "$SIM" 2>/dev/null || true; sleep 3
  xcrun simctl boot "$SIM"; sleep 8
  until xcrun simctl list devices booted | grep -q "$SIM"; do sleep 2; done
  xcrun simctl launch "$SIM" com.apple.MobileSMS >/dev/null; sleep 5
  # A reboot wipes the transcript, which is what makes each run identical: the
  # extension finds no game and offers New game instead of resuming one.
  printf '%s' "$SEED" > "$(group_dir)/dev.seed"

  tap 200 210 3            # conversation
  tap 48 826 2.5           # compose "+"
  swipe 0.4 200 700 200 380 1.5
  tap 145 776 4.5          # Foolish
  tap 200 "$(bar_y 0)" 4.5 # Create game
  tap 359 452 1.5; tap 359 469 3.5   # Messages' send circle (two candidate y)
  tap 245 300 5            # open the sent bubble -> EXPANDED lobby
  tap 200 "$(bar_y 0)" 3   # Add player (testing)
  tap 200 "$(bar_y 0)" 12  # Start playing -> auto-collapse
  swipe 0.5 200 525 200 180 4        # grabber drag back to expanded
  echo "drawer top: $(drawer_top)   (72 = expanded)"
}

cmd_film() {
  local name="${1:-film}" secs="${2:-15}"
  rm -f "$WORK/$name.mp4"; rm -rf "$WORK/$name"; mkdir -p "$WORK/$name"
  xcrun simctl io "$SIM" recordVideo --codec h264 --force "$WORK/$name.mp4" >/dev/null 2>&1 &
  local rec=$!
  sleep 3
  shift 2 || true
  "$@"                     # whatever should happen on camera
  sleep "$secs"
  kill -INT $rec 2>/dev/null || true; sleep 4
  ffmpeg -v error -i "$WORK/$name.mp4" -vf fps=30 "$WORK/$name/t_%04d.png"
  echo "frames: $(ls "$WORK/$name" | wc -l) in $WORK/$name"
}

cmd_move() {
  tap 58 800 2                       # select the first hand card
  local y; y=$(bar_y -1)             # the Attack pill is the lowest wood bar
  [ "$y" != "-1" ] && tap 325 "$y" 0
}

case "${1:-}" in
  setup) cmd_setup ;;
  move)  cmd_move ;;
  film)  shift; cmd_film "$@" ;;
  shot)  shot; python3 -c "
from PIL import Image; Image.open('$WORK/shot.png').resize((402,874)).save('$WORK/shot_pt.png')"
         echo "$WORK/shot_pt.png" ;;
  *) sed -n '2,28p' "$0"; exit 1 ;;
esac
