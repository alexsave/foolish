#!/usr/bin/env bash
# mac_tests.sh - the half of this repo's test suite that CANNOT run in CI.
#
# Everything portable is already gated on Linux (.github/workflows/ios.yml):
# the C bridge smoke test, the goldens diff, the architecture lint and the
# Swift/TypeScript replay-names codec parity. What is left needs Xcode and a
# simulator - 513 XCTest cases in FoolishTests plus 24 headless game-loop cases
# in HarnessTests, ~50s together on an M-series Mac - and there is no macOS CI
# job to run them: the repo is private, where Actions minutes are metered and
# macOS bills at 10x Linux.
#
# So this script IS the gate. It is the whole Mac-side invocation in one place,
# instead of the tribal knowledge it used to be, and it does the three things
# that are easy to forget:
#
#   1. rebuild the C engine xcframework, so the tests run against the engine in
#      this working tree and not last week's binary;
#   2. regenerate Foolish.xcodeproj from project.yml (both are build artifacts,
#      git-ignored, and hand-editing either is how they drift);
#   3. put the entitlements back, BYTE AND TIMESTAMP. `xcodegen generate`
#      silently blanks ios/FoolishApp/Foolish.entitlements to an empty <dict/>,
#      destroying the applinks:foolish.cards universal-links entry (§16.C5). It
#      reports only "Created project"; the file is TRACKED, so the damage lands
#      in your diff looking intentional, and everything builds and tests fine
#      without it - it would surface as broken universal links in production and
#      nowhere earlier.
#
# The body that does all three is shared/scripts/ios_mac_tests.sh, which every
# product in the repo runs; the long reasons for the `cp -p` restore and the
# stale-build-description retry sit there, next to the code. This file is
# foolish's half: its schemes, its library, and what it generates first.
#
# Usage:
#   ios/scripts/mac_tests.sh                 # xcframework + project + all of it
#   ios/scripts/mac_tests.sh unit            # FoolishTests only
#   ios/scripts/mac_tests.sh harness         # HarnessTests only
#   ios/scripts/mac_tests.sh app             # build the SHIPPING scheme only
#   ios/scripts/mac_tests.sh --no-lib unit   # skip the ~2 min xcframework build
#
#   DEST='platform=iOS Simulator,id=<udid>' ios/scripts/mac_tests.sh   # default: newest iPhone sim
#
# FIRST RUN IN A FRESH CHECKOUT FAILS, and that is not a regression: the
# ComponentSnapshotTests references (ios/FoolishTests/__Snapshots__) are
# git-ignored, so run one records them ("No reference was found on disk") and
# run two compares against them. Re-run once before believing a snapshot
# failure - and eyeball the recorded PNGs, since nothing else will.
#
# A note on schemes, because this has cost a release before: `Foolish` is the
# standalone iOS app (bundle cards.foolish.app) and owns FoolishTests, but the
# product that actually SHIPS is `FoolishMessagesApp` (bundle cards.foolish.msg).
# The default run tests the first and BUILDS the second, so a shipping-side
# compile break cannot hide behind a green unit-test run.
set -euo pipefail

HELP_FILE="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$0")/../.."          # foolish/

export HELP_FILE
export PROJECT="ios/Foolish.xcodeproj"
export TEST_SCHEMES="unit=Foolish harness=FoolishHarness"
export BUILD_SCHEMES="app=FoolishMessagesApp"
export LIB_CMD="make -C c ios-lib"
export LIB_LABEL="C engine xcframework (make ios-lib)"
export XCFRAMEWORK="ios/vendor/Foolish.xcframework"

# ---- 0. the generated modules FoolishKit compiles ---------------------------
#
# sdk/swift/gen is a BUILD OUTPUT, not committed, and FoolishKit compiles it:
# kernel.ios.swift (the kernel's layouts) and gen/i18n (the app's text, written
# from c/i18n). Every npm lane regenerates it through a pre-hook; nothing on the
# Mac path did, so an Xcode build here could compile last week's modules against
# this week's headers and this week's strings.
#
# That is not hypothetical. It was measured on exactly this script: a language's
# name was changed in shared/c/i18n/languages.h, the suite was run, and the test that
# exists to catch that PASSED - because the Swift it compiled still held the old
# value. A mutation that does not reach the artifact looks identical to a test
# that cannot fail.
export PRE_CMD="bash tools/structgen/gen.sh"
export PRE_LABEL="generated modules (tools/structgen/gen.sh)"

exec bash ../shared/scripts/ios_mac_tests.sh "$@"
