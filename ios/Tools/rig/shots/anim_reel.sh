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
# The scenarios, in order:
#   2p   throw in, undo, throw in again, undo       (`--goodwait 2`, live taps)
#   8p   throw in, undo                              (`--goodwait 8`)
#   replayed arrivals, each opened fresh: an attack, a cover that leaves the
#   bout open, a pickup, and a good (`--lastmove <kind> 2`, REPLAY=1)
#
# The marks are wall-clock seconds from the moment the recorder was started,
# and the movie's clock starts at its first frame, a little later - so a
# reported time can sit a fraction of a second after the mark it is filed
# under. Every scenario is given a second or more, which is what makes that
# harmless.
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

# Open onto a seed. `seat` is where we sit; empty lets seed.py choose.
reopen() {   # reopen SEAT MODE ARGS...
  local seat="$1"; shift
  if [ -n "$seat" ]; then SEAT="$seat" "$RIG" seed "$@" >/dev/null
  else "$RIG" seed "$@" >/dev/null; fi
  "$RIG" killappex >/dev/null 2>&1
  "$RIG" open >/dev/null 2>&1
}

# The seat that still has to answer on a --goodwait board: the one that can
# throw in. The searcher names it ("us=seat N").
goodwait_seat() {
  "$REPO/c/build/msg_wire_test" --goodwait "$1" 2>&1 >/dev/null \
    | sed -n 's/.*us=seat \([0-9]*\).*/\1/p' | head -1
}

# ---- roll -----------------------------------------------------------------
REC0=$(now)
xcrun simctl io "$FOOLISH_SIM" recordVideo --codec h264 --force "$D/take.mp4" >/dev/null 2>&1 &
rec=$!
mark() { printf '%.2f %s\n' "$(echo "$(now) - $REC0" | bc)" "$1" >> "$D/marks.txt"; say "$1"; }
bad=0

throw_and_undo() {   # throw_and_undo LABEL
  local px py
  if ! read -r px py < <("$RIG" throwin select); then
    say "!! $1: no throw-in found"; bad=1; return
  fi
  mark "$1 throwin";  "$RIG" tap "$px" "$py" 1.6 >/dev/null 2>&1
  mark "$1 undo";     "$RIG" tap "$px" "$py" 1.6 >/dev/null 2>&1
}

mark "2p open";  reopen 0 goodwait 2
throw_and_undo "2p"
throw_and_undo "2p again"

s8=$(goodwait_seat 8)
mark "8p open";  reopen "${s8:-0}" goodwait 8
throw_and_undo "8p"

for kind in attack cover pickup goodany; do
  mark "open ($kind)"
  REPLAY=1 reopen "" lastmove "$kind" 2
  mark "replay $kind"
  sleep 3
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
[ $bad -ne 0 ] && [ $rc -eq 0 ] && rc=2
exit $rc
