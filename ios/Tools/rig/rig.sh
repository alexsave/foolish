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
#   rig.sh openbubble             tap the newest Foolish bubble in the chat
#   rig.sh chain NAME [n] [depth] a transcript of REAL consecutive moves:
#                                 caption / caption / caption / one bubble
#   rig.sh tapopen [thread]       open it by TAPPING the newest bubble, so the
#                                 next send shares that message's MSSession
#   rig.sh clearstage             dismiss a staged bubble left in the compose
#                                 field (then `back`, so the tap it just made
#                                 does not leave the drawer 16pt short - trap 11)
#   rig.sh expand / collapse      drag the grabber
#   rig.sh back                   leave the drawer, and re-enter the thread
#   rig.sh leave                  leave the thread and STOP
#   rig.sh killappex              end the appex directly, which is what makes
#                                 the next seed readable - no navigation at all
#
#   ---- state ----------------------------------------------------------
#   rig.sh lobby N                a LOBBY with N seats filled (DEBUG button)
#   rig.sh lobbytap WHICH         start | leave | join | box (the rules checkbox)
#   rig.sh play                   select a LEGAL card and press the plank
#   rig.sh turn                   play a move AND send it, so the transcript's
#                                 last bubble is the board now on screen
#   rig.sh seed MODE ARGS...      dev.fatboard via c/build/msg_wire_test
#                                 fatboard <cards> <players> [nopass] [preroll]
#                                 endgame <players> [nopass]
#                                 lastdefense <players> | twocover <players>
#   rig.sh unseed                 back to the normal create/join flow
#   rig.sh claimed                the seed the extension actually opened onto
#                                 (dev.claimed) - a stale one is trap 10
#   rig.sh prefs [TABLE] [LANG] [APPEARANCE]      felt|wool  en|ru|..  light|dark
#   rig.sh slowmo N | ruler [off]                 debug overlays
#   rig.sh deal N | off                           pin the genesis deal (dev.seed)
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

# A TAP WITH A DURATION, because an instantaneous one is not always a tap.
#
# `idb ui tap` with no duration injects a touch down and up in the same instant,
# and some UIKit controls never see it. Messages' compose "+" is one: a bare tap
# on its exact centre does nothing at all - no menu, no state change, nothing in
# the tree - while the identical coordinates with `--duration 0.12` open the app
# menu every time. It had looked like the + was "missing" or the keyboard was
# eating the tap; it was neither, and both theories cost a while to rule out.
# 0.12s is comfortably above the threshold and far below a long-press.
tap()   { need_sim; "$IDB" ui tap --udid "$SIM" --duration 0.12 "$1" "$2" >/dev/null 2>&1; sleep "${3:-1}"; }
swipe() { need_sim; "$IDB" ui swipe --udid "$SIM" --duration "$1" "$2" "$3" "$4" "$5" >/dev/null 2>&1; sleep "${6:-1}"; }
type_s(){ need_sim; "$IDB" ui text --udid "$SIM" "$1" >/dev/null 2>&1; sleep "${2:-1}"; }

# The screen in POINTS, from the accessibility tree's root - the one thing
# that reports points on every device without a lookup table.
# Cached per simulator: a device's point size cannot change inside a run, and
# this was being answered by a full `describe-all` 28 times in a single chain.
# Keyed on the UDID and kept in FOOLISH_WORK, so it survives the one-command-
# per-process shape the rig is driven with. `rig.sh probe` re-reads it.
SCRCACHE="${FOOLISH_WORK:-/tmp/foolishrig}/screen.$SIM"
screen() {
  need_sim
  [ -s "$SCRCACHE" ] && { cat "$SCRCACHE"; return; }
  mkdir -p "$(dirname "$SCRCACHE")"
  python3 "$LIB/ax.py" screen | tee "$SCRCACHE"
}

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
  # `ax.py screen` exits non-zero on an EMPTY tree, which is precisely
  # "Messages has not answered yet" - so that is the predicate, not two seconds.
  poll 16 0.2 python3 "$LIB/ax.py" screen || true
}

in_thread() { ax "add" >/dev/null 2>&1; }

# WAIT FOR A PREDICATE, not for a guess.
#
# Every fixed sleep in this file is somebody's estimate of the worst case of an
# animation, and it is paid in full on every run whether or not the animation
# took that long. Where the rig already has a way to ASK whether the thing
# happened - `here_is`, `in_thread`, the drawer's own top edge, the claim
# receipt - polling that is both faster and MORE correct: a slow machine gets
# more time rather than a wrong frame, and a fast one stops waiting.
#
# The sleeps that stay are the ones with no predicate behind them: a fuse the
# product burns (StagedSendHint's 3s), and the settle after a drag that only
# the owner's eye can judge.
#   poll <tries> <interval> <cmd...>
poll() {
  local tries="$1" iv="$2"; shift 2
  local i=0
  while [ "$i" -lt "$tries" ]; do
    "$@" >/dev/null 2>&1 && return 0
    sleep "$iv"; i=$((i + 1))
  done
  return 1
}
# Our own surface has ARRIVED - not merely begun to appear.
#
# `grab_y` finds our felt the moment the presentation starts sliding, which is
# several hundred milliseconds before the board is where it will end up. That
# distinction did not exist while the opens ended in a flat `sleep 7`, and the
# first thing it broke was `cmd_play`: it reads the hand and the table off a
# SCREENSHOT (`ui.py hand_y` / `cards` / `table`), so an early return had it
# measuring a moving target and tapping where a card no longer was.
# Two consecutive equal readings is the cheap, honest test for "stopped".
# KILL THE APPEX, which is the only thing leaving the thread was ever for.
#
# `claimSeededPayload()` is once per appex PROCESS, and the rig's whole
# leave-and-come-back dance existed to end that process, because leaving the
# thread is what ends it. But the appex is an ORDINARY HOST PROCESS - the
# simulator runs it on this Mac - so it can just be killed: instant, verifiable,
# and it needs no navigation, no row probe and no re-entry.
#
# Measured, same seed, same device:
#   kill  + open   7.4s, claimed
#   leave + open  20.3s, and it did not always claim
#
# SCOPED TO THIS SIMULATOR by the device UDID in the process path. Rule 5 says
# one simulator per task; a bare `pkill -f FoolishMessages` would reach across
# to another agent's device and kill its appex mid-frame.
# NB the `|| true`. Under `set -o pipefail` a `pgrep` that matches nothing makes
# the whole pipeline return 1, so "the appex is already dead" - the ordinary
# case, and a SUCCESS - came back as a failure. `kill_appex` propagated it, and
# `seed_open`'s `|| continue` then skipped the seed and the open entirely: three
# silent retries that never tried anything, reported as "the extension never
# claimed its seed".
appex_pid()  { pgrep -f "Devices/$SIM/.*FoolishMessages\.appex" 2>/dev/null | head -1 || true; }
appex_gone() { [ -z "$(appex_pid)" ]; }
kill_appex() {
  local p; p=$(appex_pid || true)
  [ -z "$p" ] && return 0
  kill "$p" 2>/dev/null || true
  poll 20 0.1 appex_gone && return 0
  kill -9 "$p" 2>/dev/null || true
  poll 20 0.1 appex_gone
}

drawer_up()     { [ "$(grab_y)" != "None" ]; }
# Two grabs, taken together. Carrying the previous CALL's reading instead halves
# the screenshots and was tried: it cost the keeper 4.9s -> 26.5s, because
# readings half a second apart straddle more of the presentation than a pair
# taken back to back, so "settled" kept coming back false. Cheaper per attempt,
# many more attempts. Left as it was.
settle_reset()  { :; }
drawer_settled() {
  local a b
  a=$(grab_y); [ "$a" = "None" ] && return 1
  b=$(grab_y); [ "$a" = "$b" ]
}
not_in_thread() { ! in_thread; }
# THE SEND HAS GONE THROUGH. Messages only shows a Send button while the compose
# field holds something, so its DISAPPEARANCE is the completion signal - which
# is what the flat 4s after every Send was standing in for, five times a run.
sent_done()  { ! ax "Send" >/dev/null 2>&1; }
# Something is typed: Messages offers Send only once there is text.
has_send()   { ax "Send" >/dev/null 2>&1; }

# In a thread whose header matches `$1` (empty = any thread). The header is a
# Button carrying the remote address, which is the only stable way to tell the
# two stub conversations apart - the LIST reorders by recency, so "row 1" is
# not a thread, it is a coin flip.
here_is() {
  # ONE tree, not two. This used to call `in_thread` (a describe-all) and then
  # `ax.py dump` (another describe-all) against the same unchanged screen -
  # and `here_is` is the single most-called predicate in the rig.
  python3 "$LIB/ax.py" here "${1:-}"
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
  # RIG_RESEED is OPT-IN and off by default, so the ordinary build is the
  # ordinary build. With it, a LIVE appex re-reads `dev.fatboard` when the file
  # changes, which lets a driver skip the leave/probe/re-open cycle that exists
  # only because `claimSeededPayload()` is once per process. Two gates, not one:
  # this compile-time flag, and the `dev.reseed` file at runtime.
  local cond="DEBUG"
  [ -n "${FOOLISH_RESEED:-}" ] && cond="DEBUG RIG_RESEED"
  xcodebuild -project "$REPO/ios/Foolish.xcodeproj" -scheme FoolishMessagesApp \
    -configuration Debug -destination "platform=iOS Simulator,id=$SIM" \
    -derivedDataPath "$DD" SWIFT_ACTIVE_COMPILATION_CONDITIONS="$cond" build | tail -3
  # Install OVER the old build. `simctl uninstall` destroys the App Group and
  # the appex's Preferences container, and both come back with fresh UUIDs.
  xcrun simctl install "$SIM" "$DD/Build/Products/Debug-iphonesimulator/FoolishMessagesApp.app"
  echo "installed on $SIM"
}

# The stage: a clean status bar, an appearance, and Apple's first-run sheets
# gone. Three of them ("Shared with You", "Apple Intelligence in Messages",
# and iOS 26's "Check In")
# appear on a fresh device and each one silently eats the first tap of a run.
cmd_stage() {
  need_sim
  local appear="${1:-dark}"
  xcrun simctl status_bar "$SIM" override --time "9:41" --batteryState charged \
      --batteryLevel 100 --cellularBars 4 --wifiBars 3 --dataNetwork wifi
  xcrun simctl ui "$SIM" appearance "$appear" >/dev/null
  # Poll for Messages rather than guessing six seconds at its launch.
  xcrun simctl launch "$SIM" com.apple.MobileSMS >/dev/null
  poll 40 0.15 python3 "$LIB/ax.py" screen || true
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
    # "Not Now" FIRST, and that order is load-bearing. iOS 26 adds a Check In
    # sheet whose two buttons are "Continue" and "Not Now" - and there
    # "Continue" does not dismiss anything, it walks into Check In's setup and
    # leaves a modal that swallows every later tap. A whole 65-shot batch sat
    # on it for ten minutes reporting "no 'Foolish' on screen", because the
    # sheet had eaten the tap on the compose +. Declining is the safe answer to
    # any first-run sheet during a shoot; "OK"/"Continue" stay for the sheets
    # that only offer those (Shared with You, Apple Intelligence).
    if tap_ax "Not Now" 3 2>/dev/null || tap_ax "OK" 3 2>/dev/null \
       || tap_ax "Continue" 3 2>/dev/null; then
      quiet=0
    else
      quiet=$((quiet + 1))
      # 0.6s, not 2s. Four quiet passes at two seconds is eight seconds spent
      # proving a sheet is absent, on every run, and on an already-staged device
      # none is ever coming. The twelve-iteration budget is unchanged, so a
      # sheet that appears late is still caught - it is the cost of being WRONG
      # about one that never appears that drops.
      sleep 0.6
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
  while [ $i -lt 3 ] && in_thread; do
    tap_ax "Messages" 0.3 || break
    poll 16 0.25 not_in_thread || true
    i=$((i + 1))
  done
  # TRY THE ROW THIS THREAD WAS LAST FOUND ON, FIRST.
  #
  # The list orders by RECENCY, so the conversation just sent to is row 1 - and
  # a rig that always probes row 1 first therefore opens the WRONG thread on
  # roughly every other call, sees the wrong header, and backs out. That is the
  # "opens Kate Bell and closes it immediately" the owner watched it do twice in
  # one run, and it costs a tap, a poll and a back-out each time.
  # The rig already knows the answer: it found this thread somewhere last time.
  local ycache="${FOOLISH_WORK:-/tmp/foolishrig}/rowy.$SIM.${want:-any}"
  local yfirst=""; [ -s "$ycache" ] && yfirst=$(cat "$ycache")
  local y
  for y in $yfirst $((H * 24 / 100)) $((H * 20 / 100)) $((H * 15 / 100)) $((H * 28 / 100)) $((H * 32 / 100)); do
    # Poll for A THREAD, then decide ONCE which one it is.
    #
    # Polling `here_is` directly reads well and is a trap: when the row under
    # this y is the WRONG conversation the predicate can never come true, so
    # every miss burns the entire budget before the next candidate is tried -
    # which made `session` (it alternates threads, so it misses constantly)
    # SLOWER than the fixed 2.5s sleep it replaced. `in_thread` is the part
    # that is actually pending; the identity is settled the moment it lands.
    tap $((W / 2)) "$y" 0.3
    poll 10 0.2 in_thread || true
    if here_is "$want"; then
      mkdir -p "$(dirname "$ycache")"; printf '%s' "$y" > "$ycache"
      return 0
    fi
    in_thread && { tap_ax "Messages" 0.3 && poll 16 0.25 not_in_thread || true; }
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
  # MEASURED, NOT ASSUMED, and two of the three obvious predicates are wrong:
  #   `ax "return"` is not in the tree at all, even with the field focused, so
  #   "wait for a keyboard" never came true and burned its whole budget;
  #   and `Send` is shown whenever ANYTHING is sendable, including a staged
  #   Foolish bubble, so "wait for Send to go away" is not "the text went".
  # Only the middle one survives: Messages offers Send once there is text.
  tap_ax "Message" 0.6 || return 1
  type_s "$2" 0.2
  poll 20 0.2 has_send || true
  # Sound HERE specifically: `session` runs before any seeding, so the only
  # thing Messages can have to send is the text just typed.
  tap_ax "Send" 0.3 || return 1
  poll 40 0.2 sent_done || true
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
  # THE FIRST "+" MAY BE EATEN BY THE KEYBOARD.
  #
  # `session` types into the compose field and leaves a keyboard up, and while
  # one is up the first tap anywhere dismisses it instead of doing what it was
  # aimed at - so the menu never opens and `Foolish` is looked for on a screen
  # that has no menu on it. Leaving the thread used to dismiss the keyboard as a
  # side effect, which is the only reason this never showed before the appex
  # started being killed in place rather than walked away from.
  #
  # So ask whether the menu actually came, and tap again if it did not. The menu
  # is up when Messages puts its dismissal target on screen - NOT when "Foolish"
  # is visible, which is below the fold on a stock device and only the swipe
  # loop underneath can reach.
  local m=0
  while [ $m -lt 3 ]; do
    tap_ax "add" 0.3
    poll 12 0.25 ax "dismiss popup" && break
    m=$((m + 1))
  done
  read -r W H < <(screen)
  local i=0
  while [ $i -lt 5 ]; do
    if tap_ax "Foolish" 0.4; then
      # Seven seconds was an estimate of a cold appex launch. The drawer's own
      # top edge says when it really happened, and `seed_open` polls the claim
      # receipt after this, so a slow open is absorbed rather than mis-read.
      settle_reset; poll 30 0.2 drawer_settled || true
      return 0
    fi
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
  # FIVE ENTRIES MINIMUM, and it is not a style preference - see trap 12.
  # Messages draws only the LAST THREE messages of a session as caption lines,
  # and the simulator corrupts the summary of the first two in the session:
  # they come out carrying the NEWEST message's text. At four entries the
  # corrupted index 1 sits inside the three-line window and the frame shows one
  # caption twice - the same card played twice, which is an impossible
  # transcript. At five the corrupted pair falls outside the window.
  # Deterministic, not a flake: the four-entry chain reproduces the identical
  # wrong caption every run.
  # TWO PLAYERS, for anything whose TRANSCRIPT is in frame. A collapsed drawer
  # shows the thread behind it, and a thread between two people cannot contain a
  # six-seat board - the owner caught exactly that ("an eight player game in a
  # one-on-one chat, that's not possible"). Expanded frames may be any seat
  # count, because the drawer covers the transcript entirely.
  if [ "$np" -ne 2 ] && [ -z "${FOOLISH_CHAIN_MULTI:-}" ]; then
    echo "chain: a collapsed frame shows the thread behind it, and a 1:1 thread" >&2
    echo "  cannot hold a ${np}-player board. Use np=2 (trap 12)." >&2
    echo "  FOOLISH_CHAIN_MULTI=1 to override." >&2
    return 1
  fi
  if [ "$count" -lt 5 ] && [ -z "${FOOLISH_CHAIN_SHORT:-}" ]; then
    echo "chain: $count entries photographs a DUPLICATED caption - 5 is the" >&2
    echo "  minimum that keeps the simulator's corrupted pair out of the" >&2
    echo "  three-line window (trap 12). FOOLISH_CHAIN_SHORT=1 to override." >&2
    return 1
  fi
  local tool="${FOOLISH_TOOL:-$REPO/c/build/msg_wire_test}"
  [ -x "$tool" ] || { echo "no seeder at $tool - (cd c && make build/msg_wire_test)" >&2; return 1; }
  # The App Group container, checked once here so a missing install says so
  # before a chain is played. Every seed below goes through `seed_open`, which
  # resolves it again for itself.
  group_dir >/dev/null
  local other="${FOOLISH_OTHER_THREAD:-8583}"

  "$tool" --chain "$np" "$count" "$depth" >/tmp/rig_chain.hex 2>/tmp/rig_chain.log || {
    echo "no chain at depth $depth" >&2; return 1; }
  # TWO SEATS, NAMED ONCE, so the frame cannot disagree with itself.
  #
  # `sender` is the seat whose moves are sent FROM the photographed thread, and
  # a move sent from that thread lands on its LEFT (see the routing note in the
  # loop) - so `sender` is the OPPONENT. It is the seat that moves LAST, which
  # is what keeps the newest bubble a full board rather than a bare line.
  #
  # `mine` is the other chair: the moves that reach this thread from the other
  # one, which land on the RIGHT where a reader expects their own messages. It
  # is the seat the drawer is seeded at and the seat "Alex" is given, and both
  # come from this one variable so they cannot drift apart - a frame whose
  # right-hand caption and drawer disagree about who we are is exactly the
  # defect the owner caught twice.
  local sender mine
  sender=$(grep -o 'actor=seat [0-9]' /tmp/rig_chain.log | tail -1 | awk '{print $2}')
  [ -n "${FOOLISH_SENDER:-}" ] && sender="$FOOLISH_SENDER"
  mine=$(( (sender + 1) % np ))
  [ -n "${FOOLISH_MINE:-}" ] && mine="$FOOLISH_MINE"
  # `fixture_name` returns `slots[seat]` (c/tests/msg_wire_test.c), so the list
  # IS indexed by absolute seat.
  local nm=() k
  for ((k = 0; k < np; k++)); do nm+=("Kate"); done
  nm[$mine]="Alex"
  if [ -n "${FOOLISH_NAMES_FORCE:-}" ]; then export FOOLISH_NAMES="$FOOLISH_NAMES_FORCE"
  else export FOOLISH_NAMES="$(IFS=,; echo "${nm[*]}")"; fi
  "$tool" --chain "$np" "$count" "$depth" >/tmp/rig_chain.hex 2>/tmp/rig_chain.log || return 1

  local HEX=() ACT=()
  while IFS= read -r l; do HEX+=("$l"); done < /tmp/rig_chain.hex
  while IFS= read -r l; do ACT+=("$l"); done \
    < <(grep -o 'actor=seat [0-9]' /tmp/rig_chain.log | awk '{print $2}')
  [ "${#HEX[@]}" -gt 0 ] || { echo "chain produced no payloads" >&2; return 1; }

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
    # NAME THE FOOL'S SEAT "Kate", so the preface is not a photograph of us
    # losing - the owner caught exactly that on two earlier game-over frames.
    # Re-sealed with the loser named, not merely reported.
    #
    # FOOLISH_NAMES is indexed by ABSOLUTE SEAT, the same as the loop's own
    # naming 40 lines up: `fixture_name(seat)` returns `slots[seat]` and
    # `env_init` writes `joins[i].seat = i` with that name
    # (c/tests/msg_wire_test.c). A note here used to claim the opposite - that a
    # join list runs from the sealing player outwards, so slot 0 is always the
    # local player - and it was simply wrong; decoding a sealed chain entry
    # shows joins[0].seat == 0 carrying slots[0]'s name on every bubble. Two
    # contradictory rules for one list in one function is how a shoot loses a
    # day, so: absolute seat, everywhere.
    local prenames="Kate,Alex"
    [ "$fool" = "0" ] || prenames="Alex,Kate"
    FOOLISH_NAMES="$prenames" "$tool" --endgame "$np" >/tmp/rig_pre.hex 2>/dev/null
    local prehex; prehex=$(tail -1 /tmp/rig_pre.hex | tr -d '[:space:]')
    seed_open "$prehex" "$mine" "$other" open \
      || { echo "  preface $j: the extension never claimed its seed" >&2; return 1; }
    send_staged "$prehex" >/dev/null 2>&1 || echo "  preface $j did not send" >&2
  done

  local i thread route
  for i in "${!HEX[@]}"; do
    # A move sent FROM the thread we photograph lands on its LEFT, and the copy
    # that reaches it from the other thread lands on its RIGHT. That is the
    # inverse of what a note here used to claim, and three shoots were read
    # against the wrong version. What settles it is a pair of runs differing in
    # ONE variable: seat 0 routed to the photographed thread landed left, and
    # then seat 1 routed to the photographed thread landed left. (Photographing
    # 8583 instead of 888 with the routing swapped to match moved nothing -
    # that swaps both halves at once and so tests nothing.) The simulator has no
    # iMessage service: sending to a stub number loops the message back into
    # that thread as though it had been RECEIVED.
    #
    # So `sender` - the seat whose moves go out from the photographed thread -
    # is the seat that appears on the LEFT, which is the OPPONENT. We sit in the
    # other chair. Both facts come from one variable below, so they cannot drift
    # apart again.
    thread="$SHOOT_THREAD"; [ "${ACT[$i]}" = "$sender" ] || thread="$other"
    # The first send has no bubble to tap yet; every later one opens the way a
    # real game does (trap 6). `seed_open` is what makes the seed STICK: only a
    # dead appex claims the next one, and it checks rather than hopes - see its
    # note, and trap 10.
    route=tapopen; [ "$i" = "0" ] && route=open
    seed_open "${HEX[$i]}" "$mine" "$thread" "$route" || {
      echo "  move $i: the extension never claimed its seed - stopping the chain" >&2
      echo "  (every bubble from here would be one move behind; see trap 10)" >&2
      return 1
    }
    send_staged "${HEX[$i]}" >/dev/null 2>&1 || {
      echo "  move $i: the field never took it - stopping the chain" >&2
      return 1
    }
  done
  # Staging OFF before the last frame. It is what auto-sends each seeded move,
  # and left on it also stages a DRAFT the moment the extension is next opened -
  # a bubble sitting in the compose field over a board that did not produce it.
  cmd_stageseed off >/dev/null
  if [ "${FOOLISH_CHAIN_DRAWER:-1}" = "1" ]; then
    # The collapsed hero frame: the drawer open over the transcript the chain
    # just built, showing the SAME state the newest bubble does. Re-seeded
    # rather than opened by tapping the bubble, because tapping one this device
    # has no identity in opens the seat-claim screen instead of the board.
    #
    # Through `seed_open` like every other seed here: this is the ONE frame that
    # gets photographed, and an unclaimed seed here shows the previous move
    # under a transcript that has moved on - the same lag as the loop's, in the
    # half of the picture the shot is actually of.
    seed_open "${HEX[$((${#HEX[@]} - 1))]}" "$mine" "$SHOOT_THREAD" open \
      || echo "  hero frame: the extension never claimed its seed - the drawer may be stale" >&2
    cmd_collapse >/dev/null 2>&1
    sleep 2
    cmd_nudge || true
  else
    # No drawer in this frame: put it away and photograph the transcript alone.
    cmd_leave >/dev/null 2>&1 || true
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
  tap "$x" "$y" 0.3
  settle_reset; poll 30 0.2 drawer_settled || true
}
# SEED THE BOARD, OPEN IT, AND PROVE THE EXTENSION OPENED ONTO *THAT* SEED.
#
# The trap this closes cost a whole store chain and three rounds of reading
# frames. `MessageDevBoard.claimSeededPayload()` is once per APPEX PROCESS, and
# the only thing that ends that process is leaving the thread - so a `back` that
# does not take leaves the extension re-opening the seed it already claimed. It
# then stages THAT, and every bubble in the transcript is one move behind.
#
# Nothing in the frames says so. Each board is a real, legal state; the caption
# row under it names the defender, and a defender does not change within a bout,
# so the row reads correctly for the bubble AND for its predecessor. The only
# tell is the summary line, which reads as "the caption describes the previous
# message" - and that is how this was mis-filed as a product bug three times.
# (ios/FoolishTests/MessageCaptionActorTests.swift pins both halves: the
# product's line names its own actor, and the defender row cannot detect a lag.)
#
# So the rig stops inferring. `claimSeededPayload` writes the hex it claimed to
# `dev.claimed`; this deletes the receipt, seeds, leaves, opens, and compares.
# No sleeps: a claim either happened in a fresh process or it did not, and the
# answer is a file. Three attempts, then a hard failure - a shoot that cannot
# seed is worth stopping, and was never worth photographing.
#
# $1 payload hex   $2 seat   $3 thread   $4 route (open|tapopen, default tapopen)
seed_open() {
  local hex="$1" seat="$2" thread="$3" route="${4:-tapopen}"
  local g; g=$(group_dir)
  local try got
  # THE FAST PATH, and the shape of what it can and cannot do.
  #
  # With a RIG_RESEED build and `dev.reseed` set, a LIVE appex adopts a new
  # `dev.fatboard` where it stands - so a re-seed onto the SAME thread is a file
  # write and a receipt, about a third of a second, instead of leave + blind row
  # probe + re-open, about ten seconds.
  #
  # It cannot help a re-seed that CHANGES thread, and that is not a limitation
  # of the flag - it is what a two-sided transcript costs. A move sent from a
  # thread lands in that thread as incoming, so alternating sides means actually
  # being in the other conversation, and getting there leaves this one, which
  # kills the appex anyway. `chain` alternates every move; `batch` never does.
  local lt="${FOOLISH_WORK:-/tmp/foolishrig}/lastthread.$SIM"
  if [ -f "$g/dev.reseed" ] && [ "$(cat "$lt" 2>/dev/null)" = "$thread" ] && drawer_up; then
    rm -f "$g/dev.claimed" "$g/dev.staged"
    printf '%s' "$hex"  > "$g/dev.fatboard"
    printf '%s' "$seat" > "$g/dev.seat"
    for try in $(seq 1 40); do
      got=$(cat "$g/dev.claimed" 2>/dev/null || true)
      [ "$got" = "$hex" ] && return 0
      sleep 0.1
    done
    echo "  seed: dev.reseed set but the appex never adopted it - falling back" >&2
  fi
  for try in 1 2 3; do
    rm -f "$g/dev.claimed" "$g/dev.staged"
    # KILL FIRST, THEN WRITE. Killing is what makes the next seed readable, and
    # it replaces `cmd_leave` outright: no drawer to put away, no thread to
    # leave, no conversation row to guess at. `cmd_enter` below still navigates
    # when the MOVE is going to the other thread, which is a transcript
    # requirement rather than a seeding one.
    #
    # The order matters on a RIG_RESEED build: a live appex adopts a new
    # `dev.fatboard` the moment it appears and `openSeededBoard` stages when
    # `dev.stage` is set, so seeding before the kill has the outgoing surface
    # stage a bubble nobody asked for.
    if ! kill_appex; then
      echo "  seed: the appex would not die (try $try)" >&2
      continue
    fi
    printf '%s' "$hex"  > "$g/dev.fatboard"
    printf '%s' "$seat" > "$g/dev.seat"
    case "$route" in
      open) cmd_open "$thread" >/dev/null 2>&1 ;;
      *)    cmd_tapopen "$thread" >/dev/null 2>&1 || cmd_open "$thread" >/dev/null 2>&1 ;;
    esac
    # POLL the receipt rather than peeking once. The claim is written as the
    # extension opens, and the opens above now return the moment the drawer is
    # up instead of after a flat seven seconds - so the receipt can land a beat
    # later. This is the safety net that makes those short waits safe: a claim
    # that is coming is waited for, and one that is not still fails in a second.
    got=""
    for _ in 1 2 3 4 5 6 7 8; do
      got=$(cat "$g/dev.claimed" 2>/dev/null || true)
      if [ "$got" = "$hex" ]; then
        mkdir -p "$(dirname "$lt")"; printf '%s' "$thread" > "$lt"
        return 0
      fi
      sleep 0.25
    done
    if [ -z "$got" ]; then
      echo "  seed: the extension claimed nothing (try $try) - it re-opened a live appex" >&2
    else
      echo "  seed: claimed ${got:0:20}… wanted ${hex:0:20}… (try $try)" >&2
    fi
  done
  return 1
}

# SEND THE BUBBLE WE ASKED FOR, not whichever one is in the field.
#
# The other half of trap 10, and the half a claim receipt cannot see. A claim
# happens the instant the extension opens; the INSERT happens at the end of
# `stage()`'s expanded tail - a settle wait, a collapse, a transition, over a
# second - and `cmd_turn` taps Send as soon as a Send button exists. A Send
# button exists because the PREVIOUS bubble is still sitting there, so every
# send transmitted the previous move with a perfectly correct claim receipt
# beside it, and the transcript came out one move behind.
#
# `dev.staged` is written by the insert itself, so this waits on the one fact
# that means "the field now holds this payload". Not a sleep: the tail's length
# depends on whatever animation the seeded state plays, and a fixed wait tuned
# on an attack is short for a bout-ending cascade.
#
# $1 payload hex
send_staged() {
  local hex="$1" g try got
  g=$(group_dir)
  for try in $(seq 1 60); do
    got=$(tr -d '[:space:]' < "$g/dev.staged" 2>/dev/null || true)
    [ "$got" = "$hex" ] && break
    sleep 0.25
  done
  if [ "$got" != "$hex" ]; then
    echo "  send: the field never took this payload (staged ${got:0:20}…)" >&2
    return 1
  fi
  # WHAT WENT OUT, in order, for a shoot to compare against the generator's own
  # list. A transcript that disagrees with this file is Messages' doing; one
  # that agrees with it is the rig's.
  printf '%s\n' "$hex" >> "${FOOLISH_SENTLOG:-/tmp/rig_sent.log}"
  cmd_turn
}

# What the extension last claimed, for a hand-driven check.
cmd_claimed() {
  local g; g=$(group_dir)
  cat "$g/dev.claimed" 2>/dev/null && echo || echo "nothing claimed yet"
}

# LEAVE the thread, and stop there.
#
# Leaving is the part with a reason: it kills the appex, and
# `claimSeededPayload()` is once per appex PROCESS, so a re-seed is only read
# after the thread has been left. Coming BACK is a separate want, and most
# callers do not have it - `seed_open` routes to either thread next, `batch`
# and `lobby` call `open`, and `chain`'s no-drawer branch called `cmd_enter`
# on the very next line. They were all paying for a thread to be opened and
# then immediately closed again: on a chain that alternates threads it is a
# whole enter cycle per move, tapping a conversation row to land somewhere the
# next call walks straight back out of.
cmd_leave() {
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
  local i=0 inside=1
  while [ $i -lt 3 ]; do
    if ! in_thread; then inside=0; break; fi
    tap_ax "Messages" 0.3 || break
    poll 16 0.25 not_in_thread || true
    i=$((i + 1))
  done
  # `inside` is the loop's OWN last reading. The guard below used to re-ask the
  # identical question with nothing in between, which on the common path (we
  # left, and the loop proved it) is a describe-all for an answer already held.
  if [ "$inside" = 1 ] && in_thread; then
    echo "could not leave the thread - the next seed will not be read" >&2
    return 1
  fi
}

# Leave, and come back into the shoot thread. For callers that genuinely want
# to END there: `clearstage` (whose tap made Messages' own field first
# responder, costing the compact drawer 17pt - trap 11) and the `back` verb.
cmd_back() {
  cmd_leave || return 1
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
    # …AND THEN LEAVE THE THREAD, because the tap above was a tap INSIDE THE
    # COMPOSE AREA and Messages answers one by making its own text field first
    # responder - which costs the compact drawer 17pt (trap 11). Without this
    # the next frame is 16pt shorter than the same frame taken any other way,
    # silently, and a whole investigation has already been spent reading that
    # difference as a bug in the extension. `back` leaves and re-enters the
    # thread, which is what drops it.
    cmd_back >/dev/null 2>&1 || true
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
  cmd_leave
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

# ONE LOBBY CONTROL, by name. The lobby's buttons are wood like every other
# control, but WHICH wooden button matters here - Start and Leave share a row -
# and the rules checkbox is a plank too small for `bar_y` to see at all. Both
# are read off the screen (lib/ui.py `bar_spans`, `checkbox`) rather than from a
# y this file remembers, for the reason the whole rig does it that way: the
# lobby moves with the player count, the locale and the drawer's height.
#
#   start   the left button of the Start/Leave row
#   leave   the right one
#   join    the only button on the row (a lobby I am not seated in)
#   box     the passing checkbox
# TAP THE NEWEST FOOLISH BUBBLE in the transcript, which is the only way back
# onto a game whose first bubble has been sent - that send dismisses the drawer
# on purpose (the extension cannot be bound to a conversation it was not opened
# from), and `open` from the + menu always lands on the New game screen.
cmd_openbubble() {
  need_sim
  local x y
  front
  read -r x y < <(python3 "$LIB/ui.py" bubble | python3 -c "
import sys, ast
v = ast.literal_eval(sys.stdin.read().split('BUBBLE ')[1])
print(v[0], v[1]) if v else print(-1, -1)")
  [ "$x" = "-1" ] && { echo "no Foolish bubble in the transcript"; return 1; }
  tap "$x" "$y" 3
  echo "opened the bubble at $x,$y"
}

cmd_lobbytap() {
  need_sim
  local what="${1:?lobbytap start|leave|join|box}" y x
  # A NAME THIS DOES NOT KNOW IS NOT A TAP. Without this every unknown word
  # fell through to "not leave, so the first button" - and the first button on
  # a lobby is Start playing, so a typo silently DEALT THE GAME. That cost four
  # takes of a scenario that had already been set up correctly.
  case "$what" in
    start|leave|join|box) ;;
    *) echo "lobbytap: no such control '$what' (start|leave|join|box)" >&2; return 2 ;;
  esac
  front
  if [ "$what" = "box" ]; then
    read -r x y < <(python3 "$LIB/ui.py" box | python3 -c "
import sys, ast
v = ast.literal_eval(sys.stdin.read().split('BOX ')[1])
print(v[0], v[1]) if v else print(-1, -1)")
    [ "$x" = "-1" ] && { echo "no checkbox on screen"; return 1; }
    tap "$x" "$y" 0.4
    echo "tapped the passing checkbox at $x,$y"
    return 0
  fi
  y=$(bar_y 0); [ "$y" = "-1" ] && { echo "no button row on screen"; return 1; }
  x=$(python3 "$LIB/ui.py" span "$y" | python3 -c "
import sys, ast
sp = ast.literal_eval(sys.stdin.read().split('SPAN ')[1])
which = '$what'
print(-1 if not sp else (sp[-1][0] if which == 'leave' else sp[0][0]))")
  [ "$x" = "-1" ] && { echo "no button on the row"; return 1; }
  tap "$x" "$y" 0.4
  echo "tapped $what at $x,$y"
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
  tap_ax "Send" 0.3 || { echo "nothing staged - is stageseed on?" >&2; return 1; }
  # Four seconds, five times a run, for an event that announces itself. The
  # owner, watching: "seems to be a small pause right before sending".
  poll 40 0.2 sent_done || true
  # …AND THEN LET THE BUBBLE LAND. The Send button goes the moment Messages
  # accepts the text, which is BEFORE the bubble is in the transcript - and the
  # next move opens the extension by finding the newest bubble's icon
  # (`ui.py lastmsg`). Returning on `sent_done` alone moved the cost rather than
  # removing it: the moves dropped ~4s each and the frame that tapped the fresh
  # bubble went 4.9s -> 25.9s. Owner, watching: "hell of a wait before you make
  # the final move".
  sleep 0.8
}

# --------------------------------------------------------------- state ----

# Writing a new seed VOIDS the old claim receipt (trap 10): what the extension
# last opened onto is no longer an answer about what it is being asked to open
# onto now, and a receipt left standing would read as one.
cmd_seed()   { need_sim; local g; g=$(group_dir); rm -f "$g/dev.claimed"
               python3 "$LIB/seed.py" "$@"; }

# Did the extension open onto the seed currently on disk? Trap 10: an appex that
# did not die re-opens the seed it already claimed, and every frame after that
# is one state behind while still looking individually plausible.
claim_ok() {
  local g; g=$(group_dir)
  local want got
  want=$(tr -d '[:space:]' < "$g/dev.fatboard" 2>/dev/null || true)
  got=$(tr -d '[:space:]'  < "$g/dev.claimed"  2>/dev/null || true)
  [ -n "$want" ] && [ "$want" = "$got" ]
}
cmd_unseed() { local g; g=$(group_dir)
               rm -f "$g/dev.fatboard" "$g/dev.seat" "$g/dev.replay" "$g/dev.claimed" \
                     "$g/dev.staged"
               echo "seed removed"; }

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

# PIN THE GENESIS DEAL. `createWaiting`'s DEBUG hook reads `dev.seed` and seals
# the lobby with a 32-byte seed of that one value, so two runs of the same
# scenario deal the same cards - which is the only way a BEFORE and an AFTER
# film of the same take can be laid side by side and read as one comparison.
# Off (a random deal per create) with no argument value, exactly as a shipping
# build always is.
cmd_deal() {
  local g; g=$(group_dir)
  if [ "${1:-}" = "off" ]; then rm -f "$g/dev.seed"; echo "deal: random"
  else printf '%s' "${1:-3}" > "$g/dev.seed"; echo "deal: seed ${1:-3}"; fi
}

cmd_ruler() {
  local g; g=$(group_dir)
  if [ "${1:-on}" = "off" ]; then rm -f "$g/dev.ruler"; echo "ruler off"
  else : > "$g/dev.ruler"; echo "ruler on"; fi
}

# `dev.stage`: make a seeded open ALSO stage its own chain as a bubble, so a
# frame's last bubble is the board underneath it. On for every gameplay frame
# whose transcript is visible.
# `dev.reseed`: the RUNTIME half of RIG_RESEED. Both are needed - a build
# without the flag ignores this file entirely.
cmd_reseed() {
  local g; g=$(group_dir)
  if [ "${1:-on}" = "off" ]; then rm -f "$g/dev.reseed"; echo "reseed off"
  else : > "$g/dev.reseed"; echo "reseed on (needs a FOOLISH_RESEED=1 build)"; fi
}

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
    # RESEED FAST PATH (RIG_RESEED build + `dev.reseed`). `cmd_seed` has just
    # written the new board, and a live appex adopts it where it stands - so a
    # frame that follows one whose drawer is still up needs no leave, no blind
    # row probe and no re-open. `claim_ok` is the same receipt the slow path
    # checks, so nothing is taken on trust.
    #
    # It applies to a NARROW case and that is inherent: the drawer has to still
    # be up, so any frame whose `act` SENT its bubble (turn / select) dismissed
    # the surface and pays the full cycle. Frames that only stage - `hint`,
    # `good` - chain.
    if [ -n "${mode:-}" ] && [ -f "$(group_dir)/dev.reseed" ] && drawer_up \
       && poll 40 0.1 claim_ok; then
      :
    else
    # One bad frame must not end the run. `set -e` applies inside this loop, so
    # an un-guarded failure here killed a 41-shot batch after its FIRST line and
    # still exited 0 - the list simply stopped, with nothing to say it had.
    kill_appex || { echo "!! $name skipped - the appex would not die" >&2; continue; }
    cmd_open_retry || { echo "!! $name skipped - could not open the extension" >&2; continue; }
    # …and it opened onto THIS seed, not the one before it (trap 10). `back`
    # returning 0 says the thread was left, not that the appex died; only the
    # claim receipt says that. One re-open, then skip the frame - a photograph
    # of the previous state is worse than a missing one, because it looks fine.
    if [ -n "${mode:-}" ] && ! claim_ok; then
      echo "   $name: stale seed, re-opening" >&2
      kill_appex && cmd_open_retry || true
      claim_ok || { echo "!! $name skipped - the extension never claimed its seed" >&2; continue; }
    fi
    fi
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
  leave)    shift; cmd_leave "$@" ;;
  killappex) shift; kill_appex "$@" ;;
  expand)   shift; cmd_expand "$@" ;;
  collapse) shift; cmd_collapse "$@" ;;
  goodtap)  shift; cmd_goodtap "$@" ;;
  seed)     shift; cmd_seed "$@" ;;
  unseed)   shift; cmd_unseed "$@" ;;
  claimed)  shift; cmd_claimed "$@" ;;
  prefs)    shift; cmd_prefs "$@" ;;
  slowmo)   shift; cmd_slowmo "$@" ;;
  deal)     shift; cmd_deal "$@" ;;
  ruler)    shift; cmd_ruler "$@" ;;
  stageseed) shift; cmd_stageseed "$@" ;;
  reseed)   shift; cmd_reseed "$@" ;;
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
  lobbytap) shift; cmd_lobbytap "$@" ;;
  openbubble) shift; cmd_openbubble "$@" ;;
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
