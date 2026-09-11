#!/bin/bash
# rig.sh - the ONE rig. Drives the real FoolishMessages extension inside
# Apple's real Messages app on a simulator, deterministically.
#
# It replaces five that had drifted apart: ios/Tools/msgrig.sh (scenarios,
# film, contact sheets), ~/Downloads/foolish-store-reshot/rig/* (store
# photography, cfprefsd prefs, the named cast), scratchpad/rigw.sh (install,
# flight log), scratchpad/rig.sh (taps and shots) and scratchpad/burst.sh.
# Each knew something the others did not and each was one machine away from
# being lost; this file is in the repo so that stops happening.
#
#   ---- setup ----------------------------------------------------------
#   rig.sh doctor                 what is missing, and how to get it
#   rig.sh newsim [NAME] [TYPE]   create + boot a shoot simulator
#   rig.sh build                  kernel -> xcframework -> app -> install
#   rig.sh stage                  9:41 status bar, appearance, first-run sheets
#   rig.sh setname NAME           type the local player's name into the prompt
#   rig.sh nickname NAME          rewrite it later, without the UI
#
#   ---- the session ----------------------------------------------------
#   rig.sh enter                  open the first conversation
#   rig.sh session [N]            TEXT the open thread: N of our own bubbles,
#                                 so the transcript behind every later frame
#                                 is a real chat and not a black void
#   rig.sh open                   +  ->  Foolish  (the extension, compact)
#   rig.sh chain NAME [n] [depth] a transcript of REAL consecutive moves:
#                                 caption / caption / caption / one bubble
#   rig.sh tapopen [thread]       open it by TAPPING the newest bubble, so the
#                                 next send shares that message's MSSession
#   rig.sh clearstage             dismiss a staged bubble left in the compose field
#   rig.sh expand / collapse      drag the grabber
#   rig.sh back                   leave the drawer (keeps Messages alive)
#
#   ---- state ----------------------------------------------------------
#   rig.sh lobby N                a LOBBY with N seats filled (DEBUG button)
#   rig.sh play                   select a LEGAL card and press the plank
#   rig.sh turn                   play a move AND send it, so the transcript's
#                                 last bubble is the board now on screen
#   rig.sh seed MODE ARGS...      dev.fatboard via c/build/msg_wire_test
#                                 fatboard <cards> <players> [nopass] [preroll]
#                                 endgame <players> [nopass]
#                                 lastdefense <players> | twocover <players>
#   rig.sh unseed                 back to the normal create/join flow
#   rig.sh prefs [TABLE] [LANG] [APPEARANCE]      felt|wool  en|ru|..  light|dark
#   rig.sh slowmo N | ruler [off]                 debug overlays
#
#   ---- capture --------------------------------------------------------
#   rig.sh shot NAME              one frame, flattened, size-checked
#   rig.sh batch LIST             a whole shot list (see shots/*.txt)
#   rig.sh burst LABEL N          N screenshots as fast as they come
#   rig.sh film NAME SECS [CMD..] record, keeping every composited frame
#   rig.sh sheet NAME [ARGS]      contact sheets from a take or a directory
#   rig.sh probe                  screen size + what the colour finder sees
#   rig.sh flight | mem | log     the extension's own diagnostics
#
# THE ONE RULE THAT IS NOT OBVIOUS: **do not restart Messages mid-shoot.**
# The simulator's Messages keeps its conversations IN MEMORY - there is no
# writable sms.db behind it (a fresh device has no data/Library/SMS at all, a
# sent message lands in no file on disk, and both survive exactly until the
# app is terminated). So the transcript that `session` types is lost the
# moment Messages is relaunched. Everything below therefore re-enters the
# DRAWER instead of relaunching the app - which is also what makes a re-seed
# take: leaving and re-entering kills the appex, and `claimSeededPayload()` is
# once per appex process, so the next open reads the new `dev.fatboard`.
#
# Automation is idb, never cliclick: cliclick needs window focus and a lost
# mouse-up leaves a touch stuck down until the simulator reboots. idb injects
# HID directly, in DEVICE POINTS (not pixels).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
LIB="$HERE/lib"
SIM="${FOOLISH_SIM:-}"
IDB="${FOOLISH_IDB:-idb}"
OUT="${FOOLISH_OUT:-$HOME/Downloads/foolish-shots}"
APP_ID=cards.foolish.msg
EXT_DOM=cards.foolish.msg.MessagesExtension
# DerivedData is PER SIMULATOR, which is to say per task (rule 5). One shared
# DerivedData is the build-side version of sharing a device, and it fails the
# same silent way: a concurrent agent building a WORKTREE writes its own
# `ios_api.h` into the shared tree, and every later build here dies with
# "file has been modified since the module file was built: size changed" -
# a failure that names a header this checkout never touched.
DD="${FOOLISH_DD:-/tmp/foolishDD-${FOOLISH_SIM:0:8}}"
export FOOLISH_SIM FOOLISH_IDB

need_sim() { [ -n "$SIM" ] || { echo "set FOOLISH_SIM (rig.sh newsim prints one)" >&2; exit 2; }; }

tap()   { need_sim; "$IDB" ui tap --udid "$SIM" "$1" "$2" >/dev/null 2>&1; sleep "${3:-1}"; }
swipe() { need_sim; "$IDB" ui swipe --udid "$SIM" --duration "$1" "$2" "$3" "$4" "$5" >/dev/null 2>&1; sleep "${6:-1}"; }
type_s(){ need_sim; "$IDB" ui text --udid "$SIM" "$1" >/dev/null 2>&1; sleep "${2:-1}"; }

# The screen in POINTS, from the accessibility tree's root - the one thing
# that reports points on every device without a lookup table.
screen() { need_sim; python3 "$LIB/ax.py" screen; }

# Messages' OWN chrome is found BY ACCESSIBILITY LABEL, not by a coordinate
# table. This was the rig's biggest single source of drift: every earlier
# version pinned "the compose +" and "the conversation row" to numbers read off
# one phone, and a missed tap does not fail - it opens something else and
# yields a frame that passes every size and naming check. (The run that found
# this typed five perfectly good lines into the conversation list's SEARCH
# field and reported "5 bubbles in the thread".)
#
# Inside a THREAD the tree carries exactly what is needed:
#     Messages   Button     the back chevron
#     add        Button     the "+"
#     Message    TextField  the compose field
#     Send       Button     appears only once there is text to send
# and the "+" menu lists every app by name, so "Foolish" is found rather than
# guessed. What the tree does NOT carry is a conversation row (the list reports
# four elements, none of them a row) or anything at all inside our own
# extension, which is a separate process - those are found by colour instead
# (lib/ui.py), and the conversation row is probed and VERIFIED below.
# EXACT labels, always. Substring matching looks friendlier and is a trap:
# "Message" (the compose field) is a substring of "Messages" (the back
# chevron), the chevron is the SMALLER element, and `find` returns the
# smallest match - so a loose lookup for the compose field reliably tapped
# BACK OUT OF THE THREAD, five times in a row, reporting nothing wrong.
ax() { python3 "$LIB/ax.py" find "$1" --exact; }

# `idb ui describe-all` answers with the LAST FOREGROUND app's tree even when
# something else is on screen, so a stale tree looks exactly like a live one
# and every tap derived from it silently misses. Re-activating Messages is
# cheap and does NOT restart it (the transcript, which only exists in memory,
# survives) - so do it before anything that navigates.
front() {
  need_sim
  xcrun simctl launch "$SIM" com.apple.MobileSMS >/dev/null 2>&1 || true
  sleep 2
}

in_thread() { ax "add" >/dev/null 2>&1; }

# In a thread whose header matches `$1` (empty = any thread). The header is a
# Button carrying the remote address, which is the only stable way to tell the
# two stub conversations apart - the LIST reorders by recency, so "row 1" is
# not a thread, it is a coin flip.
here_is() {
  in_thread || return 1
  [ -z "${1:-}" ] && return 0
  python3 "$LIB/ax.py" dump "$1" | grep -q Button
}

# The drawer's own top edge, found by colour - "None" when no drawer is up.
grab_y() { python3 "$LIB/ui.py" top | awk '{print $2}'; }

# Where a DOWNWARD drag on the drawer may start. Never the drawer's own top
# edge when it is expanded: that edge sits within a few points of the screen
# top, and a downward swipe from there is iOS's own Notification Centre
# gesture - which pulls the shade down, leaves the simulator on the LOCK
# SCREEN, and makes every subsequent accessibility read return Messages' last
# known tree while nothing of Messages is on screen.
pull_y() {
  local top="$1" h="$2"
  # NB: a single `local a=$1 b=$((a*2))` does not work - every word on a
  # `local` line is expanded before any of them is assigned.
  local floor=$((h * 9 / 100))
  [ "$top" = "None" ] && { echo "$floor"; return; }
  if [ $((top + 6)) -lt "$floor" ]; then echo "$floor"; else echo $((top + 6)); fi
}

tap_ax() {
  local pt; pt=$(ax "$1") || { echo "no '$1' on screen" >&2; return 1; }
  tap $(echo "$pt" | awk '{print $1, $2}') "${2:-1}"
}

group_dir() {
  need_sim
  local d id
  for d in ~/Library/Developer/CoreSimulator/Devices/"$SIM"/data/Containers/Shared/AppGroup/*/; do
    id=$(plutil -extract MCMMetadataIdentifier raw \
         "$d/.com.apple.mobile_container_manager.metadata.plist" 2>/dev/null || true)
    [ "$id" = "group.cards.foolish.msg" ] && { echo "${d%/}"; return; }
  done
  # Printing nothing here would make every caller write to "/dev.slowmo", which
  # on a machine with a writable root would silently do the wrong thing.
  echo "no App Group container on $SIM - install the app first" >&2
  echo "/nonexistent/group.cards.foolish.msg"
}

# ---------------------------------------------------------------- setup ----

cmd_doctor() {
  local bad=0
  command -v "$IDB" >/dev/null || { echo "MISSING idb - brew install facebook/fb/idb-companion && python3.12 -m venv ~/.venvs/idb && ~/.venvs/idb/bin/pip install fb-idb"; bad=1; }
  command -v ffmpeg >/dev/null || { echo "MISSING ffmpeg (only 'film'/'sheet' need it) - brew install ffmpeg"; bad=1; }
  python3 -c "import PIL, numpy" 2>/dev/null || { echo "MISSING pillow/numpy - pip3 install pillow numpy"; bad=1; }
  [ -x "$REPO/c/build/msg_wire_test" ] || { echo "MISSING seeder - (cd c && make build/msg_wire_test)"; bad=1; }
  if [ -n "$SIM" ]; then
    xcrun simctl list devices booted | grep -q "$SIM" \
      && echo "sim $SIM booted, $(screen) pt" || echo "sim $SIM NOT booted"
    xcrun simctl get_app_container "$SIM" "$APP_ID" >/dev/null 2>&1 \
      && echo "app installed" || { echo "MISSING app - rig.sh build"; bad=1; }
  else
    echo "FOOLISH_SIM unset - rig.sh newsim"
    bad=1
  fi
  [ "$bad" = 0 ] && echo "all good"
  return 0
}

cmd_newsim() {
  local name="${1:-FoolishShoot}"
  local type="${2:-com.apple.CoreSimulator.SimDeviceType.iPhone-17-Pro-Max}"
  local rt; rt=$(xcrun simctl list runtimes | awk '/iOS/ {print $NF}' | tail -1)
  local udid; udid=$(xcrun simctl create "$name" "$type" "$rt")
  xcrun simctl boot "$udid"; xcrun simctl bootstatus "$udid" -b >/dev/null
  echo "export FOOLISH_SIM=$udid   # $name"
}

cmd_build() {
  need_sim
  make -C "$REPO/c" ios-lib
  make -C "$REPO/c" build/msg_wire_test
  (cd "$REPO/ios" && xcodegen generate)
  # xcodegen BLANKS the entitlements files every run; without this the
  # extension loses its App Group and every seed silently does nothing.
  (cd "$REPO" && git checkout -- $(cd "$REPO" && git ls-files -- '*.entitlements'))
  local name; name=$(xcrun simctl list devices | grep "$SIM" | sed 's/ (.*//;s/^ *//')
  # DEBUG, not Release: `dev.fatboard` seeding is #if DEBUG.
  xcodebuild -project "$REPO/ios/Foolish.xcodeproj" -scheme FoolishMessagesApp \
    -configuration Debug -destination "platform=iOS Simulator,id=$SIM" \
    -derivedDataPath "$DD" build | tail -3
  # Install OVER the old build. `simctl uninstall` destroys the App Group and
  # the appex's Preferences container, and both come back with fresh UUIDs.
  xcrun simctl install "$SIM" "$DD/Build/Products/Debug-iphonesimulator/FoolishMessagesApp.app"
  echo "installed on $SIM"
}

# The stage: a clean status bar, an appearance, and Apple's first-run sheets
# gone. Two of them ("Shared with You", "Apple Intelligence in Messages")
# appear on a fresh device and each one silently eats the first tap of a run.
cmd_stage() {
  need_sim
  local appear="${1:-dark}"
  xcrun simctl status_bar "$SIM" override --time "9:41" --batteryState charged \
      --batteryLevel 100 --cellularBars 4 --wifiBars 3 --dataNetwork wifi
  xcrun simctl ui "$SIM" appearance "$appear" >/dev/null
  xcrun simctl launch "$SIM" com.apple.MobileSMS >/dev/null; sleep 6
  read -r W H < <(screen)
  # Dismiss whatever onboarding is up: both sheets put their button on the
  # bottom eighth, centred. Tapping there twice is harmless once they are gone
  # (it lands in the conversation list's empty space or the search field).
  # Apple's first-run sheets DO name their buttons in the accessibility tree
  # ("OK" for Shared with You, "Continue" for Apple Intelligence), so ask for
  # them rather than measuring a pill. Two things were learned the hard way:
  # the sheets put their button at different heights, so a fixed y dismissed
  # the first and hit "Edit in Settings" on the second - which walked the run
  # into the Settings app; and a sheet can appear SECONDS after launch, so a
  # loop that stops at the first clear look stops before the sheet exists.
  # Hence: keep looking for a while, and only give up after several quiet
  # passes in a row.
  # A leftover MODAL survives a relaunch and swallows every later tap - a New
  # Contact sheet opened by hand once ate an entire four-take batch, whose
  # frames all came back as the same grey editor. Clear those first, and
  # confirm any "discard changes" they raise.
  local m=0
  while [ $m -lt 3 ]; do
    tap_ax "Cancel" 2 2>/dev/null || tap_ax "Back" 2 2>/dev/null || break
    tap_ax "Discard Changes" 2 2>/dev/null || true
    m=$((m + 1))
  done
  local quiet=0 i=0
  while [ $i -lt 12 ] && [ $quiet -lt 4 ]; do
    if tap_ax "OK" 3 2>/dev/null || tap_ax "Continue" 3 2>/dev/null; then
      quiet=0
    else
      quiet=$((quiet + 1))
      sleep 2
    fi
    i=$((i + 1))
  done
  echo "staged: $appear, ${W}x${H}pt"
}

# The local player's display name. It lives in the APP GROUP suite as
# `fmsg.nickname` (MessageGameStore), which is a different store from the
# extension's own preferences domain that `prefs` writes - and unlike that one
# it is NOT reachable through `simctl spawn defaults`, which answers "Domain
# group.cards.foolish.msg does not exist" because the group domain is only
# registered inside the sandbox that owns it. So this edits the container's
# plist directly, and the key has a DOT in it, which plutil reads as a path
# separator unless it is escaped - `-replace fmsg.nickname` fails with "Key
# path not found" while looking like a name that is simply absent.
# Type the name into the extension's own "New game" prompt. Needed exactly
# once per fresh simulator, and it has to be the UI rather than a file write:
# the App Group preferences plist does not EXIST until the extension has
# written to it, so there is nothing for `nickname` to edit yet.
#
# The field is ours, so it is not in the accessibility tree; it sits directly
# above the "Enter Nickname" wooden bar, which lib/ui.py finds by colour.
cmd_setname() {
  local name="${1:-Alex}" y
  cmd_open >/dev/null
  read -r W H < <(screen)
  y=$(bar_y 0)
  [ "$y" = "-1" ] && { echo "no wooden bar on screen - is the prompt up?" >&2; return 1; }
  tap $((W / 2)) $((y - H * 66 / 1000)) 1.5
  type_s "$name" 1
  tap $((W / 2)) "$y" 3
  cmd_nickname "$name"
}

cmd_nickname() {
  local name="${1:-Alex}" g p
  g=$(group_dir); p="$g/Library/Preferences/group.cards.foolish.msg.plist"
  [ -f "$p" ] || { echo "no group prefs yet - run 'rig.sh setname $name' instead" >&2; return 1; }
  plutil -replace 'fmsg\.nickname' -string "$name" "$p"
  echo "nickname: $(plutil -extract 'fmsg\.nickname' raw "$p")"
}

# ------------------------------------------------------------- session ----

# Walk into a thread and TEXT it, so every later frame has a real conversation
# behind the drawer instead of a black void with a phone number in it.
#
# The bubbles are all OURS. The simulator has no iMessage account, nothing ever
# arrives, and there is no writable store to forge an incoming row into - see
# lib/transcript.py, which documents the search. One side of a conversation is
# what this platform can produce, and it is still a transcript.
# Open the first conversation, and PROVE it opened. The row is the one thing
# the accessibility tree will not name, and its y moves with the large-title
# collapse - so the y is probed rather than pinned, and the loop stops on the
# first candidate after which the thread's own compose field exists.
# Open a conversation, and PROVE it opened. The row is the one thing the
# accessibility tree will not name, and its y moves with the large-title
# collapse - so the y is probed rather than pinned. An optional argument is a
# substring of the thread's own header (a phone number), which is how a run
# gets the SAME thread every time instead of whichever one happens to be top.
cmd_enter() {
  need_sim
  local want="${1:-}"
  here_is "$want" && return 0
  read -r W H < <(screen)
  # A presented drawer hides the whole thread from the accessibility tree - an
  # EXPANDED extension reports two elements, the application and a zero-height
  # "Activate to dismiss pop-up window". So if a drawer is up, put it away
  # first; otherwise the row probe below taps five times into our own board.
  local top; top=$(grab_y)
  if [ "$top" != "None" ]; then
    swipe 0.5 $((W / 2)) $(pull_y "$top" "$H") $((W / 2)) $((H * 88 / 100)) 2.5
    here_is "$want" && return 0
  fi
  # Leave whatever thread we are in, so the row probe has a list to probe.
  local i=0
  while [ $i -lt 3 ] && in_thread; do tap_ax "Messages" 2 || break; i=$((i + 1)); done
  local y
  for y in $((H * 24 / 100)) $((H * 20 / 100)) $((H * 15 / 100)) $((H * 28 / 100)) $((H * 32 / 100)); do
    tap $((W / 2)) "$y" 2.5
    here_is "$want" && return 0
    in_thread && { tap_ax "Messages" 2 || true; }
  done
  echo "could not open conversation '${want:-any}'" >&2
  return 1
}

# THE TRANSCRIPT.
#
# The problem: a store frame is a drawer over a chat, and the chat was always
# an empty black rectangle with a phone number on it. The owner: "id like for
# you to set up the emulator and text the two contacts back and forth so there
# are some 'text bubbles' in the chat transcript above."
#
# What does not work: writing rows into `Library/SMS/sms.db`. The simulator's
# Messages keeps its conversations IN MEMORY - a fresh device has no sms.db at
# all, a message sent from the UI lands in no file anywhere on disk, and
# injected rows are never read. (lib/transcript.py holds that search; it is
# kept because the finding is worth more than the code.)
#
# What does work: the runtime ships TWO stub conversations, and a message sent
# in one of them arrives in the OTHER as an INCOMING message - grey, on the
# left. So alternating between the two threads builds a real two-sided
# conversation in whichever one gets shot.
#
# Both sides are ours and the bubbles are SMS green, because the simulator has
# no iMessage account. That is the platform's ceiling, not a choice.
SHOOT_THREAD="${FOOLISH_SHOOT_THREAD:-888}"     # the thread every frame is over
OTHER_THREAD="${FOOLISH_OTHER_THREAD:-8583}"    # the one that "sends" to it

say() {  # say <thread-substring> <text>
  cmd_enter "$1" >/dev/null || return 1
  tap_ax "Message" 1.5 || return 1
  type_s "$2" 1.5
  tap_ax "Send" 2.5
}

cmd_session() {
  need_sim
  front
  local n="${1:-12}" i=0 sent=0
  # them|... arrives as an incoming bubble; me|... goes out as ours. Ordinary
  # and short: a transcript is scenery, and scenery that reads as ad copy is
  # the fastest way to make a store frame look staged.
  local script=(
    "them|one game before dinner?"
    "me|always. deal me in"
    "them|you still owe me from last time"
    "me|that was one hand and you know it"
    "them|sending"
    "me|ok that was brutal"
    "them|ha"
    "me|again tomorrow?"
    "them|bring the good deck"
    "me|there is one deck"
    "them|then bring it"
    "me|deal"
  )
  for line in "${script[@]}"; do
    [ "$i" -ge "$n" ] && break
    local who="${line%%|*}" text="${line#*|}"
    if [ "$who" = "them" ]; then
      say "$OTHER_THREAD" "$text" && sent=$((sent + 1))
    else
      say "$SHOOT_THREAD" "$text" && sent=$((sent + 1))
    fi
    i=$((i + 1))
  done
  cmd_enter "$SHOOT_THREAD" >/dev/null
  echo "session: $sent bubbles, both sides, in the $SHOOT_THREAD thread"
}

cmd_open() {
  need_sim
  cmd_enter "${1:-$SHOOT_THREAD}" >/dev/null
  tap_ax "add" 2.5
  read -r W H < <(screen)
  local i=0
  while [ $i -lt 5 ]; do
    if tap_ax "Foolish" 7; then return 0; fi
    swipe 0.5 $((W * 2 / 5)) $((H * 89 / 100)) $((W * 2 / 5)) $((H * 55 / 100)) 1.5
    i=$((i + 1))
  done
  echo "Foolish is not in the app menu - is the extension installed?" >&2
  return 1
}

# `open`, and if the app menu never appeared, put whatever IS presented away
# and try once more. The usual cause is a drawer left expanded by the previous
# frame: it covers the "+" and reports nothing to the accessibility tree.
cmd_open_retry() {
  cmd_open && return 0
  front
  read -r W H < <(screen)
  local top; top=$(grab_y)
  swipe 0.6 $((W / 2)) $(pull_y "$top" "$H") $((W / 2)) $((H * 90 / 100)) 3
  cmd_open
}

# Leave the drawer and come straight back into the thread. Leaving is what
# kills the appex, and the appex has to die for the next `seed` to be read.
cmd_chain() {
  # chain NAME [count] [depth] - photograph a transcript whose bubbles are a
  # REAL consecutive run of moves.
  #
  # `msg_wire_test --chain` plays one game and seals EVERY state along the way,
  # so bubble N+1 is what bubble N's board became. Invented sequences do not
  # survive a close look: a deck that counts up, a defender who never changes,
  # the same move six times. The chain is sent move by move, alternating the
  # two stub threads so both sides of the conversation are real, and each send
  # opens the extension by TAPPING the newest bubble so the whole run collapses
  # into caption lines over a single bubble (trap 6).
  need_sim
  local name="${1:?chain NAME}" count="${2:-12}" depth="${3:-14}" np="${4:-2}"
  local tool="${FOOLISH_TOOL:-$REPO/c/build/msg_wire_test}"
  [ -x "$tool" ] || { echo "no seeder at $tool - (cd c && make build/msg_wire_test)" >&2; return 1; }
  local g; g=$(group_dir)
  local other="${FOOLISH_OTHER_THREAD:-8583}"

  "$tool" --chain "$np" "$count" "$depth" >/tmp/rig_chain.hex 2>/tmp/rig_chain.log || {
    echo "no chain at depth $depth" >&2; return 1; }
  local last
  last=$(grep -o 'actor=seat [0-9]' /tmp/rig_chain.log | tail -1 | awk '{print $2}')
  # See the FOOLISH_NAMES note in the README: the list is NOT indexed by
  # absolute seat, so this mapping is the inverse of the obvious one.
  if [ "$last" = "0" ]; then export FOOLISH_NAMES="Kate,Alex"
  else export FOOLISH_NAMES="Alex,Kate"; fi
  "$tool" --chain "$np" "$count" "$depth" >/tmp/rig_chain.hex 2>/tmp/rig_chain.log || return 1

  local HEX=() ACT=()
  while IFS= read -r l; do HEX+=("$l"); done < /tmp/rig_chain.hex
  while IFS= read -r l; do ACT+=("$l"); done \
    < <(grep -o 'actor=seat [0-9]' /tmp/rig_chain.log | awk '{print $2}')
  [ "${#HEX[@]}" -gt 0 ] || { echo "chain produced no payloads" >&2; return 1; }
  # We sit in the seat that moves LAST, so the newest bubble is ours and the
  # board under it is the state our own move produced.
  local mine="${ACT[$((${#ACT[@]} - 1))]}"

  # Messages keeps its conversations in memory only, so terminating it is how a
  # shoot starts from an empty transcript rather than on top of the last run.
  xcrun simctl terminate "$SIM" com.apple.MobileSMS >/dev/null 2>&1 || true
  sleep 2
  cmd_stage "${FOOLISH_APPEARANCE:-dark}" >/dev/null 2>&1 \
    || cmd_stage "${FOOLISH_APPEARANCE:-dark}" >/dev/null 2>&1
  cmd_stageseed on >/dev/null

  # A previous, FINISHED game in the same thread, when asked for. Two things
  # come from it. It is what a real thread looks like - people play more than
  # one game - and it is the only way to make the transcript TALLER than its
  # viewport, which is the only way to scroll Apple's unknown-sender banner
  # behind the drawer. Nothing else moves that banner: it is glued under the
  # newest message, and turning off both unknown-sender filtering prefs leaves
  # it exactly where it was.
  local pre="${FOOLISH_CHAIN_PREFACE:-0}" j fool
  for ((j = 0; j < pre; j++)); do
    "$tool" --endgame "$np" >/tmp/rig_pre.hex 2>/tmp/rig_pre.log || break
    fool=$(grep -o 'fool=seat [0-9]' /tmp/rig_pre.log | head -1 | awk '{print $2}')
    # FOOLISH_NAMES is written from the LOCAL seat outwards, so name[0] is us -
    # which means a fixture whose fool IS our seat photographs as us losing.
    # The owner caught exactly that on two earlier game-over frames.
    # An envelope's join list is written from the SEALING player outwards, so
    # slot 0 is always the local player whatever the absolute seat number says.
    # Naming the fool's slot first is therefore what keeps us from photographing
    # ourselves losing - the owner caught exactly that on two earlier game-over
    # frames. Re-sealed with the loser named, not merely reported.
    local prenames="Kate,Alex"
    [ "$fool" = "0" ] || prenames="Alex,Kate"
    FOOLISH_NAMES="$prenames" "$tool" --endgame "$np" >/tmp/rig_pre.hex 2>/dev/null
    tail -1 /tmp/rig_pre.hex > "$g/dev.fatboard"
    printf '%s' "$mine" > "$g/dev.seat"
    cmd_back >/dev/null 2>&1 || true
    cmd_open "$other" >/dev/null 2>&1
    cmd_turn >/dev/null 2>&1 || echo "  preface $j did not send" >&2
  done

  local i thread
  for i in "${!HEX[@]}"; do
    printf '%s' "${HEX[$i]}" > "$g/dev.fatboard"
    printf '%s' "$mine" > "$g/dev.seat"
    thread="$SHOOT_THREAD"; [ "${ACT[$i]}" = "$mine" ] || thread="$other"
    # `back` is what kills the appex, and only a dead appex claims the next
    # seed - see trap 1.
    cmd_back >/dev/null 2>&1 || true
    if [ "$i" = "0" ]; then cmd_open "$thread" >/dev/null 2>&1
    else cmd_tapopen "$thread" >/dev/null 2>&1 || cmd_open "$thread" >/dev/null 2>&1
    fi
    cmd_turn >/dev/null 2>&1 || echo "  move $i did not send" >&2
  done
  # Staging OFF before the last frame. It is what auto-sends each seeded move,
  # and left on it also stages a DRAFT the moment the extension is next opened -
  # a bubble sitting in the compose field over a board that did not produce it.
  cmd_stageseed off >/dev/null
  cmd_back >/dev/null 2>&1 || true
  if [ "${FOOLISH_CHAIN_DRAWER:-1}" = "1" ]; then
    # The collapsed hero frame: the drawer open over the transcript the chain
    # just built, showing the SAME state the newest bubble does. Re-seeded
    # rather than opened by tapping the bubble, because tapping one this device
    # has no identity in opens the seat-claim screen instead of the board.
    printf '%s' "${HEX[$((${#HEX[@]} - 1))]}" > "$g/dev.fatboard"
    printf '%s' "$mine" > "$g/dev.seat"
    cmd_open "$SHOOT_THREAD" >/dev/null 2>&1 && cmd_collapse >/dev/null 2>&1
    sleep 2
    cmd_nudge || true
  else
    cmd_enter "$SHOOT_THREAD" >/dev/null 2>&1 || true
  fi
  cmd_shot "$name"
}
cmd_nudge() {
  # Scroll the transcript until Apple's unknown-sender banner is behind the
  # drawer. It is a simulator artifact - there is no iMessage account, so every
  # thread is unverified - but it lands between the newest bubble and the
  # drawer, right where a collapsed App Store frame is read.
  #
  # It cannot be turned off (both unknown-sender filtering prefs leave it
  # exactly where it was) and it cannot be scrolled away on its own, because it
  # is glued under the newest message and a transcript that FITS does not
  # scroll - it rubber-bands straight back. It moves only when there is more
  # above it than the viewport holds, which is what `FOOLISH_CHAIN_PREFACE`
  # buys. So this verifies rather than assumes: it re-measures after every drag
  # and says so when the transcript would not move.
  need_sim
  read -r W H < <(screen)
  local i sp top
  for i in 1 2 3 4 5 6 7 8 9 10; do
    sp=$(python3 "$LIB/ui.py" spam | awk '{print $2}')
    [ "$sp" = "None" ] && { echo "banner clear"; return 0; }
    top=$(grab_y)
    if [ "$top" != "None" ] && [ "$sp" -ge "$top" ]; then
      echo "banner behind the drawer (banner $sp, drawer $top)"; return 0
    fi
    # Small steps, and stop at the FIRST clear reading. The banner sits
    # directly under the newest bubble, so anything more than the minimum puts
    # that bubble behind the compose bar - which looks like a bug in the app
    # rather than a scrolled transcript.
    swipe 0.30 $((W / 2)) $((H * 22 / 100)) $((W / 2)) $((H * 22 / 100 + 55)) 1.3
  done
  echo "banner still showing at $sp - the transcript is not tall enough to scroll." >&2
  echo "  add history with FOOLISH_CHAIN_PREFACE=1" >&2
  return 1
}
cmd_tapopen() {
  # Open the extension the way a REAL GAME does: by tapping the newest bubble,
  # not through the `+` menu. Both routes show the same seeded board (the
  # `dev.fatboard` claim runs before the selectedMessage payload path), but only
  # this one sets `conversation.selectedMessage` - and a send inherits THAT
  # message's MSSession, which is the single thing that makes Messages collapse
  # the earlier bubble into a caption line. Through `+` every send is its own
  # session, so a chain photographs as a stack of full bubbles. The owner:
  # "it should be a caption from us, a caption from kate, a bubble from us."
  need_sim
  cmd_enter "${1:-$SHOOT_THREAD}" >/dev/null
  local pt; pt=$(python3 "$LIB/ui.py" lastmsg | sed 's/LASTMSG //')
  [ "$pt" = "None" ] && { echo "no bubble to tap - send one first" >&2; return 1; }
  local x y
  x=$(echo "$pt" | tr -d '(),' | awk '{print $1}')
  y=$(echo "$pt" | tr -d '(),' | awk '{print $2}')
  # `lastmsg` reports the tapback/avatar edge; step INWARD to land on the
  # bubble itself - ours sits on the right, theirs on the left.
  read -r W H < <(screen)
  if [ "$x" -gt $((W / 2)) ]; then x=$((x - 60)); else x=$((x + 60)); fi
  tap "$x" "$y" 5
}
cmd_back() {
  need_sim
  front
  read -r W H < <(screen)
  # An EXPANDED drawer covers the back chevron and hides it from the
  # accessibility tree, so put the drawer down first.
  local top; top=$(grab_y)
  if [ "$top" != "None" ] && [ "$top" -lt $((H / 3)) ]; then
    swipe 0.6 $((W / 2)) $(pull_y "$top" "$H") $((W / 2)) $((H * 66 / 100)) 2.5
  fi
  # LEAVING THE THREAD IS THE POINT, not tidiness: it is what kills the appex,
  # and `claimSeededPayload()` is once per appex process - so a `back` that
  # merely collapses the drawer leaves the NEXT open showing the PREVIOUS
  # seed's board, with the new one never read. Verified, not assumed.
  local i=0
  while [ $i -lt 3 ] && in_thread; do
    tap_ax "Messages" 2.5 || break
    i=$((i + 1))
  done
  if in_thread; then
    echo "could not leave the thread - the next seed will not be read" >&2
    return 1
  fi
  cmd_enter "$SHOOT_THREAD" >/dev/null
}

# The grabber belongs to Messages' presentation, not to us, and is in nobody's
# accessibility tree - but it always sits on the drawer's own top edge, which
# lib/ui.py finds by colour. Dragging from THERE works at either detent and on
# any device; dragging from a pinned y worked only at the detent it was
# measured at.
cmd_expand() {
  read -r W H < <(screen)
  local y; y=$(grab_y)
  [ "$y" = "None" ] && { echo "no drawer on screen" >&2; return 1; }
  swipe 0.6 $((W / 2)) $((y + 6)) $((W / 2)) $((H * 16 / 100)) 3
}

# Collapse and return AT ONCE. Everything with a fuse on it - the Send hint,
# a toast - is gone by the time the ordinary 3-second settle returns.
cmd_collapse_fast() {
  read -r W H < <(screen)
  local y; y=$(grab_y)
  [ "$y" = "None" ] && return 1
  swipe 0.45 $((W / 2)) $(pull_y "$y" "$H") $((W / 2)) $((H * 66 / 100)) 0.9
}

# Press the board's own action plank - Good for an attacker whose defender has
# covered everything, Pickup for a defender - and leave it STAGED. The green
# check a player sees on their own side before they send is a state of the
# product and was in none of the first two hundred frames.
cmd_goodtap() {
  read -r W H < <(screen)
  local y; y=$(bar_y -1)
  [ "$y" = "-1" ] && return 1
  tap $((W * 4 / 5)) "$y" 2.5
}

cmd_collapse() {
  read -r W H < <(screen)
  local y; y=$(grab_y)
  [ "$y" = "None" ] && { echo "no drawer on screen" >&2; return 1; }
  swipe 0.6 $((W / 2)) $(pull_y "$y" "$H") $((W / 2)) $((H * 66 / 100)) 3
}

# Dismiss a STAGED Foolish bubble sitting in the compose field. Merely opening
# the extension onto a lobby stages one, and it then rides along in the compose
# field of every later frame as an unsent draft - which is not what a store
# photograph should show. Its close button is inset from the bubble's own
# top-right corner, and the bubble is found by colour, so this holds wherever
# the compose field ends up.
cmd_clearstage() {
  need_sim
  front
  # Messages names the close button ("Remove app from message") and only ever
  # shows it for a bubble in the COMPOSE FIELD, which is exactly the question.
  # There was a colour fallback here and it was worse than nothing: a SENT
  # Foolish bubble sitting in the transcript is the same felt, so the fallback
  # found one and tapped it - opening the bubble instead of clearing a draft.
  if tap_ax "Remove app from message" 2 2>/dev/null; then
    echo "cleared staged bubble"
  else
    echo "nothing staged"
  fi
}

# Wooden buttons are found by COLOUR, never by a hard-coded y: the lobby moves
# with the player count, the locale, the device and the presentation style, and
# a stale constant lands in the Settings gear - which silently switches the
# app's language. `-1` asks for the LOWEST bar on screen.
bar_y() {
  python3 "$LIB/ui.py" bars | python3 -c "
import sys, ast
b = ast.literal_eval(sys.stdin.read().split('BARS ')[1])
print(b[${1:-0}][0] if b else -1)"
}

# A LOBBY with N seats filled, left open. Seats are filled with the DEBUG
# 'Add player (testing)' control, so one device can stand in for a whole
# table - which also means a lobby photographed this way is NEVER shippable as
# a store frame while that button is on screen.
cmd_lobby() {
  local seats="${1:-2}" i=1 y
  cmd_unseed >/dev/null
  cmd_back
  cmd_open
  read -r W H < <(screen)
  y=$(bar_y 0); [ "$y" != "-1" ] && tap $((W / 2)) "$y" 3.5      # New game
  while [ "$i" -lt "$seats" ]; do
    y=$(bar_y 0); [ "$y" = "-1" ] && break
    tap $((W / 2)) "$y" 2.5                                      # Add player
    i=$((i + 1))
  done
  echo "lobby: $i seated; bars at $(python3 "$LIB/ui.py" bars)"
}

# Select the leftmost hand card and play it. Both taps are found by colour, so
# this is the same code on every device and in every locale.
# Play a LEGAL move, and prove one was made.
#
# The rig does not know the rules and does not need to. It knows one thing: a
# move that lands STAGES A BUBBLE, and a staged bubble is named in the
# accessibility tree ("Send"). So candidates are tried and the tree is the
# judge. An illegal one is refused by the board itself ("That move was not
# allowed") and costs nothing but a tap.
#
# Two shapes of move, and the second is why an earlier version reported "no
# legal move" on every defender board it was handed:
#   ATTACK  select a card; a valid attack makes an action plank appear
#           (lib/newbar.py spots it) and pressing it plays the card.
#   COVER   select a card, then tap the attack it covers. There is no button
#           for this at all. A refused attempt also DESELECTS, so the card has
#           to be re-selected before the next attack is tried - which is the
#           bug that made the first cover loop try one card against one attack
#           and then tap dead board for the rest of the sweep.
#
# One card only. Multi-card attacks and greedy full covers exist and are not
# attempted; for a photograph, one card is a move.
cmd_play() {
  need_sim
  local before after newy x y t tx ty
  before=$(python3 "$LIB/ui.py" bars)
  y=$(python3 "$LIB/ui.py" hand_y | awk '{print $2}')
  [ "$y" = "-1" ] && { echo "no hand on screen" >&2; return 1; }
  read -r W H < <(screen)
  local cards table
  cards=$(python3 "$LIB/ui.py" cards | sed 's/CARDS //' | tr -d '[],')
  table=$(python3 "$LIB/ui.py" table | sed 's/TABLE //' | tr -d '[]()' | tr ',' ' ')
  for x in $cards; do
    tap "$x" "$y" 1.2
    after=$(python3 "$LIB/ui.py" bars)
    newy=$(python3 "$LIB/newbar.py" "$before" "$after")
    if [ "$newy" != "-1" ]; then
      tap $((W / 2)) "$newy" 2.5
      if ax "Send" >/dev/null 2>&1; then echo "played: attack"; return 0; fi
    fi
    set -- $table
    while [ $# -ge 2 ]; do
      tx=$1; ty=$2; shift 2
      tap "$x" "$y" 0.8                 # (re)select - a refusal deselects
      tap "$tx" "$ty" 1.5
      if ax "Send" >/dev/null 2>&1; then echo "played: cover"; return 0; fi
    done
  done
  echo "no legal one-card move in hand" >&2
  return 1
}

# SEND what the seeded open staged, so the transcript's last bubble is the
# board on screen. Requires `stageseed on`.
#
# This replaced a live-play attempt that swept the hand against the table
# looking for a legal move. It could be made to work and it was not worth it:
# the sweep needs the exact pixel of every uncovered attack, and a lone attack
# on an otherwise covered table is one card across the whole board - under
# every threshold that excludes the seat badges. Seeding the bubble from the
# same chain the board came from is the same answer with none of the guessing.
# Select the leftmost hand card, so the frame carries the selection border.
cmd_select() {
  need_sim
  local x y
  x=$(python3 "$LIB/ui.py" cards | python3 -c "
import sys, ast
c = ast.literal_eval(sys.stdin.read().split('CARDS ')[1]); print(c[0] if c else -1)")
  y=$(python3 "$LIB/ui.py" hand_y | awk '{print $2}')
  [ "$x" = "-1" ] || [ "$y" = "-1" ] && return 1
  tap "$x" "$y" 2
}

cmd_turn() {
  need_sim
  local top; top=$(grab_y)
  read -r W H < <(screen)
  if [ "$top" != "None" ] && [ "$top" -lt $((H / 3)) ]; then
    swipe 0.6 $((W / 2)) $(pull_y "$top" "$H") $((W / 2)) $((H * 66 / 100)) 3
  fi
  tap_ax "Send" 4 || { echo "nothing staged - is stageseed on?" >&2; return 1; }
}

# --------------------------------------------------------------- state ----

cmd_seed()   { need_sim; python3 "$LIB/seed.py" "$@"; }
cmd_unseed() { local g; g=$(group_dir); rm -f "$g/dev.fatboard" "$g/dev.seat" "$g/dev.replay"; echo "seed removed"; }

# THE PREFERENCES PLIST IS NOT THE STORE OF RECORD; the simulator's cfprefsd
# is. Writing .../Library/Preferences/<domain>.plist by hand does NOTHING that
# reaches the app: cfprefsd holds its own copy, serves that to every launch,
# and overwrites the file from it. A hand-written file looks correct on disk
# and on `plutil -p` and never reaches the app - which is why a run that asked
# for wool photographed felt. Verify a pref by setting it to something it is
# NOT, never by re-asserting what it has.
cmd_prefs() {
  need_sim
  local table="${1:-}" lang="${2:-}" appear="${3:-}"
  local d=(xcrun simctl spawn "$SIM" defaults)
  [ -n "$table" ]  && "${d[@]}" write "$EXT_DOM" 'ios.table.surface' -string "$table"
  [ -n "$lang" ]   && "${d[@]}" write "$EXT_DOM" 'ios.language' -string "$lang"
  [ -n "$appear" ] && xcrun simctl ui "$SIM" appearance "$appear" >/dev/null
  echo "prefs: table=${table:-unchanged} lang=${lang:-unchanged} appearance=${appear:-unchanged}"
}

cmd_slowmo() {
  local g; g=$(group_dir)
  if [ "${1:-0}" = "0" ]; then rm -f "$g/dev.slowmo"; echo "slowmo off"
  else printf '%s' "$1" > "$g/dev.slowmo"; echo "slowmo x$1"; fi
}

cmd_ruler() {
  local g; g=$(group_dir)
  if [ "${1:-on}" = "off" ]; then rm -f "$g/dev.ruler"; echo "ruler off"
  else : > "$g/dev.ruler"; echo "ruler on"; fi
}

# `dev.stage`: make a seeded open ALSO stage its own chain as a bubble, so a
# frame's last bubble is the board underneath it. On for every gameplay frame
# whose transcript is visible.
cmd_stageseed() {
  local g; g=$(group_dir)
  if [ "${1:-on}" = "off" ]; then rm -f "$g/dev.stage"; echo "stageseed off"
  else : > "$g/dev.stage"; echo "stageseed on"; fi
}

# ------------------------------------------------------------- capture ----

# One frame. `simctl io screenshot` writes RGBA even though the image is fully
# opaque, and the App Store uploader rejects an alpha channel - so every frame
# is flattened on the way out and its size is asserted, not assumed.
cmd_shot() {
  need_sim
  local name="${1:?shot NAME}"
  mkdir -p "$OUT/$(dirname "$name")"
  local p="$OUT/$name.png"
  xcrun simctl io "$SIM" screenshot "$p" >/dev/null 2>&1
  python3 - "$p" <<'PY'
import sys
from PIL import Image
p = sys.argv[1]
im = Image.open(p)
if im.mode != "RGB":
    im.convert("RGB").save(p)
print(f"{p}  {im.width}x{im.height}")
PY
}

# name|mode|args|seat|table|lang|appearance|view|act
#
# A blank `mode` means "do not seed" - for lobby, settings and rules frames,
# which are reached by tapping rather than by opening a canned chain.
#
# `act` is `turn` (send the staged bubble), `select` (send, then select a
# hand card so the selection border shows) or `hint` (leave it staged and
# collapse, which is what puts the Send hint on screen). `turn` is the one that
# is what makes the transcript honest: the last bubble is then the board that
# is on screen, rather than a bubble left over from some other game. Without
# it the drawer and the chat behind it are two unrelated games, which is what
# "impossible sequences" means. A frame whose `turn` fails is SKIPPED rather
# than shot, because the alternative is a frame that lies quietly.
cmd_batch() {
  need_sim
  local list="${1:?batch LIST}" name mode args seat table lang appear view act
  while IFS='|' read -r name mode args seat table lang appear view act; do
    [ -z "${name:-}" ] && continue
    case "$name" in \#*) continue ;; esac
    echo "=== $name (${mode:-noseed} ${args:-} seat=${seat:-def} ${table:-} ${lang:-} ${appear:-} ${view:-compact} ${act:-})"
    if [ -n "${mode:-}" ]; then
      SEAT="${seat:-}" cmd_seed $mode $args | tail -1
    fi
    cmd_prefs "${table:-}" "${lang:-}" "${appear:-}" >/dev/null
    # One bad frame must not end the run. `set -e` applies inside this loop, so
    # an un-guarded failure here killed a 41-shot batch after its FIRST line and
    # still exited 0 - the list simply stopped, with nothing to say it had.
    cmd_back || { echo "!! $name skipped - could not leave the drawer" >&2; continue; }
    cmd_open_retry || { echo "!! $name skipped - could not open the extension" >&2; continue; }
    case "${act:-}" in
      # Send the bubble the seeded open staged: the transcript's last bubble
      # is then the board on screen.
      turn)   cmd_turn || { echo "!! $name skipped - nothing to send" >&2; continue; } ;;
      # Send it, then SELECT a hand card - the bright red selection border is
      # a whole state of the product and was in none of the first 100 frames.
      select) cmd_turn || { echo "!! $name skipped - nothing to send" >&2; continue; }
              cmd_select || { echo "!! $name skipped - no hand to select from" >&2; continue; } ;;
      # Leave it STAGED and collapse, which is what puts the Send hint on
      # screen. Coherent on its own terms: the move is made and not yet sent,
      # and the bubble in the field is the board underneath it.
      # The hint burns a 3-SECOND FUSE (StagedSendHint) and then fades, so the
      # frame has to be taken inside it. `cmd_collapse` settles for 3s, which
      # is exactly too long - those frames came back with a staged bubble and
      # no arrow over it. Collapse fast, shoot at once, verify nothing.
      hint)   cmd_collapse_fast || { echo "!! $name skipped - no drawer" >&2; continue; } ;;
      # Tap the board's own action plank (Good / Pickup) and do NOT send, so
      # the frame carries the move staged and the player's own green check.
      good)   cmd_goodtap || { echo "!! $name skipped - no action plank" >&2; continue; } ;;
    esac
    [ "${view:-compact}" = "expanded" ] && cmd_expand
    cmd_shot "$name"
  done < "$list"
}

# Screenshots as fast as they come - each costs ~0.4s, so a 40-shot burst
# covers ~16s of animation and catches in-flight frames a settled screenshot
# never shows. The owner's verdict on this technique: "probably the most
# useful thing you have come up with for QA testing".
cmd_burst() {
  need_sim
  local label="${1:?burst LABEL N}" n="${2:-40}" i=1
  mkdir -p "$OUT/burst/$label"
  while [ "$i" -le "$n" ]; do
    xcrun simctl io "$SIM" screenshot "$OUT/burst/$label/$(printf '%04d' $i).png" >/dev/null 2>&1
    i=$((i + 1))
  done
  echo "$OUT/burst/$label  ($n frames)"
}

# Record a take and keep EVERY COMPOSITED FRAME.
#
# `simctl io recordVideo` captures at the display's own 60Hz and writes a
# VARIABLE-rate movie: one frame per frame the device actually composited, and
# none at all while the screen is still. Extracting with `-vf fps=30` RESAMPLES
# that - duplicating the still frames and throwing away half of every
# animation. `-fps_mode passthrough` keeps them 1:1; their presentation times
# go beside them in times.txt, one line per frame, so a frame is PLACED IN TIME
# rather than assumed to be 1/60 after the one before it. On a healthy take the
# dominant inter-frame delta is 0.0167s; anything outside 12-22ms is a beat the
# device dropped, and `sheet` flags those cells.
cmd_film() {
  need_sim
  local name="${1:-film}" secs="${2:-15}"; shift 2 || true
  local d="$OUT/film/$name"
  rm -rf "$d"; mkdir -p "$d"
  xcrun simctl io "$SIM" recordVideo --codec h264 --force "$d/take.mp4" >/dev/null 2>&1 &
  local rec=$!
  sleep 3
  "$@"
  sleep "$secs"
  kill -INT $rec 2>/dev/null || true; sleep 4
  ffmpeg -v error -i "$d/take.mp4" -fps_mode passthrough "$d/f%05d.png" 2>"$d/ffmpeg.err" || {
    cat "$d/ffmpeg.err" >&2; return 1; }
  # The image2 muxer complains "non monotonically increasing dts" once per
  # repeated timestamp in a variable-rate source. It writes numbered PNGs,
  # which carry no timestamps at all, so it is noise about nothing - but it is
  # the ONLY expected noise, so everything else still reaches the terminal.
  grep -v 'non monotonically increasing dts\|Last message repeated' "$d/ffmpeg.err" >&2 || true
  ffprobe -v error -select_streams v:0 -show_entries frame=pts_time -of csv=p=0 \
          "$d/take.mp4" | tr -d ',' > "$d/times.txt"
  echo "$d  ($(ls "$d" | grep -c '^f') frames)"
}

cmd_sheet() { python3 "$LIB/sheet.py" "$@"; }

# The extension's own diagnostics, which live in the App Group beside the dev
# flags. `flight` is the always-compiled FlightRecorder (on a device it is
# reached by HOLDING THE GEAR for 5 seconds); `mem` is the memory probe, whose
# .prev is the run before the one that crashed.
cmd_flight() { local g; g=$(group_dir); cat "$g/flight.txt" 2>/dev/null || echo "no flight log yet"; }
cmd_mem()    { local g; g=$(group_dir); cat "$g/memprobe.txt" 2>/dev/null || echo "no memory probe yet"
               [ -f "$g/memprobe.prev.txt" ] && { echo "--- previous run ---"; cat "$g/memprobe.prev.txt"; } || true; }

# Stream the animation subsystem's log out of the simulator. AnimLog is DEBUG
# plus an env var, so this says nothing on a shipped build - which is exactly
# how three release builds came to be diagnosed by inference.
cmd_log() {
  need_sim
  xcrun simctl spawn "$SIM" log stream --style compact \
    --predicate 'subsystem BEGINSWITH "cards.foolish"'
}

cmd_probe() {
  need_sim
  read -r W H < <(screen); echo "screen ${W}x${H}pt"
  echo "in a thread: $(in_thread && echo yes || echo no)"
  for l in Messages add Message Send Foolish; do
    printf '  %-10s %s\n' "$l" "$(ax "$l" 2>/dev/null || echo '-')"
  done
  python3 "$LIB/ui.py" all
}

case "${1:-}" in
  doctor)   shift; cmd_doctor "$@" ;;
  newsim)   shift; cmd_newsim "$@" ;;
  build)    shift; cmd_build "$@" ;;
  stage)    shift; cmd_stage "$@" ;;
  nickname) shift; cmd_nickname "$@" ;;
  setname)  shift; cmd_setname "$@" ;;
  session)  shift; cmd_session "$@" ;;
  enter)    shift; cmd_enter "$@" ;;
  open)     shift; cmd_open "$@" ;;
  tapopen)  shift; cmd_tapopen "$@" ;;
  nudge)    shift; cmd_nudge "$@" ;;
  chain)    shift; cmd_chain "$@" ;;
  back)     shift; cmd_back "$@" ;;
  expand)   shift; cmd_expand "$@" ;;
  collapse) shift; cmd_collapse "$@" ;;
  goodtap)  shift; cmd_goodtap "$@" ;;
  seed)     shift; cmd_seed "$@" ;;
  unseed)   shift; cmd_unseed "$@" ;;
  prefs)    shift; cmd_prefs "$@" ;;
  slowmo)   shift; cmd_slowmo "$@" ;;
  ruler)    shift; cmd_ruler "$@" ;;
  stageseed) shift; cmd_stageseed "$@" ;;
  shot)     shift; cmd_shot "$@" ;;
  batch)    shift; cmd_batch "$@" ;;
  burst)    shift; cmd_burst "$@" ;;
  film)     shift; cmd_film "$@" ;;
  sheet)    shift; cmd_sheet "$@" ;;
  probe)    shift; cmd_probe "$@" ;;
  flight)   shift; cmd_flight "$@" ;;
  mem)      shift; cmd_mem "$@" ;;
  log)      shift; cmd_log "$@" ;;
  lobby)    shift; cmd_lobby "$@" ;;
  clearstage) shift; cmd_clearstage "$@" ;;
  play)     shift; cmd_play "$@" ;;
  turn)     shift; cmd_turn "$@" ;;
  select)   shift; cmd_select "$@" ;;
  tap)      shift; tap "$@" ;;
  swipe)    shift; swipe "$@" ;;
  text)     shift; type_s "$@" ;;
  group)    group_dir ;;
  *) sed -n '2,60p' "$0"; exit 1 ;;
esac
