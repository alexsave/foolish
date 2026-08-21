#!/bin/bash
# msgrig.sh - drive the REAL Messages app in the simulator, deterministically.
#
# Why this exists: the iMessage board's collapse defects only reproduce in the
# host app (the harness animates its fake drawer, the real one does not), and
# hand-driven runs were costing an hour each - taps drifted with the layout,
# the deal handed us the defender seat at random, and `cliclick` wedged the
# simulator's touch state. This rig removes all three.
#
#   ios/Tools/msgrig.sh setup        fresh seeded game, board open and EXPANDED
#   ios/Tools/msgrig.sh move         select a card and play it (auto-collapses)
#   ios/Tools/msgrig.sh film NAME S  record S seconds into NAME.mp4 + frames
#   ios/Tools/msgrig.sh shot         one screenshot, downscaled to points
#
# Requirements (one-time):
#   brew install facebook/fb/idb-companion
#   python3.12 -m venv <dir> && <dir>/bin/pip install fb-idb   # 3.14 is broken
#   pip install pillow numpy   (in that venv or system python3)
#
# NEVER use cliclick here: it cannot drag the drawer's grabber, needs window
# focus, and a lost mouse-up leaves a touch stuck down until the sim reboots.
# idb injects HID directly, in DEVICE POINTS (NOT pixels - see `scale`).
#
# DEVICE-AGNOSTIC, deliberately: a rig that only works on the phone it was
# written on hides exactly the bugs a second phone finds (the SE's 262pt drawer
# caught a board collision the 17's 355pt one had slack to hide). Everything
# inside our own extension is found by COLOUR at runtime; only Messages' own
# chrome needs coordinates, and those come from the profile below. To add a
# device, boot it, run `msgrig.sh probe`, and add eight numbers.
set -euo pipefail

SIM="${FOOLISH_SIM:-EFB2FD39-DD17-4284-9C46-013142226F6F}"
IDB="${FOOLISH_IDB:?set FOOLISH_IDB to the fb-idb binary, e.g. /path/venv/bin/idb}"
WORK="${FOOLISH_WORK:-/tmp/msgrig}"
SEED="${FOOLISH_SEED:-3}"          # 3 deals the CREATOR the first attack at 2p
HERE="$(cd "$(dirname "$0")" && pwd)"
UI="$HERE/msgui.py"
export FOOLISH_SIM FOOLISH_WORK
mkdir -p "$WORK"

tap()   { "$IDB" ui tap --udid "$SIM" "$1" "$2"; sleep "${3:-1}"; }
swipe() { "$IDB" ui swipe --udid "$SIM" --duration "$1" "$2" "$3" "$4" "$5"; sleep "${6:-1}"; }
shot()  { xcrun simctl io "$SIM" screenshot "$WORK/shot.png" >/dev/null 2>&1; }

# The screen in POINTS, straight from the accessibility tree's root element -
# the one place that reports points on every device without a lookup table.
screen() { python3 "$HERE/msgax.py" screen; }

# Messages' own chrome, per device. Everything else is found at runtime.
#   CONV  first conversation row      APPS  the Foolish tile in the app drawer
#   PLUS  the compose "+"             SEND  Messages' blue send circle
#   BUBB  the sent bubble to open     GRAB  the drawer grabber, compact/expanded
profile() {
  case "$1x$2" in
    402x874)   echo "200 210  48 826  145 776  359 452  245 300  200 525 200 180" ;;  # iPhone 17
    375x667)   echo "188 150  48 618  145 602  333 353  230 250  188 420 188 140" ;;  # iPhone SE
    *) echo "no profile for $1x$2 points - run 'msgrig.sh probe' and add one" >&2
       exit 2 ;;
  esac
}

# Wooden buttons are found by COLOUR, never by hard-coded y: the lobby moves
# with the player count, locale, device and presentation style, and a stale
# constant lands in the Settings gear (which silently switches the app's
# language). `-1` asks for the LOWEST bar on screen.
bar_y() { python3 "$UI" bars | python3 -c "
import sys,ast; b=ast.literal_eval(sys.stdin.read().split('BARS ')[1])
print(b[${1:-0}][0] if b else -1)"; }
drawer_top() { python3 "$UI" top | awk '{print $2}'; }

# The App Group container holds the dev flags. Its UUID changes on reinstall.
group_dir() {
  for d in ~/Library/Developer/CoreSimulator/Devices/"$SIM"/data/Containers/Shared/AppGroup/*/; do
    id=$(plutil -extract MCMMetadataIdentifier raw "$d/.com.apple.mobile_container_manager.metadata.plist" 2>/dev/null || true)
    [ "$id" = "group.cards.foolish.msg" ] && { echo "$d"; return; }
  done
}

cmd_setup() {
  xcrun simctl shutdown "$SIM" 2>/dev/null || true; sleep 3
  xcrun simctl boot "$SIM"; sleep 8
  until xcrun simctl list devices booted | grep -q "$SIM"; do sleep 2; done
  xcrun simctl launch "$SIM" com.apple.MobileSMS >/dev/null; sleep 5
  # A reboot wipes the transcript, which is what makes each run identical: the
  # extension finds no game and offers New game instead of resuming one.
  printf '%s' "$SEED" > "$(group_dir)/dev.seed"

  read -r W H < <(screen)
  read -r cx cy px py ax ay sx sy bx by g1x g1y g2x g2y < <(profile "$W" "$H")
  echo "device ${W}x${H}pt"

  tap "$cx" "$cy" 3        # conversation
  tap "$px" "$py" 2.5      # compose "+"
  swipe 0.4 $((W / 2)) $((H * 82 / 100)) $((W / 2)) $((H * 45 / 100)) 1.5
  tap "$ax" "$ay" 4.5      # Foolish
  tap $((W / 2)) "$(bar_y 0)" 4.5     # Create game
  tap "$sx" "$sy" 1.5; tap "$sx" $((sy + 17)) 3.5   # send circle (two candidate y)
  tap "$bx" "$by" 5        # open the sent bubble -> EXPANDED lobby
  tap $((W / 2)) "$(bar_y 0)" 3       # Add player (testing)
  tap $((W / 2)) "$(bar_y 0)" 12      # Start playing -> auto-collapse
  swipe 0.5 "$g1x" "$g1y" "$g2x" "$g2y" 4   # grabber drag back to expanded
  echo "drawer top: $(drawer_top)   (a small number = expanded)"
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

# Both taps are found by colour, so this is the same code on every device: the
# leftmost hand card, then the lowest wooden pill (the play button).
cmd_move() {
  local x y
  x=$(python3 "$UI" cards | python3 -c "
import sys,ast; c=ast.literal_eval(sys.stdin.read().split('CARDS ')[1]); print(c[0] if c else -1)")
  y=$(python3 "$UI" hand_y | awk '{print $2}')
  [ "$x" = "-1" ] && { echo "no hand cards found" >&2; exit 1; }
  tap "$x" "$y" 2
  y=$(bar_y -1)            # the play pill is the lowest wood bar
  [ "$y" != "-1" ] && tap "$(( $(screen | awk '{print $1}') * 4 / 5 ))" "$y" 0
}

case "${1:-}" in
  setup) cmd_setup ;;
  move)  cmd_move ;;
  film)  shift; cmd_film "$@" ;;
  probe) read -r W H < <(screen); echo "screen ${W}x${H}pt"; python3 "$UI" all ;;
  shot)  shot; read -r W H < <(screen); python3 -c "
from PIL import Image; Image.open('$WORK/shot.png').resize(($W,$H)).save('$WORK/shot_pt.png')"
         echo "$WORK/shot_pt.png" ;;
  *) sed -n '2,30p' "$0"; exit 1 ;;
esac
