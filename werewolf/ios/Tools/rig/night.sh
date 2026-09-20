#!/usr/bin/env bash
# THE NIGHT RIG. Drives the real WerewolfMessages extension inside Apple's real
# Messages app on a simulator, so that one operator can be every player in one
# thread and a whole night can be looked at.
#
# WHY IT HAS TO BE THE REAL HOST. A werewolf night is a claim about what several
# people can see, and the claim is only observable end to end where the bubbles
# really are: you cannot add participants to Messages (it is a signed Apple
# binary), so the solo seat picker (WerewolfKit/SoloSeatPicker.swift, DEBUG only)
# is the mechanism, and this script is how it gets exercised.
#
# It is a SMALL sibling of ios/Tools/rig/rig.sh, which came over from the fork and
# knows far more about filming and photography than this needs. What is reused is
# the part that is not about cards: lib/ax.py, which finds a control by
# accessibility label in device points on any device.
#
#   ios/Tools/rig/night.sh build     kernel -> xcframework -> project -> install
#   ios/Tools/rig/night.sh stage     status bar, appearance, Apple's first-run sheets
#   ios/Tools/rig/night.sh flag      switch the solo seat picker on (needs one open first)
#   ios/Tools/rig/night.sh open      enter a thread and open our drawer
#   ios/Tools/rig/night.sh shot NAME screenshot into $WW_OUT
#   ios/Tools/rig/night.sh tap LABEL tap a control by accessibility label
#   ios/Tools/rig/night.sh xy X Y    tap a point, in POINTS
#   ios/Tools/rig/night.sh dump      every labelled element on screen
#
# WW_SIM must name a simulator this session owns. Use your own - two rigs on one
# device fight over the drawer, and the loser's taps land in the winner's game.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
LIB="$HERE/lib"
SIM="${WW_SIM:-}"
OUT="${WW_OUT:-/tmp/werewolf-shots}"
APP_ID="cards.werewolf.msg"
GROUP_ID="group.cards.werewolf.msg"
DD="${WW_DD:-/tmp/wwdd}"
IDB="${WW_IDB:-idb}"

need_sim() { [ -n "$SIM" ] || { echo "set WW_SIM to your own simulator's udid" >&2; exit 2; }; }

# lib/ax.py reads FOOLISH_SIM, because it came over from the fork unchanged. It is
# EXPORTED here rather than the file edited: ax.py is generic (it finds a label and
# returns points, and knows nothing about either game), so it is worth keeping
# byte-identical to the tree it came from - the day these two products share a
# core, that is one fewer file to reconcile.
export FOOLISH_SIM="$SIM"

tap_xy() { need_sim; "$IDB" ui tap --udid "$SIM" --duration 0.12 "$1" "$2" >/dev/null 2>&1; sleep "${3:-1}"; }
ax()     { python3 "$LIB/ax.py" find "$1" --exact 2>/dev/null; }
ax_sub() { python3 "$LIB/ax.py" find "$1" 2>/dev/null; }

cmd_build() {
  need_sim
  make -C "$REPO/c" tests
  make -C "$REPO/c" ios-lib
  ( cd "$REPO/ios" && xcodegen generate >/dev/null )
  # xcodegen rewrites the entitlements from project.yml `properties`; put the
  # tracked bytes back so a stray diff never rides along in a commit.
  ( cd "$REPO" && git checkout -- $(cd "$REPO" && git ls-files -- '*.entitlements') )
  xcodebuild -project "$REPO/ios/Werewolf.xcodeproj" -scheme WerewolfMessagesApp \
    -configuration Debug -destination "platform=iOS Simulator,id=$SIM" \
    -derivedDataPath "$DD" -quiet build | tail -3
  # Install OVER the old build. `simctl uninstall` destroys the App Group
  # container, and it comes back with a fresh UUID - which takes the solo flag
  # with it.
  xcrun simctl install "$SIM" "$DD/Build/Products/Debug-iphonesimulator/WerewolfMessagesApp.app"
  echo "installed on $SIM"
}

cmd_stage() {
  need_sim
  xcrun simctl status_bar "$SIM" override --time "9:41" --batteryState charged \
      --batteryLevel 100 --cellularBars 4 --wifiBars 3 --dataNetwork wifi
  xcrun simctl ui "$SIM" appearance "${1:-dark}" >/dev/null
  xcrun simctl launch "$SIM" com.apple.MobileSMS >/dev/null
  sleep 3
  # Apple's first-run sheets each silently eat the first tap of a run.
  local i
  for i in 1 2 3; do
    local p
    p=$(ax_sub "Continue") || p=""
    [ -n "$p" ] && { tap_xy $p 1; continue; }
    p=$(ax_sub "Not Now") || p=""
    [ -n "$p" ] && { tap_xy $p 1; continue; }
    break
  done
  echo "staged"
}

# The App Group container, which only EXISTS once the extension has run and
# written to it - so `flag` has to come after the first `open`, not before.
group_dir() {
  need_sim
  local d id
  for d in ~/Library/Developer/CoreSimulator/Devices/"$SIM"/data/Containers/Shared/AppGroup/*/; do
    id=$(plutil -extract MCMMetadataIdentifier raw \
         "$d/.com.apple.mobile_container_manager.metadata.plist" 2>/dev/null || true)
    [ "$id" = "$GROUP_ID" ] && { echo "${d%/}"; return; }
  done
  echo "no App Group container on $SIM yet - run 'open' once first" >&2
  return 1
}

cmd_flag() {
  local g; g=$(group_dir) || exit 1
  printf 'solo.seatpicker=%s' "${1:-1}" > "$g/dev.flags"
  echo "wrote $g/dev.flags: $(cat "$g/dev.flags")"
  echo "the flags file is read ONCE per appex process, so leave the drawer and come back"
}

cmd_open() {
  need_sim
  read -r W H < <(python3 "$LIB/ax.py" screen)
  # Into the first conversation in the list.
  local p; p=$(ax_sub "Kate Bell") || p=""
  [ -z "$p" ] && p="$((W / 2)) $((H * 22 / 100))"
  tap_xy $p 2
  # The app drawer. The "+" is `add` in the tree; the menu's own rows are the
  # honest evidence that it opened, because iOS 27 no longer puts a dismissal
  # target there.
  local m=0
  while [ $m -lt 3 ]; do
    ax_sub "Photos" >/dev/null 2>&1 && break
    local a; a=$(ax "add") || a=""
    [ -n "$a" ] && tap_xy $a 1.5
    m=$((m + 1))
  done
  # Werewolf is below the fold on a stock device, so scroll the menu before
  # looking for it.
  local s=0
  while [ $s -lt 6 ]; do
    local w; w=$(ax_sub "Werewolf") || w=""
    [ -n "$w" ] && { tap_xy $w 4; echo "opened"; return 0; }
    "$IDB" ui swipe --udid "$SIM" --duration 0.3 \
        $((W / 2)) $((H * 80 / 100)) $((W / 2)) $((H * 55 / 100)) >/dev/null 2>&1
    sleep 0.6
    s=$((s + 1))
  done
  echo "could not find Werewolf in the app drawer" >&2
  return 1
}

cmd_shot() {
  need_sim
  mkdir -p "$OUT"
  local f="$OUT/${1:-shot}.png"
  xcrun simctl io "$SIM" screenshot --type=png "$f" >/dev/null 2>&1
  echo "$f"
}

cmd_tap() {
  need_sim
  local p; p=$(ax_sub "$1") || { echo "no '$1' on screen" >&2; return 1; }
  tap_xy $p "${2:-1.2}"
  echo "tapped $1"
}

# OUR OWN DRAWER IS NOT IN THE ACCESSIBILITY TREE. A presented Messages drawer
# hides everything under it AND everything in it: `describe-all` reports the
# Messages application, the thread's Back button, the "+" and the compose field,
# and nothing of ours at all. So `tap` (by label) reaches Apple's chrome and this
# reaches our screens, in points off a screenshot. Not a workaround for a missing
# label - the labels are there, and the host does not publish them.
cmd_xy() { need_sim; tap_xy "$1" "$2" "${3:-1.2}"; echo "tapped $1,$2"; }

cmd_dump() { need_sim; python3 "$LIB/ax.py" dump "${1:-}"; }

case "${1:-}" in
  build) shift; cmd_build "$@" ;;
  stage) shift; cmd_stage "$@" ;;
  flag)  shift; cmd_flag "$@" ;;
  open)  shift; cmd_open "$@" ;;
  shot)  shift; cmd_shot "$@" ;;
  tap)   shift; cmd_tap "$@" ;;
  xy)    shift; cmd_xy "$@" ;;
  dump)  shift; cmd_dump "$@" ;;
  group) group_dir ;;
  *) sed -n '1,30p' "$0" | sed 's/^# \{0,1\}//' ;;
esac
