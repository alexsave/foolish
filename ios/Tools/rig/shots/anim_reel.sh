#!/bin/bash
# anim_reel.sh - many animation scenarios in ONE film, every table pair checked.
#
#   FOOLISH_SIM=<udid> [FOOLISH_FLAGS='key=0 ...'] ios/Tools/rig/shots/anim_reel.sh [NAME]
#
# LOCAL ONLY, NEVER CI: a booted simulator, a DEBUG build (`rig.sh build`) and
# Messages. Meant to be run by hand now and then - weekly, say - to catch a
# table animation regressing. Exit 0 clean, 1 anomalies, 2 nothing measured.
#
# Owner: "instead of one rig test per game, just run a bunch of different
# animation scenarios in a single film and film that. Check box positions for
# any jumps and report anomalies." Under `dev.ruler` the live table draws a
# coloured square at the centre of every pair (CollapseRuler `tableSquare`);
# `lib/tablesquares.py` reads the movie straight off a pipe and reports every
# jump with the scenario it happened in, from `marks.txt`.
#
# The scenarios, in order - owner: "vary the moves, not just throw in but bout
# ending stuff". Every live move is undone again, which is its own animation.
#   2p attacker   throw in; GOOD that ends the bout (sweep + deal)   --goodwait 2
#   2p defender   cover that leaves the bout open; PICKUP             --fatboard 4 2
#   2p defender   the cover that ENDS the bout                        --lastdefense 2
#   8p attacker   throw in; GOOD that ends the bout                   --goodwait 8
#   replayed arrivals, each opened fresh (REPLAY=1, --lastmove <kind> 2):
#                 attack, cover, trump cover, pickup, good, refill, a player
#                 going out, and the move that ends the game
#
# The marks are wall-clock seconds from the moment the recorder was started,
# and the movie's clock starts at its first frame, ~4s later. tablesquares.py
# lines the two up exactly off the ruler's clock strip (`clock_offset`).
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

# ---- a clean Messages -----------------------------------------------------
# Relaunched, because a running Messages keeps the extension's bundle path from
# before the last build (throwin_slide.sh has the whole story).
"$RIG" ruler on >/dev/null
rm -f "$G/dev.stage" "$G/dev.staged" "$G/dev.claimed" "$G/dev.flags"
# FOOLISH_FLAGS='table.slide=0' runs the reel against a flag's other state -
# which is how the reel itself is checked: it must report what that puts back.
[ -n "${FOOLISH_FLAGS:-}" ] && printf '%s' "$FOOLISH_FLAGS" > "$G/dev.flags"
xcrun simctl terminate "$FOOLISH_SIM" com.apple.MobileSMS >/dev/null 2>&1
xcrun simctl launch "$FOOLISH_SIM" com.apple.MobileSMS >/dev/null 2>&1
"$RIG" enter >/dev/null 2>&1 || { echo "could not enter a conversation" >&2; exit 2; }
"$RIG" clearstage >/dev/null 2>&1 || true

bad=0
miss() { say "!! $1"; echo "$1" >> "$D/missed.txt"; bad=1; }

# Open onto a seed. `seat` is where we sit; empty lets seed.py choose. The last
# board's move is still STAGED in the compose field - an Undo leaves the
# pre-move board there - so it is cleared first: a draft from another game
# riding along over this one is a frame nobody could produce.
reopen() {   # reopen SEAT MODE ARGS...
  local seat="$1"; shift
  "$RIG" clearstage >/dev/null 2>&1 || true
  if [ -n "$seat" ]; then SEAT="$seat" "$RIG" seed "$@" >/dev/null
  else "$RIG" seed "$@" >/dev/null; fi
  "$RIG" killappex >/dev/null 2>&1
  "$RIG" open >/dev/null 2>&1
  # …AND WAIT FOR THE BOARD. `open` returns when the drawer is up, and a cold
  # extension can take several seconds more to draw into it: the second reel
  # tried its covers on a black drawer and reported "no legal cover" for boards
  # that had one.
  local i=0
  while [ $i -lt 60 ]; do
    [ "$(python3 "$LIB/ui.py" hand_y | awk '{print $2}')" != "-1" ] && return 0
    sleep 0.5; i=$((i + 1))
  done
  miss "board never drew after: seed $*"
}

# The seat that still has to answer on a --goodwait board: the one that can
# throw in. The searcher names it ("us=seat N").
goodwait_seat() {
  "$REPO/c/build/msg_wire_test" --goodwait "$1" 2>&1 >/dev/null \
    | sed -n 's/.*us=seat \([0-9]*\).*/\1/p' | head -1
}

# ---- roll -----------------------------------------------------------------
REC0=$(now)
printf 'rec0 %s\n' "$REC0" > "$D/marks.txt"    # tablesquares.py aligns marks to the film by it
xcrun simctl io "$FOOLISH_SIM" recordVideo --codec h264 --force "$D/take.mp4" >/dev/null 2>&1 &
rec=$!
mark() { printf '%.2f %s\n' "$(echo "$(now) - $REC0" | bc)" "$1" >> "$D/marks.txt"; say "$1"; }

# The action plank: the lowest wooden bar, at the column every pill shares.
plank() {   # plank LABEL [SETTLE]
  local W H y
  read -r W H < <(python3 "$LIB/ax.py" screen)
  y=$(python3 "$LIB/ui.py" bars | python3 -c "
import sys, ast
b = ast.literal_eval(sys.stdin.read().split('BARS ')[1]); print(b[-1][0] if b else -1)")
  [ "$y" = "-1" ] && { miss "$1: no plank"; return 1; }
  mark "$1"
  "$RIG" tap $((W * 4 / 5)) "$y" "${2:-2.6}" >/dev/null 2>&1
}

throwin() {   # throwin LABEL
  local px py
  read -r px py < <("$RIG" throwin select) || { miss "$1: no throw-in found"; return 1; }
  mark "$1";  "$RIG" tap "$px" "$py" 1.8 >/dev/null 2>&1
}

cover() {     # cover LABEL - the cover stages as the plank changes, so mark first
  mark "$1"
  "$RIG" cover >/dev/null 2>&1 || { miss "$1: no legal cover"; return 1; }
  sleep 2.6
}

# ---- the scenarios --------------------------------------------------------
# Live moves, each undone - an undo is its own animation (the exact reverse).
mark "open: 2p attacker (goodwait)";  reopen 0 goodwait 2
throwin "2p throw in" && plank "2p undo throw-in"
plank "2p GOOD - ends the bout (sweep + deal)" 3.4 && plank "2p undo good" 3.4

mark "open: 2p defender (fatboard)";  reopen "" fatboard 4 2
cover "2p cover (bout stays open)" && plank "2p undo cover"
plank "2p PICKUP" 3.4 && plank "2p undo pickup" 3.4

mark "open: 2p last defence";  reopen "" lastdefense 2
cover "2p COVER that ends the bout" && sleep 1.2 && plank "2p undo bout-ending cover" 3.4

s8=$(goodwait_seat 8)
mark "open: 8p attacker (goodwait)";  reopen "${s8:-0}" goodwait 8
throwin "8p throw in" && plank "8p undo throw-in"
plank "8p GOOD - ends the bout" 3.4 && plank "8p undo good" 3.4

# Replayed arrivals: someone else's move, played back as the board opens.
for kind in attack cover covertrump pickup goodany refill out final; do
  mark "open: replay $kind"
  REPLAY=1 reopen "" lastmove "$kind" 2
  mark "replay $kind"
  sleep 3.2
done

kill -INT $rec 2>/dev/null || true
i=0
while [ $i -lt 80 ]; do
  a=$(stat -f%z "$D/take.mp4" 2>/dev/null || echo 0); sleep 0.2
  b=$(stat -f%z "$D/take.mp4" 2>/dev/null || echo 0)
  [ "$a" = "$b" ] && [ "$a" != 0 ] && break
  i=$((i + 1))
done
"$RIG" clearstage >/dev/null 2>&1 || true

# ---- read it --------------------------------------------------------------
python3 "$LIB/tablesquares.py" "$D/take.mp4" --marks "$D/marks.txt" --csv "$D/squares.csv"
rc=$?
rm -f "$G/dev.flags"
echo "$D"
[ -s "$D/missed.txt" ] && { echo "scenarios that did not run:"; sed 's/^/  /' "$D/missed.txt"; }
[ $bad -ne 0 ] && [ $rc -eq 0 ] && rc=2
exit $rc
