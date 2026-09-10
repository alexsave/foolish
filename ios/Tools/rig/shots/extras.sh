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

read -r W H < <(python3 "$LIB/ax.py" screen)

bar_y() {
  python3 "$LIB/ui.py" bars | python3 -c "
import sys, ast
b = ast.literal_eval(sys.stdin.read().split('BARS ')[1])
print(b[${1:-0}][0] if b else -1)"
}

# The gear and the book are small wooden squares on the board's own control
# row, so they share that row's y - which `bars` reports - and sit at a tenth
# and just over a fifth of the width. They are too narrow for `wood_bars` to
# report on their own, which is why the y comes from the row and the x from a
# proportion.
gear() { "$RIG" tap $((W * 10 / 100)) "$(bar_y -1)" 2.5; }
book() { "$RIG" tap $((W * 22 / 100)) "$(bar_y -1)" 2.5; }

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
  "$RIG" swipe 0.4 $((W * 80 / 100)) $((H / 2)) $((W * 20 / 100)) $((H / 2)) 1.5
  "$RIG" shot "92_rules/${appear}_page2"
  "$RIG" swipe 0.4 $((W * 80 / 100)) $((H / 2)) $((W * 20 / 100)) $((H / 2)) 1.5
  "$RIG" shot "92_rules/${appear}_page3"
  "$RIG" back >/dev/null; "$RIG" open >/dev/null; "$RIG" expand >/dev/null
  gear
  "$RIG" shot "93_settings/${appear}"
done
"$RIG" prefs "" "" dark >/dev/null
echo "extras done"
