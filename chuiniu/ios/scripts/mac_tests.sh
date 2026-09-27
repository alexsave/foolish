#!/usr/bin/env bash
# COPIED from chuiniu/ios/scripts/mac_tests.sh at 03eb3362 - a later lift into shared/ replaces it
#
# mac_tests.sh - Chui Niu's Mac-side gate: what needs Xcode and a simulator.
# The portable half (the kernel, its wire and the bridge smoke) is
# `make -C chuiniu/c run asan`.
#
# THE SCAFFOLD HAS NO KERNEL YET: step 1 runs only once project.yml links
# vendor/Chuiniu.xcframework (the tie-together uncomments that line); until
# then it is skipped and says so.
#
# It does the three things that are easy to forget (foolish's reasons, kept):
#   1. rebuild the kernel's xcframework AND the generated readers and string
#      tables beside it (`make -C chuiniu/c ios-lib`), so the tests run
#      against the kernel in this tree and readers of its layout;
#   2. regenerate Chuiniu.xcodeproj from project.yml (a git-ignored build
#      artifact, like uttt's);
#   3. put every tracked entitlements file back, BYTES AND MTIME (`cp -p`),
#      because xcodegen blanks the ones it knows about and a `git checkout`
#      restore leaves a new mtime that poisons Xcode's cached build
#      description ("Entitlements file ... was modified during the build").
#      This product's only entitlements file is set per-config by hand, so
#      xcodegen does not know it, but the guard stays: it costs nothing.
#
# Usage:
#   chuiniu/ios/scripts/mac_tests.sh                 # lib + project + tests + the shipping build
#   chuiniu/ios/scripts/mac_tests.sh unit            # ChuiniuKitTests only
#   chuiniu/ios/scripts/mac_tests.sh app             # build the SHIPPING scheme only
#   chuiniu/ios/scripts/mac_tests.sh --no-lib unit   # skip the xcframework build (once there is one)
#   chuiniu/ios/scripts/mac_tests.sh --regen         # force xcodegen
#
#   DEST='platform=iOS Simulator,id=<udid>' chuiniu/ios/scripts/mac_tests.sh
#
# At most two booted simulators on this Mac, ever (every shell dies past
# that); shut yours down when done.
set -euo pipefail

cd "$(dirname "$0")/../../.."        # repo root
ROOT="$PWD"

DEST="${DEST:-platform=iOS Simulator,name=iPhone 17e}"
PROJECT="chuiniu/ios/Chuiniu.xcodeproj"

build_lib=1
force_regen=0
want_unit=0
want_app=0
for arg in "$@"; do
  case "$arg" in
    --no-lib)  build_lib=0 ;;
    --regen)   force_regen=1 ;;
    unit)      want_unit=1 ;;
    app)       want_app=1 ;;
    -h|--help) sed -n '/^# Usage:/,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
  esac
done
if [ $((want_unit + want_app)) -eq 0 ]; then want_unit=1; want_app=1; fi

# The kernel is linked once project.yml names the xcframework on a live line.
if ! grep -Eq '^[[:space:]]*- framework: vendor/Chuiniu\.xcframework' chuiniu/ios/project.yml 2>/dev/null; then
  kernel_linked=0
else
  kernel_linked=1
fi

for tool in xcodebuild xcodegen; do
  command -v "$tool" >/dev/null 2>&1 || { echo "error: $tool not found ('brew install xcodegen')" >&2; exit 1; }
done

if command -v xcbeautify >/dev/null 2>&1; then FMT=(xcbeautify)
elif command -v xcpretty >/dev/null 2>&1; then FMT=(xcpretty)
else FMT=(cat); fi

say() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

# ---- 1. the kernel, its readers and its strings, as the app links them -------
if [ "$kernel_linked" -eq 0 ]; then
  say "no kernel linked yet (project.yml runs on FakeKernel) - skipping the xcframework build"
elif [ "$build_lib" -eq 1 ]; then
  say "kernel xcframework + generated readers (make -C chuiniu/c ios-lib)"
  make -C chuiniu/c ios-lib
else
  say "skipping the xcframework build (--no-lib)"
  [ -d chuiniu/ios/vendor/Chuiniu.xcframework ] && [ -f chuiniu/ios/Generated/ChuiniuKernel.swift ] || {
    echo "error: --no-lib but chuiniu/ios/vendor or chuiniu/ios/Generated is missing" >&2; exit 1; }
fi

# ---- 2. the project, and 3. the entitlements it may eat --------------------
ENT_FILES=$(git -C "$ROOT" ls-files -- 'chuiniu/ios/*.entitlements')
BACKUP_DIR=""
bak_path() { printf '%s/%s' "$BACKUP_DIR" "$(printf '%s' "$1" | tr '/' '_')"; }
backup_entitlements() {
  BACKUP_DIR="$(mktemp -d -t chuiniu_entitlements)"
  local rel
  for rel in $ENT_FILES; do
    cp -p "$rel" "$(bak_path "$rel")" || { echo "error: could not back up $rel" >&2; exit 1; }
  done
}
restore_entitlements() {
  [ -n "$BACKUP_DIR" ] || return 0
  local rel bak failed=0
  for rel in $ENT_FILES; do
    bak="$(bak_path "$rel")"
    [ -f "$bak" ] || continue
    cmp -s "$bak" "$rel" || echo "  restored $rel (xcodegen blanked it)"
    cp -p "$bak" "$rel" || { echo "error: could not restore $rel" >&2; failed=1; }
  done
  rm -rf "$BACKUP_DIR"; BACKUP_DIR=""
  [ "$failed" -eq 0 ] || exit 1
}
trap 'if [ -n "$BACKUP_DIR" ]; then restore_entitlements; fi' EXIT

# xcodegen only re-reads project.yml when told to, and a new .swift file is
# silently left out of a stale project: regenerate when the spec moved, when
# asked, or when any source directory is newer than the project.
regen=0
if [ ! -d "$PROJECT" ] || [ "$force_regen" -eq 1 ] || [ chuiniu/ios/project.yml -nt "$PROJECT/project.pbxproj" ]; then regen=1; fi
if [ "$regen" -eq 0 ] && [ -n "$(find chuiniu/ios/ChuiniuKit chuiniu/ios/ChuiniuKitTests chuiniu/ios/ChuiniuMessages \
      -newer "$PROJECT/project.pbxproj" -name '*.swift' -print -quit)" ]; then regen=1; fi
if [ "$regen" -eq 1 ]; then
  say "Xcode project (xcodegen generate)"
  backup_entitlements
  (cd chuiniu/ios && xcodegen generate)
  restore_entitlements
else
  say "Xcode project is current - not regenerating"
fi

# ---- 4. the tests, and the shipping build ---------------------------------
POISON='was modified during the build'
unpoison_derived_data() {
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
  log="$(mktemp -t chuiniu_xcodebuild)"
  say "$action: scheme $scheme"
  set +e
  xcodebuild -project "$PROJECT" -scheme "$scheme" -destination "$DEST" "$action" 2>&1 | tee "$log" | "${FMT[@]}"
  rc=${PIPESTATUS[0]}
  set -e
  if [ "$rc" -ne 0 ] && grep -q "$POISON" "$log" && unpoison_derived_data "$log"; then
    set +e
    xcodebuild -project "$PROJECT" -scheme "$scheme" -destination "$DEST" "$action" 2>&1 | tee "$log" | "${FMT[@]}"
    rc=${PIPESTATUS[0]}
    set -e
  fi
  # A green run that executed nothing proves nothing.
  if [ "$action" = test ] && [ "$rc" -eq 0 ] && ! grep -Eq 'Executed [1-9][0-9]* tests?' "$log"; then
    echo "error: $scheme reported success after executing zero tests" >&2; rc=1
  fi
  rm -f "$log"
  return "$rc"
}

if [ "$want_unit" -eq 1 ]; then run_scheme ChuiniuKitTests test; fi
# The shipping product has no test target of its own; compiling it is the check.
if [ "$want_app" -eq 1 ]; then run_scheme ChuiniuMessagesApp build; fi

say "Chui Niu Mac-side suite finished clean"
