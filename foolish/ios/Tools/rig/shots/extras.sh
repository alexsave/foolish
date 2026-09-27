#!/bin/bash
# extras.sh - the frames a shot LIST cannot describe, because they are reached
# by tapping rather than by opening a seeded chain: the lobby, the rules sheet
# and the settings sheet, in both appearances.
#
# Run it after the batch lists, in the SAME Messages session - the transcript
# only exists in memory (see the README).
#
#   FOOLISH_SIM=... FOOLISH_OUT=~/Downloads/shots ios/Tools/rig/shots/extras.sh
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
RIG="$HERE/../rig.sh"
LIB="$HERE/../lib"
SHLIB="$HERE/../../../../shared/rig/lib"  # the shared measurement half (no product knowledge in it)

read -r W H < <(python3 "$SHLIB/ax.py" screen)

bar_y() {
  python3 "$LIB/ui.py" bars | python3 -c "
import sys, ast
b = ast.literal_eval(sys.stdin.read().split('BARS ')[1])
print(b[${1:-0}][0] if b else -1)"
}

# The gear and the book are small wooden SQUARES on the board's control row.
# `wood_bars` only reports planks 120pt or wider, so asking it for "the lowest
# bar" returned whatever wide button happened to sit lower - which tapped Add
# player and produced two frames of the wrong surface. `wood_icons` reports the
# squares themselves.
icon() {
  python3 "$LIB/ui.py" icons | python3 -c "
import sys, ast
i = ast.literal_eval(sys.stdin.read().split('ICONS ')[1])
print('%d %d' % i[$1] if len(i) > $1 else '-1 -1')"
}
gear() { "$RIG" tap $(icon 0) 2.5; }
book() { "$RIG" tap $(icon 1) 2.5; }

# ---- the lobby --------------------------------------------------------------
# ONLY a FULL lobby is shippable. `soloControls` (MessagesRootView) replaces the
# real Start/Exit row with a DEBUG "Add player (testing)" button whenever the
# lobby still has room, and seeding needs a DEBUG build - so a lobby with a free
# seat cannot be photographed at all. Note the puppet seat is named "Solo 2".
"$RIG" unseed >/dev/null
"$RIG" lobby 2 >/dev/null
"$RIG" shot 90_lobby/2p_full_expanded
"$RIG" collapse >/dev/null
"$RIG" shot 90_lobby/2p_full_compact
"$RIG" clearstage >/dev/null

# ---- rules and settings, dark then light ------------------------------------
for appear in dark light; do
  "$RIG" prefs "" "" "$appear" >/dev/null
  "$RIG" seed fatboard 6 4 pass 0 >/dev/null
  "$RIG" back >/dev/null; "$RIG" open >/dev/null; "$RIG" expand >/dev/null
  book
  "$RIG" shot "92_rules/${appear}_page1"
  # The rules sheet SCROLLS, it does not paginate. A horizontal swipe moved
  # nothing and produced three identical frames per appearance.
  for page in 2 3 4; do
    "$RIG" swipe 0.5 $((W / 2)) $((H * 78 / 100)) $((W / 2)) $((H * 26 / 100)) 1.5
    "$RIG" shot "92_rules/${appear}_page${page}"
  done
  "$RIG" back >/dev/null; "$RIG" open >/dev/null; "$RIG" expand >/dev/null
  gear
  "$RIG" shot "93_settings/${appear}"
  "$RIG" swipe 0.5 $((W / 2)) $((H * 78 / 100)) $((W / 2)) $((H * 40 / 100)) 1.5
  "$RIG" shot "93_settings/${appear}_lower"
done
"$RIG" prefs "" "" dark >/dev/null
echo "extras done"
