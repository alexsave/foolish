#!/usr/bin/env bash
# THE RELEASE GATE. A shipped build must offer no way to choose which seat you
# are, and this is what refuses to let that stop being true.
#
# WHY IT IS ITS OWN GATE. In the game this tree was forked from, choosing a seat
# meant choosing whose cards you could see. Here it means CHOOSING TO BE THE WOLF
# - choosing to read the wolves' channel, choosing to be the seat whose kill vote
# counts tonight. A solo rig that leaked into Release would not be a debug
# affordance that shipped; it would be the game, gone.
#
# Two halves, and the second is the one that cannot be argued with:
#
#   1. SOURCE. Every file that names the picker is wrapped in
#      `#if DEBUG || SOLO_TESTING`, and no file outside that gate mentions it.
#      Cheap, runs anywhere, and catches the mistake at the moment it is made.
#   2. BINARY. Build WerewolfKit for Release and assert the symbol is not in it.
#      This is the half that asks the PRODUCT rather than the diff.
#
# NEITHER HALF IS SUFFICIENT ALONE, and it is worth writing down which way each
# one is weak, because a gate whose limits are not written down gets trusted for
# things it does not do:
#
#   * the BINARY half alone would pass a file whose guard was deleted while its
#     only call site stayed guarded - the type compiles, nothing references it,
#     and the linker dead-strips it. Measured: that exact mutation leaves the
#     Release binary clean.
#   * the SOURCE half alone is a text check, and text checks can be walked
#     around.
#
# Together they close it: you cannot get the symbol INTO the Release binary
# without an unguarded reference, and an unguarded reference is what half 1
# refuses. Both halves are mutation-checked - with the picker genuinely shipped
# (guard deleted AND an unguarded reference added) the Release binary carries 86
# occurrences of the symbol, so the grep below is not vacuous.
#
# Half 1 alone is what CI runs (there is no macOS runner). Half 2 runs on a Mac,
# from ios/scripts/mac_tests.sh.
#
# Usage:
#   ios/scripts/release_gate.sh          # source only (portable)
#   ios/scripts/release_gate.sh binary   # source + the Release build check
set -euo pipefail
cd "$(dirname "$0")/../.."

fail() { echo "RELEASE GATE FAILED: $*" >&2; exit 1; }

# The symbols a release build must not contain, and the guard they must sit
# behind. Add to this list, never remove from it.
GUARDED='SoloSeatPicker|SoloDealButton|DevFlags'
GUARD='#if DEBUG \|\| SOLO_TESTING'

echo "== source: every mention of the solo rig is behind the debug guard =="
hits=0
while IFS= read -r f; do
  grep -Eq "$GUARDED" "$f" || continue
  hits=$((hits + 1))
  # The file either IS the guarded file (its whole body inside one #if) or it
  # mentions the symbol inside a guarded block. Both are proven the same way:
  # every LINE that names a guarded symbol must have an unclosed
  # `#if DEBUG || SOLO_TESTING` open above it.
  python3 - "$f" "$GUARDED" <<'PY' || fail "$f names the solo rig outside #if DEBUG || SOLO_TESTING"
import re, sys
path, pattern = sys.argv[1], sys.argv[2]
depth_debug = 0          # how many enclosing #if are the debug guard
stack = []
bad = []
for n, line in enumerate(open(path), 1):
    s = line.strip()
    if s.startswith("#if"):
        stack.append("DEBUG" if "DEBUG" in s and "SOLO_TESTING" in s else "other")
    elif s.startswith("#endif"):
        if stack: stack.pop()
    elif s.startswith("#else"):
        if stack: stack[-1] = "other"        # the #else of a debug guard is NOT debug
    elif re.search(pattern, s) and not s.startswith("//"):
        if "DEBUG" not in stack:
            bad.append((n, s[:90]))
if bad:
    for n, s in bad: print(f"  {path}:{n}: {s}")
    sys.exit(1)
PY
done < <(find ios sdk -name '*.swift' -not -path '*/WerewolfTests/*')
[ "$hits" -gt 0 ] || fail "no file mentions the solo rig at all - the gate is asserting nothing"
echo "   $hits file(s) checked, all guarded"

# And the RELEASE ROUTE for an unresolved seat must be the spectator surface. A
# gate on the absence of the picker says nothing about what replaced it, and
# "nothing replaced it" is a blank screen a seated player could also land on.
grep -q 'SpectatorScreen()' ios/WerewolfKit/RootView.swift \
  || fail "RootView has no SpectatorScreen branch - an unresolved seat has nowhere to go"
echo "   an unresolved seat routes to the spectator surface"

[ "${1:-}" = "binary" ] || { echo "== source gate green (pass 'binary' for the Release build check) =="; exit 0; }

echo "== binary: WerewolfKit built for Release contains no picker =="
command -v xcodebuild >/dev/null || fail "xcodebuild is not on this machine"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
xcodebuild -project ios/Werewolf.xcodeproj -scheme WerewolfMessagesApp \
  -configuration Release -sdk iphonesimulator -derivedDataPath "$out" -quiet build \
  CODE_SIGNING_ALLOWED=NO >/dev/null
bin=$(find "$out" -name WerewolfKit -type f -path '*WerewolfKit.framework*' | head -1)
[ -n "$bin" ] || fail "could not find the Release WerewolfKit binary"
for sym in SoloSeatPicker SoloDealButton DevFlags; do
  if nm -gU "$bin" 2>/dev/null | grep -q "$sym" || strings "$bin" | grep -q "$sym"; then
    fail "the Release binary contains $sym"
  fi
done
echo "   $(basename "$bin"): no SoloSeatPicker, no SoloDealButton, no DevFlags"
echo "== release gate green =="
