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
#   ios/Tools/msgrig.sh lobby [N]                a LOBBY with N seated, left open
#   ios/Tools/msgrig.sh endgame [P] [SEAT] [nopass]  SEED a FINISHED game, to
#                                                verify what "New game" does at
#                                                the end; `nopass` finishes a
#                                                PODKIDNOY one, which is what a
#                                                rematch must inherit
#   ios/Tools/msgrig.sh fatboard [N] [P] [SEAT]  SEED a dense N-card table
#   ios/Tools/msgrig.sh lastdefense [P] [SEAT]   SEED the bout-ENDING cover,
#                                                seated as the defender who
#                                                must play it
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
#   ios/Tools/msgrig.sh slowmo N     stretch every flight by N (0 = off); set it
#                                    BEFORE the scenario that opens the board
#   ios/Tools/msgrig.sh move         select a card and play it (auto-collapses)
#   ios/Tools/msgrig.sh ruler [off]  draw the debug ruler on the surface's box,
#                                    so a filmed frame reports where the box's
#                                    own top and bottom edges were; set it
#                                    BEFORE the scenario that opens the board
#   ios/Tools/msgrig.sh film NAME S  record S seconds into NAME.mp4 + EVERY
#                                    composited frame + times.txt
#   ios/Tools/msgrig.sh sheet NAME [FIRST] [LAST] [COLS] [ROWS]
#                                    contact sheets of that take, one cell per
#                                    frame, each stamped with its own timestamp
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
    428x926)   echo "214 176  48 878  145 761  385 505  260 355  214 570 214 180" ;;  # iPhone 14 Plus
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
  # Found nothing. Every caller writes a flag file into whatever this prints, so
  # printing NOTHING means writing to `/dev.slowmo` - which on a machine where
  # the root is writable would silently do the wrong thing, and here just says
  # "read-only file system", which reads as a bug in the rig rather than as "the
  # app is not installed on this simulator".
  echo "no App Group container on $SIM - install the app first" >&2
  echo "/nonexistent/group.cards.foolish.msg"
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

# A LOBBY with N players seated, left OPEN - the surface round 16's Exit button
# lives on. `setup` walks straight past this into a dealt game; this stops at
# the lobby so the Start/Exit row can be filmed with 2, 3 or 8 seated.
#
# Seats are filled with the DEBUG "Add player (testing)" control, the same one
# `setup` uses, so one device can stand in for a whole table.
cmd_lobby() {
  local seats="${1:-3}"
  xcrun simctl shutdown "$SIM" 2>/dev/null || true; sleep 3
  xcrun simctl boot "$SIM"; sleep 8
  until xcrun simctl list devices booted | grep -q "$SIM"; do sleep 2; done
  xcrun simctl launch "$SIM" com.apple.MobileSMS >/dev/null; sleep 5
  printf '%s' "$SEED" > "$(group_dir)/dev.seed"
  rm -f "$(group_dir)/dev.fatboard" "$(group_dir)/dev.replay" "$(group_dir)/dev.seat"

  read -r W H < <(screen)
  read -r cx cy px py ax ay sx sy bx by g1x g1y g2x g2y < <(profile "$W" "$H")
  tap "$cx" "$cy" 3        # conversation
  tap "$px" "$py" 2.5      # compose "+"
  swipe 0.4 $((W / 2)) $((H * 82 / 100)) $((W / 2)) $((H * 45 / 100)) 1.5
  tap "$ax" "$ay" 4.5      # Foolish
  tap $((W / 2)) "$(bar_y 0)" 4.5     # Create game
  tap "$sx" "$sy" 1.5; tap "$sx" $((sy + 17)) 3.5   # send circle
  tap "$bx" "$by" 5        # open the sent bubble -> EXPANDED lobby
  # Seat the rest. "Add player" is always the TOP bar while the lobby has room.
  local i=1
  while [ "$i" -lt "$seats" ]; do
    tap $((W / 2)) "$(bar_y 0)" 3
    i=$((i + 1))
  done
  echo "lobby with $seats seated; bars at: $(python3 "$UI" bars | tail -1)"
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
  local players="${1:-3}" seat="${2:-0}" variant="${3:-}"
  local tool="$HERE/../../c/build/msg_wire_test"
  [ -x "$tool" ] || { echo "build it first: (cd c && make build/msg_wire_test)" >&2; exit 1; }

  xcrun simctl shutdown "$SIM" 2>/dev/null || true; sleep 3
  xcrun simctl boot "$SIM"; sleep 8
  until xcrun simctl list devices booted | grep -q "$SIM"; do sleep 2; done
  xcrun simctl launch "$SIM" com.apple.MobileSMS >/dev/null; sleep 5

  "$tool" --endgame "$players" $variant > "$(group_dir)/dev.fatboard"
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

# The BOUT-ENDING COVER, seated as the defender about to play it. The C
# searcher (--lastdefense) finds a deal where the defender's whole remaining
# hand covers every uncovered attack, which is the one state that exercises
# six of build 16's changes at once: the Cover button's greedy full cover, a
# multicover's per-card flights, the hold before the sweep, the withheld
# settlement while the move is only staged, the count lag, and the role
# hand-off that closes it. No deal can be arranged into this by hand.
cmd_lastdefense() {
  local players="${1:-4}" seat="${2:-}"
  local tool="$HERE/../../c/build/msg_wire_test"
  [ -x "$tool" ] || { echo "build it first: (cd c && make build/msg_wire_test)" >&2; exit 1; }

  xcrun simctl shutdown "$SIM" 2>/dev/null || true; sleep 3
  xcrun simctl boot "$SIM"; sleep 8
  until xcrun simctl list devices booted | grep -q "$SIM"; do sleep 2; done
  xcrun simctl launch "$SIM" com.apple.MobileSMS >/dev/null; sleep 5

  # The first line is the human-readable description; the hex is the second.
  "$tool" --lastdefense "$players" | tail -1 > "$(group_dir)/dev.fatboard"
  if [ -n "$seat" ]; then printf '%s' "$seat" > "$(group_dir)/dev.seat"
  else rm -f "$(group_dir)/dev.seat"; fi
  rm -f "$(group_dir)/dev.replay"
  echo "seeded: $(wc -c < "$(group_dir)/dev.fatboard") hex chars, seat=${seat:-defender}"
  "$tool" --lastdefense "$players" | head -1

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

# Record a take and keep EVERY COMPOSITED FRAME.
#
# `simctl io recordVideo` captures at the display's own 60Hz and writes a
# VARIABLE-rate movie: one frame per frame the device actually composited, and
# none at all while the screen is still. Extracting that with `-vf fps=30`
# RESAMPLED it to a constant 30 - which both duplicates the still frames and
# throws away half of every animation. The round-10d collapse was measured off
# such an extraction and got ten samples across the tween; at the recorder's own
# rate the same take carries twice that, and the extra samples fall exactly
# where the curve is steepest.
#
# So: `-fps_mode passthrough`, which keeps the captured frames 1:1. Passthrough
# frames are NOT evenly spaced, so their presentation timestamps go beside them
# in `times.txt`, one line per frame - a frame is PLACED IN TIME, never assumed
# to be 1/30 (or 1/60) after the one before it. On a healthy take the dominant
# inter-frame delta is 0.0167s; a delta outside 12-22ms is a beat the device
# dropped, and `sheet` flags those cells.
#
# FRAME NUMBERING CHANGED HERE (it was `t_%04d.png` at a constant 30fps, from
# t_0001): frames are now `f%05d.png` from f00001, one per composited frame, and
# their times are in `times.txt`. Anything that indexed the old names by
# arithmetic on 33ms is wrong twice over and should read `times.txt` instead.
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
  # The image2 muxer complains "non monotonically increasing dts" once per
  # repeated timestamp in a variable-rate source. It is writing numbered PNGs,
  # which carry no timestamps at all, so it is noise about nothing - but it is
  # the ONLY expected noise, so everything else still reaches the terminal.
  ffmpeg -v error -i "$WORK/$name.mp4" -fps_mode passthrough \
         "$WORK/$name/f%05d.png" 2> "$WORK/$name/ffmpeg.err" || {
    cat "$WORK/$name/ffmpeg.err" >&2; return 1; }
  grep -v 'non monotonically increasing dts\|Last message repeated' \
       "$WORK/$name/ffmpeg.err" >&2 || true
  ffprobe -v error -select_streams v:0 -show_entries frame=pts_time \
          -of csv=p=0 "$WORK/$name.mp4" | tr -d ',' > "$WORK/$name/times.txt"
  local n dom
  n=$(ls "$WORK/$name" | grep -c '^f' || true)
  # The dominant inter-frame delta. 0.0167s = the display's own 60Hz, i.e. the
  # take is intact; anything else is a finding, not a detail.
  dom=$(python3 -c "
import collections
t=[float(x) for x in open('$WORK/$name/times.txt')]
d=collections.Counter(round(b-a,4) for a,b in zip(t,t[1:]))
print(' '.join(f'{k}s x{v}' for k,v in d.most_common(3)))" 2>/dev/null || true)
  echo "frames: $n in $WORK/$name  (deltas: $dom)"
}

# Contact sheets from a take: one cell per composited frame, in reading order,
# each stamped `index +offset (+delta)` read from `times.txt`.
#
# The stamp is the point. A sheet of unlabelled frames says an animation looks
# wrong; a sheet whose cells carry their own time says WHERE it went wrong and
# whether a beat was dropped. A delta outside 12-22ms is flagged.
#
#   msgrig.sh sheet NAME [FIRST] [LAST] [COLS] [ROWS]
cmd_sheet() {
  local name="${1:-film}" first="${2:-0}" last="${3:-0}" cols="${4:-8}" rows="${5:-6}"
  python3 - "$WORK/$name" "$first" "$last" "$cols" "$rows" <<'SHEET'
import os, sys
from PIL import Image, ImageDraw, ImageFont
d, first, last, cols, rows = (sys.argv[1], int(sys.argv[2]), int(sys.argv[3]),
                              int(sys.argv[4]), int(sys.argv[5]))
files = sorted(f for f in os.listdir(d) if f[0] == 'f' and f.endswith('.png'))
times = [float(x) for x in open(os.path.join(d, 'times.txt'))]
# A movie can carry one more frame than ffprobe listed a time for; hold the
# last time rather than shifting every later stamp by a frame.
times += [times[-1] if times else 0.0] * (len(files) - len(times))
keep = list(range(first, min(last or len(files), len(files))))
if not keep:
    print('no frames'); raise SystemExit
cw, band = 240, 30
src = Image.open(os.path.join(d, files[keep[0]]))
ch = int(src.height * cw / src.width)
try:    font = ImageFont.truetype('/System/Library/Fonts/Menlo.ttc', 15)
except Exception: font = ImageFont.load_default()
per, t0 = cols * rows, times[keep[0]]
for s in range((len(keep) + per - 1) // per):
    chunk = keep[s * per:(s + 1) * per]
    sheet = Image.new('RGB', (cols * cw, rows * (ch + band)), (18, 18, 18))
    dr = ImageDraw.Draw(sheet)
    for k, i in enumerate(chunk):
        x, y = (k % cols) * cw, (k // cols) * (ch + band)
        sheet.paste(Image.open(os.path.join(d, files[i])).resize((cw, ch), Image.LANCZOS),
                    (x, y + band))
        dt = (times[i] - times[i - 1]) * 1000 if i else 0
        # Not ~17ms: a frame the device never composited.
        flag = '' if (i == keep[0] or 12 <= dt <= 22) else ' !'
        dr.text((x + 5, y + 7),
                f'{i:04d} +{(times[i] - t0) * 1000:.0f}ms ({dt:+.0f}){flag}',
                fill=(255, 210, 90) if flag else (170, 170, 170), font=font)
        dr.rectangle([x, y, x + cw - 1, y + band + ch - 1], outline=(60, 60, 60))
    p = os.path.join(d, f'sheet{s + 1:02d}.png')
    sheet.save(p); print(p)
SHEET
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
  lastdefense) shift; cmd_lastdefense "$@" ;;
  endgame) shift; cmd_endgame "$@" ;;
  lobby) shift; cmd_lobby "$@" ;;
  twocover) shift; cmd_twocover "$@" ;;
  reopen) shift; cmd_reopen "$@" ;;
  unseed) rm -f "$(group_dir)/dev.fatboard" "$(group_dir)/dev.replay"; echo "seed removed" ;;
  # Stretch every flight by N (0 = off). Read once at first use, so this must be
  # set BEFORE the extension launches - i.e. before setup/fatboard/lastdefense.
  slowmo) if [ "${2:-0}" = "0" ]; then rm -f "$(group_dir)/dev.slowmo"; echo "slowmo off"
          else printf '%s' "$2" > "$(group_dir)/dev.slowmo"; echo "slowmo x$2"; fi ;;
  move)  cmd_move ;;
  film)  shift; cmd_film "$@" ;;
  sheet) shift; cmd_sheet "$@" ;;
  # Draw the debug RULER on the surface's own box (CollapseRuler) - the two
  # edges of `boxHeight` in a filmed frame, which is the only way to read the
  # collapse curve off a transition the host composites from snapshots. Read
  # once, like slowmo, so set it BEFORE the scenario that opens the board.
  ruler) if [ "${2:-on}" = "off" ]; then rm -f "$(group_dir)/dev.ruler"; echo "ruler off"
         else : > "$(group_dir)/dev.ruler"; echo "ruler on"; fi ;;
  probe) read -r W H < <(screen); echo "screen ${W}x${H}pt"; python3 "$UI" all ;;
  shot)  shot; read -r W H < <(screen); python3 -c "
from PIL import Image; Image.open('$WORK/shot.png').resize(($W,$H)).save('$WORK/shot_pt.png')"
         echo "$WORK/shot_pt.png" ;;
  *) sed -n '2,30p' "$0"; exit 1 ;;
esac
