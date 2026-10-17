#!/usr/bin/env bash
# ios_mac_tests.sh - the Mac-side gate every iOS product in this repo runs: the
# part of its suite that needs Xcode and a simulator, and so cannot run in CI.
#
# A product never runs this directly. Its own ios/scripts/mac_tests.sh keeps the
# usage text and the product's reasons, `cd`s to the product's folder (the one
# holding c/ and ios/), sets the env below and `exec`s this file.
#
# It does the things that are easy to forget, in this order:
#
#   0. PRE_CMD, whatever the product must generate before Xcode compiles it;
#   1. rebuild the C xcframework (LIB_CMD), so the tests run against the kernel
#      in this working tree and not last week's binary;
#   2. regenerate the Xcode project from project.yml (both are build artifacts,
#      git-ignored, and hand-editing either is how they drift);
#   3. put every tracked entitlements file back, BYTE AND TIMESTAMP, because
#      `xcodegen generate` silently blanks the ones it knows about to an empty
#      <dict/>. It reports only "Created project"; the file is TRACKED, so the
#      damage lands in your diff looking intentional, and everything builds and
#      tests fine without it - the loss surfaces in production and nowhere
#      earlier (each product's script says what its entitlements carry);
#   4. test every TEST_SCHEMES scheme (compiled first, then run on a locked
#      simulator - see "THE SIMULATOR") and build every BUILD_SCHEMES one.
#
# WHY THE RESTORE COPIES THE TIMESTAMP TOO, which is the whole reason the first
# of these scripts was unrunnable for a while. Restoring the bytes with
# `git checkout` leaves the file with a NEW mtime, and Xcode records the
# entitlements file's timestamp in the build description it caches under
# DerivedData/<project>/Build/Intermediates.noindex/XCBuildData. Any later build
# that reuses that description sees a timestamp it does not recognise and
# refuses to run:
#
#   error: Entitlements file "<name>.entitlements" was modified during the
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
# The env (paths are relative to the product folder, the working directory):
#
#   PROJECT         the generated Xcode project, e.g. ios/<Name>.<xcode project>
#   IOS_DIR         where project.yml lives and xcodegen runs (default: PROJECT's dir)
#   TEST_SCHEMES    "word=Scheme ...": each is run with `xcodebuild test`
#   BUILD_SCHEMES   "word=Scheme ...": each is run with `xcodebuild build`
#                   The words are the command-line selectors; no selector runs
#                   every scheme, tests first, in the order given.
#   LIB_CMD         the command that writes the xcframework (skipped by --no-lib)
#   LIB_LABEL       what the step prints (default: LIB_CMD)
#   XCFRAMEWORK     what --no-lib requires to exist already: the xcframework and
#                   anything else LIB_CMD writes, space separated
#   PRE_CMD         run first on every invocation, --no-lib or not (optional)
#   PRE_LABEL       what that step prints (default: PRE_CMD)
#   SUITE_NAME      prefix of the final line (optional)
#   HELP_FILE       the product script, whose "# Usage:" block --help prints
#   DEST            the xcodebuild destination (optional: unset, it is this
#                   Mac's own test simulator - see "THE SIMULATOR" below)
#   SIM_NAME        that simulator's name (default: ios-mac-tests)
#
# Flags: --no-lib, --regen, -h/--help, and the scheme words.
set -euo pipefail

ROOT="$PWD"

: "${PROJECT:?PROJECT is not set - run ios/scripts/mac_tests.sh of the product instead}"
# ---- THE SIMULATOR: one per Mac, used by nothing else, one run at a time ----
#
# Every run used to test on "the first iPhone on the newest runtime", which is
# the one simulator every agent, every hand-run xcodebuild and every rig on this
# Mac also reaches for first. Two runs on one simulator kill each other: the
# second run's install of the same bundle SIGKILLs the first run's test host
# mid-suite, and xcodebuild reports it as
#
#   Early unexpected exit, operation never finished bootstrapping - no restart
#   will be attempted. (Underlying Error: Test crashed with signal kill ...)
#
# against whichever innocent test happened to be running ("Test crashed with
# signal kill"). Reproduced 2026-10-17 by running two worktrees' suites at once
# on one simulator: 2 of the first 3 overlapping runs died that way, and 10 of
# 10 sequential runs on the same simulator were green. It "passes on rerun"
# because the rerun usually does not overlap.
#
# So, when DEST is not given:
#   - the run uses a simulator of its own NAME ($SIM_NAME, created on first use
#     as the newest runtime's first iPhone model), which nothing else on the
#     Mac picks by accident;
#   - the test schemes run under an exclusive lock on that simulator, so a
#     second run (another worktree, another agent, another product) WAITS for
#     the first instead of killing it. The lock is a kernel flock on an open
#     file, so it dies with its holder: no stale lock after a crash or a kill;
#   - the run boots it and waits for the boot to finish before xcodebuild sees
#     it, and shuts it down again at exit if it found it shut down, so idle
#     test simulators never pile up (a booted simulator is hundreds of
#     processes, and too many of them exhaust the per-user process limit).
# A DEST you pass is used as given, and is locked the same way (by its text),
# so two runs naming the same device also queue instead of colliding.
SIM_NAME="${SIM_NAME:-ios-mac-tests}"
SIM_UDID=""
if [ -z "${DEST:-}" ]; then
  # `simctl list` prints runtimes oldest first, so the last iOS section that
  # holds an iPhone wins. Prints "<runtime>|<model>|<udid of SIM_NAME or empty>".
  pick="$(xcrun simctl list devices available | awk -v want="$SIM_NAME" '
    /^-- iOS / { in_ios = 1; rt = $0; gsub(/^-- | --$/, "", rt); model = ""; mine = ""; next }
    /^-- /     { in_ios = 0; next }
    in_ios && match($0, /\([0-9A-F-]{36}\)/) {
      udid = substr($0, RSTART + 1, RLENGTH - 2)
      name = $0; sub(/^ +/, "", name); sub(/ \([0-9A-F-]{36}\).*$/, "", name)
      if (name == want) mine = udid
      else if (model == "" && name ~ /^iPhone/) model = name
      if (model != "" || mine != "") best = rt "|" model "|" mine
    }
    END { print best }')"
  [ -n "$pick" ] || { echo "error: no available iPhone simulator; set DEST" >&2; exit 2; }
  IFS='|' read -r sim_runtime sim_model SIM_UDID <<<"$pick"
  if [ -z "$SIM_UDID" ]; then
    echo "creating this Mac's test simulator: $SIM_NAME ($sim_model, $sim_runtime)"
    SIM_UDID="$(xcrun simctl create "$SIM_NAME" "$sim_model" "$sim_runtime")"
  fi
  DEST="platform=iOS Simulator,id=$SIM_UDID"
  echo "DEST not set - using $SIM_NAME: $DEST"
fi
: "${LIB_CMD:?LIB_CMD is not set}"
: "${XCFRAMEWORK:?XCFRAMEWORK is not set}"
IOS_DIR="${IOS_DIR:-$(dirname "$PROJECT")}"
TEST_SCHEMES="${TEST_SCHEMES:-}"
BUILD_SCHEMES="${BUILD_SCHEMES:-}"
PRE_CMD="${PRE_CMD:-}"
[ -n "$TEST_SCHEMES$BUILD_SCHEMES" ] || { echo "error: neither TEST_SCHEMES nor BUILD_SCHEMES is set" >&2; exit 2; }

build_lib=1
wanted=" "
for arg in "$@"; do
  case "$arg" in
    --no-lib)  build_lib=0 ;;
    --regen)   ;;   # regeneration is every run now; kept so old command lines work
    -h|--help) sed -n '/^# Usage:/,/^set -euo/p' "${HELP_FILE:-$0}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *)
      known=0
      for pair in $TEST_SCHEMES $BUILD_SCHEMES; do
        [ "${pair%%=*}" = "$arg" ] && known=1
      done
      [ "$known" -eq 1 ] || { echo "unknown argument: $arg (try --help)" >&2; exit 2; }
      wanted="$wanted$arg "
      ;;
  esac
done
want() { [ "$wanted" = " " ] || case "$wanted" in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

for tool in xcodebuild xcodegen; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "error: $tool not found. This script needs a Mac with Xcode 16+ and" >&2
    echo "       'brew install xcodegen'. The portable checks that DO run" >&2
    echo "       without Xcode are in the product's CI lane." >&2
    exit 1
  }
done

# Prettify if the user happens to have a formatter; raw xcodebuild otherwise.
if command -v xcbeautify >/dev/null 2>&1; then FMT=(xcbeautify)
elif command -v xcpretty >/dev/null 2>&1; then FMT=(xcpretty)
else FMT=(cat); fi

say() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

# ---- 0. what the product generates before Xcode compiles it -----------------
if [ -n "$PRE_CMD" ]; then
  say "${PRE_LABEL:-$PRE_CMD}"
  eval "$PRE_CMD"
fi

# ---- 1. the C kernel, as the app links it ----------------------------------
if [ "$build_lib" -eq 1 ]; then
  say "${LIB_LABEL:-$LIB_CMD}"
  eval "$LIB_CMD"
else
  say "skipping the xcframework build (--no-lib)"
  for need in $XCFRAMEWORK; do
    [ -e "$need" ] || {
      echo "error: --no-lib was passed but $need does not exist" >&2
      exit 1
    }
  done
fi

# ---- 2. the project, plus 3. the entitlements it eats -----------------------
# The entitlements files this product OWNS, which is exactly git's list of
# tracked ones under the working directory. Do not reach for
# `find ios -name '*.entitlements'`: it also matches the vendored SPM checkouts
# under ios/build/.../SourcePackages, which are read-only and are not ours to
# write - copying onto one aborts the restore mid-loop and leaves the real
# entitlements blanked, which is the very damage this guards. The find is only a
# fallback for a checkout with no git.
if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  ENT_FILES=$(git -C "$ROOT" ls-files -- '*.entitlements')
else
  ENT_FILES=$(find "$IOS_DIR" -type f -name '*.entitlements' \
                -not -path "$IOS_DIR/build/*" -not -path "$IOS_DIR/vendor/*" | LC_ALL=C sort)
fi

BACKUP_DIR=""
bak_path() { printf '%s/%s' "$BACKUP_DIR" "$(printf '%s' "$1" | tr '/' '_')"; }

backup_entitlements() {
  BACKUP_DIR="$(mktemp -d -t ios_entitlements)"
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

# Regenerate on EVERY run. The project is a function of project.yml AND of the
# files in every folder it lists as sources, so no trigger short of running
# xcodegen knows it is current. This used to regenerate only when project.yml
# was newer than the project (plus an opt-in mtime watch of source folders),
# and that missed exactly the case that matters: a rebase or a checkout that
# brings in a new .swift file leaves project.yml alone, the stale project
# silently lacks the file, and the build fails on a symbol that is plainly in
# the tree ("cannot find 'TrumpNudge' in scope") until somebody thinks of
# `--regen`. A deleted file was missed the same way, and no mtime can show it.
#
# It costs nothing: `xcodegen generate` on a product's project.yml takes ~0.1s.
# When the result is byte-identical to the project that was there, the old copy
# goes back with its timestamps, so Xcode sees no change at all and keeps its
# cached build description. `--regen` is still accepted and changes nothing.
say "Xcode project (xcodegen generate)"
PROJ_KEEP="$(mktemp -d -t ios_xcodeproj)"
PROJ_NAME="$(basename "$PROJECT")"
if [ -d "$PROJECT" ]; then cp -Rp "$PROJECT" "$PROJ_KEEP/"; fi
backup_entitlements
(cd "$IOS_DIR" && xcodegen generate --quiet)
restore_entitlements
if [ -d "$PROJ_KEEP/$PROJ_NAME" ] && diff -rq "$PROJ_KEEP/$PROJ_NAME" "$PROJECT" >/dev/null 2>&1; then
  rm -rf "$PROJECT"
  mv "$PROJ_KEEP/$PROJ_NAME" "$PROJECT"
  echo "  project unchanged - kept the previous one, timestamps and all"
else
  echo "  project regenerated (a source file or project.yml changed)"
fi
rm -rf "$PROJ_KEEP"

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

run_scheme() {   # $1 = scheme, $2 = build|build-for-testing|test-without-building
  local scheme="$1" action="$2" log rc
  log="$(mktemp -t ios_xcodebuild)"
  say "$action: scheme $scheme"

  # NO SYSDIAGNOSE ON A FAILURE. xcodebuild's default (`on-failure`) runs
  # `simctl diagnose --timeout=600` after any failing test, and on a busy Mac it
  # runs to that timeout: a run whose tests had finished in 3 seconds sat for
  # ten more minutes before reporting one failed assertion (2026-10-17). The
  # failures, their messages and the .xcresult are all still there without it.
  local extra=()
  if [ "$action" = test-without-building ]; then extra=(-collect-test-diagnostics never); fi

  set +e
  xcodebuild -project "$PROJECT" -scheme "$scheme" -destination "$DEST" ${extra[@]+"${extra[@]}"} "$action" \
    2>&1 | tee "$log" | "${FMT[@]}"
  rc=${PIPESTATUS[0]}
  set -e

  if [ "$rc" -ne 0 ] && grep -q "$POISON" "$log"; then
    echo
    echo "  ^ that is Xcode's stale-entitlements-timestamp error: not a signing"
    echo "    problem, and not a change in your tree. Healing it, retrying once."
    if unpoison_derived_data "$log"; then
      set +e
      xcodebuild -project "$PROJECT" -scheme "$scheme" -destination "$DEST" ${extra[@]+"${extra[@]}"} "$action" \
        2>&1 | tee "$log" | "${FMT[@]}"
      rc=${PIPESTATUS[0]}
      set -e
    fi
  fi

  # A green run that executed nothing proves nothing.
  if [ "$action" = test-without-building ] && [ "$rc" -eq 0 ] && ! grep -Eq 'Executed [1-9][0-9]* tests?' "$log"; then
    echo "error: $scheme reported success after executing zero tests" >&2
    rc=1
  fi

  rm -f "$log"
  return "$rc"
}

# The lock and the boot (see "THE SIMULATOR" at the top). Only RUNNING the tests
# holds the lock: every test scheme is compiled first, unlocked, with
# build-for-testing, and then run with test-without-building under the lock.
# Holding it through the compile made a second run wait out the first one's
# cold build (8 minutes in a fresh worktree) for no reason - compiling never
# touches a simulator, and neither do the C build, xcodegen or the build-only
# schemes.
LOCK_DIR="$HOME/Library/Caches/ios_mac_tests"
SIM_WAS_SHUTDOWN=0

acquire_simulator() {
  mkdir -p "$LOCK_DIR"
  local lock
  lock="$LOCK_DIR/$(printf '%s' "$DEST" | tr -c 'A-Za-z0-9.-' '_').lock"
  exec 9>>"$lock"
  # flock(2) on fd 9, taken by a child: the lock belongs to the open file, which
  # this shell (and every xcodebuild it starts) holds until it exits. lockf(1)
  # cannot do this - it only locks around a command it runs itself.
  if ! /usr/bin/perl -MFcntl=:flock -e 'open(my $f, ">&=", 9) or die; exit(flock($f, LOCK_EX | LOCK_NB) ? 0 : 1)'; then
    echo "  waiting for the simulator: another run is testing on it ($(cat "$lock.who" 2>/dev/null || echo unknown))"
    /usr/bin/perl -MFcntl=:flock -e 'open(my $f, ">&=", 9) or die; flock($f, LOCK_EX) or die "flock: $!"'
  fi
  printf 'pid %s in %s, since %s\n' "$$" "$ROOT" "$(date '+%H:%M:%S')" > "$lock.who"
  if [ -n "$SIM_UDID" ]; then
    if xcrun simctl list devices | grep -q "($SIM_UDID) (Shutdown)"; then SIM_WAS_SHUTDOWN=1; fi
    xcrun simctl boot "$SIM_UDID" >/dev/null 2>&1 || true   # already booted is fine
    xcrun simctl bootstatus "$SIM_UDID" -b >/dev/null
  fi
}

release_simulator() {
  if [ "$SIM_WAS_SHUTDOWN" -eq 1 ]; then
    xcrun simctl shutdown "$SIM_UDID" >/dev/null 2>&1 || true
    SIM_WAS_SHUTDOWN=0
  fi
  exec 9>&-
}

any_test=0
for pair in $TEST_SCHEMES; do if want "${pair%%=*}"; then any_test=1; fi; done
if [ "$any_test" -eq 1 ]; then
  for pair in $TEST_SCHEMES; do
    if want "${pair%%=*}"; then run_scheme "${pair#*=}" build-for-testing; fi
  done
  say "simulator (${SIM_UDID:+$SIM_NAME, }locked for this run)"
  trap 'if [ -n "$BACKUP_DIR" ]; then restore_entitlements; fi; release_simulator' EXIT
  acquire_simulator
  for pair in $TEST_SCHEMES; do
    if want "${pair%%=*}"; then run_scheme "${pair#*=}" test-without-building; fi
  done
  release_simulator
  trap 'if [ -n "$BACKUP_DIR" ]; then restore_entitlements; fi' EXIT
fi
# A shipping product with no test target of its own: compiling it is the check.
for pair in $BUILD_SCHEMES; do
  if want "${pair%%=*}"; then run_scheme "${pair#*=}" build; fi
done

say "${SUITE_NAME:+$SUITE_NAME }Mac-side suite finished clean"
