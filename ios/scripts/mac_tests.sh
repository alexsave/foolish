#!/usr/bin/env bash
# mac_tests.sh - the half of this repo's test suite that CANNOT run in CI.
#
# Everything portable is already gated on Linux (.github/workflows/ios.yml):
# the C bridge smoke test, the goldens diff, the architecture lint and the
# Swift/TypeScript replay-names codec parity. What is left needs Xcode and a
# simulator - 512 XCTest cases in FoolishTests plus 24 headless game-loop cases
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
# WHY THE RESTORE COPIES THE TIMESTAMP TOO, which is the whole reason this
# script was unrunnable for a while. Restoring the bytes with `git checkout`
# leaves the file with a NEW mtime, and Xcode records the entitlements file's
# timestamp in the build description it caches under
# DerivedData/<project>/Build/Intermediates.noindex/XCBuildData. Any later build
# that reuses that description sees a timestamp it does not recognise and
# refuses to run:
#
#   error: Entitlements file "Foolish.entitlements" was modified during the
#   build, which is not supported.
#
# The message is a misdiagnosis twice over: nothing is modified during the
# build, and the CONTENT is not modified at all - a bare `touch` on an otherwise
# untouched file reproduces it exactly. It is also PERMANENT: it does not clear
# on the next build, and putting the old timestamp back afterwards does not
# clear it either (both measured), so every build fails until a fresh build
# description is created. A script that restores with `git checkout` therefore
# poisons every build after it, while looking like a signing problem.
#
# `cp -p` restores the mtime to the nanosecond, so the file is byte-for-byte AND
# stat-for-stat what it was before xcodegen ran, and there is nothing for Xcode
# to notice. That is not papering over the error - across the whole run the file
# genuinely did not change.
#
# For a DerivedData somebody else already poisoned (a hand-run `xcodegen`, an
# Xcode GUI session), run_scheme below recognises that exact error, deletes just
# the build-description cache and retries once. Deleting XCBuildData keeps the
# compiled products, so it costs one partial rebuild, not a cold one.
#
# Usage:
#   ios/scripts/mac_tests.sh                 # xcframework + project + all of it
#   ios/scripts/mac_tests.sh unit            # FoolishTests only
#   ios/scripts/mac_tests.sh harness         # HarnessTests only
#   ios/scripts/mac_tests.sh app             # build the SHIPPING scheme only
#   ios/scripts/mac_tests.sh --no-lib unit   # skip the ~2 min xcframework build
#   ios/scripts/mac_tests.sh --regen         # force xcodegen (project.yml is the trigger)
#
#   DEST='platform=iOS Simulator,name=iPhone 17' ios/scripts/mac_tests.sh
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

cd "$(dirname "$0")/../.."          # repo root
ROOT="$PWD"

DEST="${DEST:-platform=iOS Simulator,name=iPhone 16}"
PROJECT="ios/Foolish.xcodeproj"

build_lib=1
force_regen=0
want_unit=0
want_harness=0
want_app=0
for arg in "$@"; do
  case "$arg" in
    --no-lib)  build_lib=0 ;;
    --regen)   force_regen=1 ;;
    unit)      want_unit=1 ;;
    harness)   want_harness=1 ;;
    app)       want_app=1 ;;
    -h|--help) sed -n '/^# Usage:/,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
  esac
done
if [ $((want_unit + want_harness + want_app)) -eq 0 ]; then
  want_unit=1; want_harness=1; want_app=1
fi

for tool in xcodebuild xcodegen; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "error: $tool not found. This script needs a Mac with Xcode 16+ and" >&2
    echo "       'brew install xcodegen'. The portable checks that DO run" >&2
    echo "       without Xcode are in .github/workflows/ios.yml." >&2
    exit 1
  }
done

# Prettify if the user happens to have a formatter; raw xcodebuild otherwise.
if command -v xcbeautify >/dev/null 2>&1; then FMT=(xcbeautify)
elif command -v xcpretty >/dev/null 2>&1; then FMT=(xcpretty)
else FMT=(cat); fi

say() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

# ---- 1. the C engine, as the app links it ----------------------------------
if [ "$build_lib" -eq 1 ]; then
  say "C engine xcframework (make ios-lib)"
  make -C c ios-lib
else
  say "skipping the xcframework build (--no-lib)"
  [ -d ios/vendor/Foolish.xcframework ] || {
    echo "error: --no-lib was passed but ios/vendor/Foolish.xcframework does not exist" >&2
    exit 1
  }
fi

# ---- 2. the project, plus 3. the entitlements it eats -----------------------
# The entitlements files this repo OWNS, which is exactly git's list of tracked
# ones. Do not reach for `find ios -name '*.entitlements'`: it also matches the
# vendored SPM checkouts under ios/build/.../SourcePackages, which are read-only
# and are not ours to write - copying onto one aborts the restore mid-loop and
# leaves the real entitlements blanked, which is the very damage this guards.
# The find is only a fallback for a checkout with no git.
if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  ENT_FILES=$(git -C "$ROOT" ls-files -- '*.entitlements')
else
  ENT_FILES=$(find ios -type f -name '*.entitlements' \
                -not -path 'ios/build/*' -not -path 'ios/vendor/*' | LC_ALL=C sort)
fi

BACKUP_DIR=""
bak_path() { printf '%s/%s' "$BACKUP_DIR" "$(printf '%s' "$1" | tr '/' '_')"; }

backup_entitlements() {
  BACKUP_DIR="$(mktemp -d -t foolish_entitlements)"
  local rel
  for rel in $ENT_FILES; do
    # -p is the entire point: it carries the mtime, so the restore below is
    # invisible to Xcode's cached build description.
    cp -p "$rel" "$(bak_path "$rel")" || {
      echo "error: could not back up $rel - refusing to run xcodegen over it" >&2
      exit 1
    }
  done
}

restore_entitlements() {
  [ -n "$BACKUP_DIR" ] || return 0
  local rel bak restored=0 failed=0
  for rel in $ENT_FILES; do
    bak="$(bak_path "$rel")"
    [ -f "$bak" ] || continue
    if ! cmp -s "$bak" "$rel"; then
      echo "  restored $rel (xcodegen blanked it)"
      restored=$((restored + 1))
    fi
    # bytes AND mtime, every time. One failure must never abandon the files
    # after it in the list - that is how a blanked entitlements file ships.
    cp -p "$bak" "$rel" || { echo "error: could not restore $rel" >&2; failed=1; }
  done
  if [ "$restored" -eq 0 ]; then echo "  entitlements untouched by this xcodegen run"; fi
  rm -rf "$BACKUP_DIR"
  BACKUP_DIR=""
  [ "$failed" -eq 0 ] || exit 1
}

# If xcodegen dies half way, or somebody interrupts it, the entitlements still
# go back.
trap 'if [ -n "$BACKUP_DIR" ]; then restore_entitlements; fi' EXIT

# Regenerate ONLY when project.yml has actually moved. This is not the fix for
# anything - the mtime-preserving restore is - it just skips a dance that buys
# nothing: the project is a pure function of project.yml, so regenerating on an
# unchanged spec reproduces the same .xcodeproj. `--regen` forces it, for a
# .xcodeproj that has been hand-edited or half-written.
regen=0
if [ ! -d "$PROJECT" ]; then regen=1
elif [ "$force_regen" -eq 1 ]; then regen=1
elif [ ios/project.yml -nt "$PROJECT/project.pbxproj" ]; then regen=1
fi

if [ "$regen" -eq 1 ]; then
  say "Xcode project (xcodegen generate)"
  backup_entitlements
  (cd ios && xcodegen generate)
  restore_entitlements
else
  say "Xcode project is current (project.yml unchanged) - not regenerating"
fi

# ---- 4. the tests that need a simulator ------------------------------------
POISON='was modified during the build'

# Delete the cached build description holding the stale entitlements timestamp.
# Compiled products and module caches survive, so the retry is a partial
# rebuild (~30s) rather than the cold one an `xcodebuild clean` or a DerivedData
# wipe would force. The path is read back out of xcodebuild's own log instead of
# being guessed, and only a directory literally named
# .../Build/Intermediates.noindex/XCBuildData is ever removed.
unpoison_derived_data() {   # $1 = the failed build's log
  local dd
  dd=$(sed -n 's|.*\(/DerivedData/[^/]*\)/Build/.*|\1|p' "$1" | head -1)
  [ -n "$dd" ] || return 1
  dd="$HOME/Library/Developer/Xcode$dd/Build/Intermediates.noindex/XCBuildData"
  [ -d "$dd" ] || return 1
  echo "  clearing the stale build description: $dd"
  rm -rf "$dd"
}

run_scheme() {   # $1 = scheme, $2 = build|test
  local scheme="$1" action="$2" log rc
  log="$(mktemp -t foolish_xcodebuild)"
  say "$action: scheme $scheme"

  set +e
  xcodebuild -project "$PROJECT" -scheme "$scheme" -destination "$DEST" "$action" \
    2>&1 | tee "$log" | "${FMT[@]}"
  rc=${PIPESTATUS[0]}
  set -e

  if [ "$rc" -ne 0 ] && grep -q "$POISON" "$log"; then
    echo
    echo "  ^ that is Xcode's stale-entitlements-timestamp error: not a signing"
    echo "    problem, and not a change in your tree. Healing it, retrying once."
    if unpoison_derived_data "$log"; then
      set +e
      xcodebuild -project "$PROJECT" -scheme "$scheme" -destination "$DEST" "$action" \
        2>&1 | tee "$log" | "${FMT[@]}"
      rc=${PIPESTATUS[0]}
      set -e
    fi
  fi

  rm -f "$log"
  return "$rc"
}

if [ "$want_unit" -eq 1 ];    then run_scheme Foolish test; fi
if [ "$want_harness" -eq 1 ]; then run_scheme FoolishHarness test; fi
# The shipping product has no test target of its own; compiling it is the check.
if [ "$want_app" -eq 1 ];     then run_scheme FoolishMessagesApp build; fi

say "Mac-side suite finished clean"
