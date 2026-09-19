#!/usr/bin/env bash
# mac_tests.sh - the half of this repo's suite that needs Xcode and a simulator.
#
# Everything portable is already gated by `make -C c tests`: the kernel's
# assertions and the C bridge's, on any machine with a compiler. What is left
# needs a Mac, and there is no macOS CI job to run it, so THIS SCRIPT is the gate.
#
# It does the three things that are easy to forget:
#
#   1. rebuild the C xcframework, so the tests run against the kernel in this
#      working tree and not last week's binary;
#   2. regenerate Werewolf.xcodeproj from project.yml (both are build artifacts,
#      git-ignored, and hand-editing either is how they drift);
#   3. build the SHIPPING target as well as running the tests, so a compile break
#      on the product cannot hide behind a green unit-test run.
#
# --regen IS MANDATORY WHEN ADDING SOURCE FILES. xcodegen only re-reads
# project.yml when it is newer than the project, and a new .swift file does not
# touch project.yml - so without --regen the file is simply not in the target and
# every call into it fails to compile with a message about an unknown symbol.
#
# Usage:
#   ios/scripts/mac_tests.sh                 # xcframework + project + all of it
#   ios/scripts/mac_tests.sh unit            # WerewolfTests only
#   ios/scripts/mac_tests.sh app             # build the shipping scheme only
#   ios/scripts/mac_tests.sh --no-lib unit   # skip the xcframework build
#   ios/scripts/mac_tests.sh --regen         # force xcodegen
#
#   DEST='platform=iOS Simulator,name=iPhone 17' ios/scripts/mac_tests.sh
set -euo pipefail

cd "$(dirname "$0")/../.."          # repo root

DEST="${DEST:-platform=iOS Simulator,name=iPhone 17}"
DO_LIB=1
FORCE_REGEN=0
WHAT="all"

for arg in "$@"; do
  case "$arg" in
    --no-lib) DO_LIB=0 ;;
    --regen)  FORCE_REGEN=1 ;;
    unit|app|all) WHAT="$arg" ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

say() { printf '\n== %s ==\n' "$1"; }

# The portable gate runs first, always. A Swift test failing because the kernel
# is wrong is a failure that takes an hour to read; the same failure out of
# `make -C c tests` names the assertion.
say "C kernel + bridge"
make -C c tests

if [ "$DO_LIB" = 1 ]; then
  say "Werewolf.xcframework"
  make -C c ios-lib
fi

if [ "$FORCE_REGEN" = 1 ] || [ ! -d ios/Werewolf.xcodeproj ] \
   || [ ios/project.yml -nt ios/Werewolf.xcodeproj ]; then
  say "xcodegen"
  ( cd ios && xcodegen generate )
fi

run_scheme() {
  local scheme="$1"; shift
  xcodebuild -project ios/Werewolf.xcodeproj -scheme "$scheme" \
    -destination "$DEST" -quiet "$@"
}

if [ "$WHAT" = "all" ] || [ "$WHAT" = "unit" ]; then
  say "WerewolfTests"
  run_scheme WerewolfTests test
fi

if [ "$WHAT" = "all" ] || [ "$WHAT" = "app" ]; then
  # The product that SHIPS. Built, not tested: it has no tests of its own, and a
  # green WerewolfTests run says nothing about whether the extension compiles.
  say "WerewolfMessagesApp (build only)"
  run_scheme WerewolfMessagesApp build
fi

say "green"
