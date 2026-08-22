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
#   ios/Tools/msgrig.sh endgame [P] [SEAT]       SEED a FINISHED game, to verify
#                                                what "New game" does at the end
#   ios/Tools/msgrig.sh fatboard [N] [P] [SEAT]  SEED a dense N-card table
#   ios/Tools/msgrig.sh twocover [P] [SEAT] [one]  SEED two covers as two bubbles
#                                                (or as ONE, the control)
#                                    (default 10) and open straight onto it - no
#                                    create/join/start flow at all. DEBUG only.
#                                    The chain is searched in C (instant), not
#                                    played on the device; see MessageDevBoard.
#                                    TWO PLAYERS is enough: every cover puts its
#                                    own rank on the table, so the lone attacker
#                                    always has something new to throw.
#   ios/Tools/msgrig.sh unseed       remove the seed, back to normal flow
#   ios/Tools/msgrig.sh move         select a card and play it (auto-collapses)
#   ios/Tools/msgrig.sh film NAME S  record S seconds into NAME.mp4 + frames
#   ios/Tools/msgrig.sh shot         one screenshot, downscaled to points
#
# Requirements (one-time):
#   brew install facebook/fb/idb-companion
#   python3.12 -m venv ~/.venvs/idb && ~/.venvs/idb/bin/pip install fb-idb
#   ln -sf ~/.venvs/idb/bin/idb /opt/homebrew/bin/idb    # so `idb` is on PATH
#   pip install pillow numpy   (system python3)
# (fb-idb needs python <= 3.12; the system 3.14 cannot build it. Set FOOLISH_IDB
#  to override the binary; it defaults to whatever `idb` is on PATH.)
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
IDB="${FOOLISH_IDB:-idb}"
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
  # A plain `setup` is never quietly running a seeded board.
  rm -f "$(group_dir)/dev.fatboard"

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

# Seed a dense table and open straight onto it. The C searcher prints ONE FMSG
# envelope as hex; the extension reads that file and opens it as the defender.
# Boots the sim and opens Messages, but does NOT walk the lobby - that is the
# entire point (owner: "skip the create game / join game / start game stuff and
# jump straight to the game state").
# The END-GAME board: a FINISHED chain, for verifying what "New game" does at
# the end of a game (the fool's penalty). Same machinery as fatboard - the
# state is searched in C and the device just opens it - so the fool is the same
# seat on every run and a filmed comparison actually compares.
cmd_endgame() {
  local players="${1:-3}" seat="${2:-0}"
  local tool="$HERE/../../c/build/msg_wire_test"
  [ -x "$tool" ] || { echo "build it first: (cd c && make build/msg_wire_test)" >&2; exit 1; }

  xcrun simctl shutdown "$SIM" 2>/dev/null || true; sleep 3
  xcrun simctl boot "$SIM"; sleep 8
  until xcrun simctl list devices booted | grep -q "$SIM"; do sleep 2; done
  xcrun simctl launch "$SIM" com.apple.MobileSMS >/dev/null; sleep 5

  "$tool" --endgame "$players" > "$(group_dir)/dev.fatboard"
  printf '%s' "$seat" > "$(group_dir)/dev.seat"
  rm -f "$(group_dir)/dev.replay"
  echo "seeded endgame: $(wc -c < "$(group_dir)/dev.fatboard") hex chars, seat=$seat"

  read -r W H < <(screen)
  read -r cx cy px py ax ay sx sy bx by g1x g1y g2x g2y < <(profile "$W" "$H")
  tap "$cx" "$cy" 3        # conversation
  tap "$px" "$py" 2.5      # compose "+"
  swipe 0.4 $((W / 2)) $((H * 82 / 100)) $((W / 2)) $((H * 45 / 100)) 1.5
  tap "$ax" "$ay" 5        # Foolish -> straight onto the finished board
  swipe 0.5 "$g1x" "$g1y" "$g2x" "$g2y" 4   # grabber drag to expanded
  echo "drawer top: $(drawer_top)   (a small number = expanded)"
}

cmd_fatboard() {
  local cards="${1:-10}" players="${2:-2}" seat="${3:-}"
  local tool="$HERE/../../c/build/msg_wire_test"
  [ -x "$tool" ] || { echo "build it first: (cd c && make build/msg_wire_test)" >&2; exit 1; }

  xcrun simctl shutdown "$SIM" 2>/dev/null || true; sleep 3
  xcrun simctl boot "$SIM"; sleep 8
  until xcrun simctl list devices booted | grep -q "$SIM"; do sleep 2; done
  xcrun simctl launch "$SIM" com.apple.MobileSMS >/dev/null; sleep 5

  "$tool" --fatboard "$cards" "$players" > "$(group_dir)/dev.fatboard"
  # Which chair to sit in. Default (unset) = the defender's, for the pickup
  # case; pass a seat for the deal case, where an ATTACKER's good closes the
  # bout and deals.
  if [ -n "$seat" ]; then printf '%s' "$seat" > "$(group_dir)/dev.seat"
  else rm -f "$(group_dir)/dev.seat"; fi
  rm -f "$(group_dir)/dev.replay"   # a fatboard opens quiet; see cmd_twocover
  echo "seeded: $(wc -c < "$(group_dir)/dev.fatboard") hex chars, seat=${seat:-defender}"

  read -r W H < <(screen)
  read -r cx cy px py ax ay sx sy bx by g1x g1y g2x g2y < <(profile "$W" "$H")
  tap "$cx" "$cy" 3        # conversation
  tap "$px" "$py" 2.5      # compose "+"
  swipe 0.4 $((W / 2)) $((H * 82 / 100)) $((W / 2)) $((H * 45 / 100)) 1.5
  tap "$ax" "$ay" 5        # Foolish -> straight onto the seeded board
  swipe 0.5 "$g1x" "$g1y" "$g2x" "$g2y" 4   # grabber drag to expanded
  echo "drawer top: $(drawer_top)   (a small number = expanded)"
}

# Round 16: two covers SENT AS TWO BUBBLES, opened on the second one. The C
# searcher plays the whole thing and prints the last bubble; the extension opens
# it as the ATTACKER (seat 0 by default), because the covers are then somebody
# else's move - which is the case that animates on open. Exactly one cover
# should fly; the earlier one must already be sitting on the table, landed.
# Owner: "If anyone opens the bubble for the second cover, they will see BOTH
# covers animate. This is not ideal. We should only see the most recent move."
cmd_twocover() {
  local players="${1:-2}" seat="${2:-0}" one="${3:-}"
  local tool="$HERE/../../c/build/msg_wire_test"
  [ -x "$tool" ] || { echo "build it first: (cd c && make build/msg_wire_test)" >&2; exit 1; }

  xcrun simctl shutdown "$SIM" 2>/dev/null || true; sleep 3
  xcrun simctl boot "$SIM"; sleep 8
  until xcrun simctl list devices booted | grep -q "$SIM"; do sleep 2; done
  xcrun simctl launch "$SIM" com.apple.MobileSMS >/dev/null; sleep 5

  "$tool" --twocover "$players" $one > "$(group_dir)/dev.fatboard"
  printf '%s' "$seat" > "$(group_dir)/dev.seat"
  # The REPLAY is the subject here, so this seeded open is not a quiet one.
  : > "$(group_dir)/dev.replay"
  echo "seeded: $(wc -c < "$(group_dir)/dev.fatboard") hex chars, seat=$seat, replay on"

  read -r W H < <(screen)
  read -r cx cy px py ax ay sx sy bx by g1x g1y g2x g2y < <(profile "$W" "$H")
  tap "$cx" "$cy" 3        # conversation
  tap "$px" "$py" 2.5      # compose "+"
  swipe 0.4 $((W / 2)) $((H * 82 / 100)) $((W / 2)) $((H * 45 / 100)) 1.5
  tap "$ax" "$ay" 5        # Foolish -> straight onto the seeded board
}

# Re-open the extension onto whatever is already seeded, WITHOUT rebooting or
# re-seeding - so an open-replay can be filmed on demand. Killing Messages is
# what makes it a real cold open: re-entering from the app strip alone can find
# the board still resident and replay nothing.
cmd_reopen() {
  xcrun simctl terminate "$SIM" com.apple.MobileSMS >/dev/null 2>&1 || true
  sleep 2
  xcrun simctl launch "$SIM" com.apple.MobileSMS >/dev/null; sleep 4
  read -r W H < <(screen)
  read -r cx cy px py ax ay sx sy bx by g1x g1y g2x g2y < <(profile "$W" "$H")
  tap "$cx" "$cy" 3        # conversation
  tap "$px" "$py" 2.5      # compose "+"
  swipe 0.4 $((W / 2)) $((H * 82 / 100)) $((W / 2)) $((H * 45 / 100)) 1.5
  tap "$ax" "$ay" 5        # Foolish -> the seeded board, replaying its bubble
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
  setup) shift; cmd_setup "$@" ;;
  fatboard) shift; cmd_fatboard "$@" ;;
  endgame) shift; cmd_endgame "$@" ;;
  twocover) shift; cmd_twocover "$@" ;;
  reopen) shift; cmd_reopen "$@" ;;
  unseed) rm -f "$(group_dir)/dev.fatboard" "$(group_dir)/dev.replay"; echo "seed removed" ;;
  move)  cmd_move ;;
  film)  shift; cmd_film "$@" ;;
  probe) read -r W H < <(screen); echo "screen ${W}x${H}pt"; python3 "$UI" all ;;
  shot)  shot; read -r W H < <(screen); python3 -c "
from PIL import Image; Image.open('$WORK/shot.png').resize(($W,$H)).save('$WORK/shot_pt.png')"
         echo "$WORK/shot_pt.png" ;;
  *) sed -n '2,30p' "$0"; exit 1 ;;
esac
