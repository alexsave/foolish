#!/bin/bash
# anim_reel.sh - every move and its Undo, in ONE film, every table card checked.
#
#   FOOLISH_SIM=<udid> [FOOLISH_FLAGS='key=0 ...'] [ONLY='regex'] ios/Tools/rig/shots/anim_reel.sh [NAME]
#
# ONLY='pass|8p' plays just the scenarios whose label matches (the replayed
# arrivals match as "replay <kind>") - to watch or re-film one again.
#
# LOCAL ONLY, NEVER CI: a booted simulator, a DEBUG build (`rig.sh build`) and
# Messages. Run it by hand after any table or flight change, and now and then.
# Exit 0 clean, 1 anomalies, 2 a scenario could not be played.
#
# Owner: "instead of one rig test per game, just run a bunch of different
# animation scenarios in a single film and film that. Check box positions for
# any jumps and report anomalies" - then "vary the moves, not just throw in but
# bout ending stuff", and "no big jumps like this".
#
# Under `dev.ruler` every table pair carries a coloured square at its centre and
# every flying card an ORANGE one. `lib/tablesquares.py` reads the movie off a
# pipe and reports, named by scenario:
#   - a JUMP: a table pair's step that stands out from the steps either side;
#   - a card GONE BEFORE ITS FLIGHT: a table card that left the table while no
#     flight existed yet (the undo bugs of 2026-09-17: 108ms and 87ms).
#
# The scenarios - each a live move from the EXPANDED drawer (so it auto-collapses)
# and then its Undo, the exact reverse:
#   throw in, bout-ending Good, first attack, cover, pickup, pass (by drag),
#   the cover that ends the bout, an 8-seat throw-in, and two from the COMPACT
#   drawer; then replayed arrivals of an attack, a cover, a trump cover, a pickup,
#   a good and a refill.
#
# WAITING IS POLLING. Every wait asks for the thing itself - the hand drawn, the
# plank back (Undo only shows once the flight and the collapse are over), a new
# line in the extension's flight log - with a ceiling, never a guessed sleep.
set -uo pipefail
: "${FOOLISH_SIM:?set FOOLISH_SIM}"
export FOOLISH_OUT="${FOOLISH_OUT:-$HOME/Downloads/foolish-shots}"
HERE="$(cd "$(dirname "$0")" && pwd)"; RIG="$HERE/../rig.sh"; LIB="$HERE/../lib"
REPO="$(cd "$HERE/../../../.." && pwd)"
NAME="${1:-reel}"
G=$("$RIG" group)
D="$FOOLISH_OUT/film/$NAME"; rm -rf "$D"; mkdir -p "$D"
now() { date +%s.%N; }
say() { printf '  %s\n' "$*" >&2; }
bad=0
miss() { say "!! $1"; echo "$1" >> "$D/missed.txt"; bad=1; }

# ---- a clean Messages -----------------------------------------------------
# Relaunched: a running Messages keeps the extension's bundle path from before
# the last build (throwin_slide.sh has the whole story).
"$RIG" ruler on >/dev/null
rm -f "$G/dev.stage" "$G/dev.staged" "$G/dev.claimed" "$G/dev.flags"
# FOOLISH_FLAGS='table.slide=0' runs the reel against a flag's other state -
# which is how the reel itself is checked: it must report what that puts back.
[ -n "${FOOLISH_FLAGS:-}" ] && printf '%s' "$FOOLISH_FLAGS" > "$G/dev.flags"
xcrun simctl terminate "$FOOLISH_SIM" com.apple.MobileSMS >/dev/null 2>&1
xcrun simctl launch "$FOOLISH_SIM" com.apple.MobileSMS >/dev/null 2>&1
"$RIG" enter >/dev/null 2>&1 || { echo "could not enter a conversation" >&2; exit 2; }
"$RIG" clearstage >/dev/null 2>&1 || true
read -r W H < <(python3 "$LIB/ax.py" screen)

# ---- reading the board ----------------------------------------------------
plank_y() { python3 "$LIB/ui.py" bars | python3 -c "
import sys, ast
b = ast.literal_eval(sys.stdin.read().split('BARS ')[1]); print(b[-1][0] if b else -1)"; }
wait_plank()    { local i=0; while [ $i -lt "${1:-60}" ]; do [ "$(plank_y)" != "-1" ] && return 0; i=$((i + 1)); done; return 1; }
wait_no_plank() { local i=0; while [ $i -lt "${1:-20}" ]; do [ "$(plank_y)" = "-1" ] && return 0; i=$((i + 1)); done; return 1; }
shot()  { xcrun simctl io "$FOOLISH_SIM" screenshot "$D/now.png" >/dev/null 2>&1; }
# A flight-log line is `12.00 fly 45.5 ^45.6 place-3-11 from=... to=...`: the id
# comes after two memory columns.
flights() { local n; n=$(grep -cE " fly [0-9.]+ \^[0-9.]+ ($1)-" "$G/flight.log" 2>/dev/null); echo "${n:-0}"; }
# Poll until the flight log has more `$1` flights than `$2`.
wait_flight() { local i=0; while [ $i -lt "${3:-40}" ]; do [ "$(flights "$1")" -gt "$2" ] && return 0; i=$((i + 1)); sleep 0.1; done; return 1; }

# ---- the board ------------------------------------------------------------
# A clean board: the last one's staged bubble cleared (an Undo leaves the
# pre-move board in the compose field), the seed claimed, the hand drawn, and
# the drawer expanded unless COMPACT is set.
#
# FAST: the draft is removed IN PLACE and the appex killed - never a leave and
# re-enter of the thread (`clearstage` without `stay` does that, ~5s a board).
# No wait for a plank here either: an attacker's empty table never gets one, so
# that wait burned its whole ceiling (~14s) on half the boards. A move that
# needs the plank waits for it itself - the defender's held-back Pickup included.
open_board() {   # open_board SEAT MODE ARGS...
  local seat="$1"; shift
  "$RIG" clearstage stay >/dev/null 2>&1 || true
  if [ -n "$seat" ]; then SEAT="$seat" "$RIG" seed "$@" >/dev/null
  else "$RIG" seed "$@" >/dev/null; fi
  "$RIG" killappex >/dev/null 2>&1
  "$RIG" open >/dev/null 2>&1
  local i=0
  while [ $i -lt 60 ]; do
    [ "$(python3 "$LIB/ui.py" hand_y | awk '{print $2}')" != "-1" ] && break
    i=$((i + 1))
  done
  [ -z "${COMPACT:-}" ] && "$RIG" expand >/dev/null 2>&1
  return 0
}

goodwait_seat() {
  "$REPO/c/build/msg_wire_test" --goodwait "$1" 2>&1 >/dev/null \
    | sed -n 's/.*us=seat \([0-9]*\).*/\1/p' | head -1
}

# ---- the moves ------------------------------------------------------------
# Each returns 0 once the move is really played - proved by the extension's
# own flight log, not by a plank or a hand count (a selected card lifts out of
# the hand finder's band; Undo is hidden until the animations are over).
move() {   # move ACTION
  local n0 px py hy xs x tgt tx ty k
  n0=$(flights "place|coverland")
  case "$1" in
    throwin)
      read -r px py < <("$RIG" throwin select) || return 1
      "$RIG" tap "$px" "$py" 0 >/dev/null 2>&1
      wait_flight "place|coverland" "$n0" ;;
    good|pickup)
      wait_plank 40 || return 1
      "$RIG" tap $((W * 4 / 5)) "$(plank_y)" 0 >/dev/null 2>&1
      [ "$1" = pickup ] && { wait_flight "openpick|pick" 0 || true; }
      wait_no_plank 20 ;;
    attack)
      # An empty table has no plank until a card is selected.
      shot; read -r hy xs < <(python3 "$LIB/board.py" hand "$D/now.png")
      for x in $xs; do
        "$RIG" tap "$x" "$hy" 0.4 >/dev/null 2>&1
        if [ "$(plank_y)" != "-1" ]; then
          "$RIG" tap $((W * 4 / 5)) "$(plank_y)" 0 >/dev/null 2>&1
          wait_flight "place" "$n0" && return 0
        fi
        "$RIG" tap "$x" "$hy" 0.3 >/dev/null 2>&1
      done
      return 1 ;;
    cover)
      shot; read -r hy xs < <(python3 "$LIB/board.py" hand "$D/now.png")
      tgt=$(python3 "$LIB/board.py" table "$D/now.png")
      for x in $xs; do
        set -- $tgt
        while [ $# -ge 2 ]; do
          tx=$1; ty=$2; shift 2
          "$RIG" tap "$x" "$hy" 0.4 >/dev/null 2>&1
          "$RIG" tap "$tx" "$ty" 0 >/dev/null 2>&1
          wait_flight "place|coverland" "$n0" 20 && return 0
          "$RIG" tap "$x" "$hy" 0.3 >/dev/null 2>&1      # deselect before the next try
        done
      done
      return 1 ;;
    pass)
      # The card of the attacked rank dropped on OPEN felt. A card that can
      # cover resolves onto the nearest attack instead: a flight to an existing
      # slot is undone and the next card tried.
      for k in 0 1 2 3 4 5 6 7; do
        shot; read -r hy xs < <(python3 "$LIB/board.py" hand "$D/now.png")
        tgt=$(python3 "$LIB/board.py" table "$D/now.png")
        set -- $xs; [ $# -le $k ] && return 1; shift $k; x=$1
        # WHERE A PERSON DROPS IT: on the slot the preview opens, just right of
        # the last pair, level with the table - not 110pt above the table, which
        # made the card land at the top centre and fly DOWN into its slot (owner:
        # "why does it overshoot the table?"). And a SMOOTH drag: 6px touch
        # steps over 0.8s, not idb's default of ~38pt a step, which read as the
        # card jumping out of the hand.
        read -r tx ty < <(echo $tgt | awk '{mx=-1; my=400; for (i=1;i<NF;i+=2) if ($i>mx) {mx=$i; my=$(i+1)}; print (mx<0 ? 220 : mx+48), my}')
        n0=$(flights "place")
        idb ui swipe --udid "$FOOLISH_SIM" --duration 0.8 --delta 6 "$x" "$hy" "$tx" "$ty" >/dev/null 2>&1
        wait_flight "place" "$n0" 30 || continue
        px=$(grep -E " fly [0-9.]+ \^[0-9.]+ place-" "$G/flight.log" | tail -1 | sed -n 's/.*to=(\([0-9]*\),.*/\1/p')
        if ! echo "$tgt" | awk -v p="$px" '{for (i=1;i<=NF;i+=2) if ((p-$i)^2 < 400) f=1} END {exit f?0:1}'; then
          return 0
        fi
        undo >/dev/null 2>&1
        [ -z "${COMPACT:-}" ] && "$RIG" expand >/dev/null 2>&1
      done
      return 1 ;;
    passbutton)
      # The Pass BUTTON: select one card, and the plank that comes up (Take
      # goes while anything is selected) is the play for it. A flight to a new
      # slot is the pass; to an existing pair it was a Cover - undone, and the
      # next card tried.
      for k in 0 1 2 3 4 5 6 7; do
        shot; read -r hy xs < <(python3 "$LIB/board.py" hand "$D/now.png")
        tgt=$(python3 "$LIB/board.py" table "$D/now.png")
        set -- $xs; [ $# -le $k ] && return 1; shift $k; x=$1
        "$RIG" tap "$x" "$hy" 0.4 >/dev/null 2>&1
        if [ "$(plank_y)" = "-1" ]; then "$RIG" tap "$x" "$hy" 0.3 >/dev/null 2>&1; continue; fi
        n0=$(flights "place")
        "$RIG" tap $((W * 4 / 5)) "$(plank_y)" 0 >/dev/null 2>&1
        wait_flight "place" "$n0" 30 || continue
        px=$(grep -E " fly [0-9.]+ \^[0-9.]+ place-" "$G/flight.log" | tail -1 | sed -n 's/.*to=(\([0-9]*\),.*/\1/p')
        if ! echo "$tgt" | awk -v p="$px" '{for (i=1;i<=NF;i+=2) if ((p-$i)^2 < 400) f=1} END {exit f?0:1}'; then
          return 0
        fi
        undo >/dev/null 2>&1
        [ -z "${COMPACT:-}" ] && "$RIG" expand >/dev/null 2>&1
      done
      return 1 ;;
  esac
}

# Undo, once it is offered, and wait for its flight (if any) to be under way.
undo() {
  local n0 y
  wait_no_plank 20 || true
  wait_plank 80 || return 1
  n0=$(flights "undo|undorelease")
  y=$(plank_y)
  "$RIG" tap $((W * 4 / 5)) "$y" 0 >/dev/null 2>&1
  wait_flight "undo|undorelease" "$n0" 25 || true
  wait_no_plank 10 || true
  return 0
}

# ---- roll -----------------------------------------------------------------
REC0=$(now)
printf 'rec0 %s\n' "$REC0" > "$D/marks.txt"    # tablesquares.py aligns marks to the film by it
xcrun simctl io "$FOOLISH_SIM" recordVideo --codec h264 --force "$D/take.mp4" >/dev/null 2>&1 &
rec=$!
# ALWAYS stop the recorder with SIGINT: a recorder killed any other way leaves
# the simulator "Host recording is already in progress" until it is rebooted.
trap 'kill -INT $rec 2>/dev/null' EXIT
mark() { printf '%.2f %s\n' "$(echo "$(now) - $REC0" | bc)" "$1" >> "$D/marks.txt"; say "$1"; }

scenario() {   # scenario LABEL SEAT ACTION MODE ARGS...
  local label="$1" seat="$2" action="$3"; shift 3
  [ -n "${ONLY:-}" ] && ! [[ "$label" =~ $ONLY ]] && return
  mark "open: $label"
  open_board "$seat" "$@"
  mark "$label"
  move "$action" || { miss "$label: the move could not be played"; return; }
  mark "undo $label"
  undo || miss "$label: Undo never came back"
}

s8=$(goodwait_seat 8)
scenario "2p throw in"                0     throwin goodwait 2
scenario "2p Good (bout ends)"        0     good    goodwait 2
scenario "2p first attack"            0     attack  lastmove-live attack 2
scenario "2p cover"                   1     cover   lastmove-live cover 2
scenario "2p pickup"                  1     pickup  lastmove-live pickup 2
scenario "2p pass (drag)"             ""    pass    passable 2
scenario "2p pass (button)"           ""    passbutton passable 2
scenario "2p cover ending the bout"   ""    cover   lastdefense 2
scenario "8p throw in"                "${s8:-0}" throwin goodwait 8
COMPACT=1 scenario "compact throw in"      0     throwin goodwait 2
COMPACT=1 scenario "compact first attack"  0     attack  lastmove-live attack 2

# Replayed arrivals: someone else's move, played back as the board opens.
for kind in attack cover covertrump pickup goodany refill; do
  [ -n "${ONLY:-}" ] && ! [[ "replay $kind" =~ $ONLY ]] && continue
  # NOT "open:" - the replay IS the animation under test, and it plays while the
  # board opens.
  mark "replay $kind"
  REPLAY=1 COMPACT=1 open_board "" lastmove "$kind" 2
  wait_flight "open|openpick|opendraw|opendiscard" 0 40 || true
  wait_plank 20 || true
done

kill -INT $rec 2>/dev/null || true
trap - EXIT
i=0
while [ $i -lt 80 ]; do
  a=$(stat -f%z "$D/take.mp4" 2>/dev/null || echo 0); sleep 0.2
  b=$(stat -f%z "$D/take.mp4" 2>/dev/null || echo 0)
  [ "$a" = "$b" ] && [ "$a" != 0 ] && break
  i=$((i + 1))
done
"$RIG" clearstage >/dev/null 2>&1 || true

# ---- read it --------------------------------------------------------------
python3 "$LIB/tablesquares.py" "$D/take.mp4" --marks "$D/marks.txt" --csv "$D/squares.csv" \
        --setup-prefix "open:"
rc=$?
rm -f "$G/dev.flags"
echo "$D"
[ -s "$D/missed.txt" ] && { echo "scenarios that did not run:"; sed 's/^/  /' "$D/missed.txt"; }
[ $bad -ne 0 ] && [ $rc -eq 0 ] && rc=2
exit $rc
