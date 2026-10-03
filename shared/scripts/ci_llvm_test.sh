#!/usr/bin/env bash
# ci_llvm.sh's retry, with no network and no root: a stub installer that fails
# N-1 times then succeeds must pass, and one that always fails must fail with
# its own exit code after exactly the bounded number of attempts.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
CI_LLVM_DEFINE_ONLY=1 . "$here/ci_llvm.sh"
set +e  # ci_llvm.sh turns errexit on; this test reads exit codes itself
export CI_LLVM_RETRY_PAUSE=0
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fails=0

# stub COUNTFILE FAILURES CODE - fail with CODE the first FAILURES calls, then succeed.
stub() {
  local n; n=$(( $(cat "$1" 2>/dev/null || echo 0) + 1 )); echo "$n" > "$1"
  [ "$n" -gt "$2" ] || return "$3"
}

echo 0 > "$tmp/a"
retry 4 stub "$tmp/a" 3 2 2>/dev/null; rc=$?
if [ $rc -eq 0 ] && [ "$(cat "$tmp/a")" = 4 ]; then echo "ok   fails 3 times, passes on attempt 4"
else echo "FAIL fails 3 times then passes: rc=$rc after $(cat "$tmp/a") calls"; fails=$((fails + 1)); fi

echo 0 > "$tmp/b"
retry 4 stub "$tmp/b" 99 2 2>/dev/null; rc=$?
if [ $rc -eq 2 ] && [ "$(cat "$tmp/b")" = 4 ]; then echo "ok   always fails: exit 2 after 4 attempts"
else echo "FAIL always fails: rc=$rc after $(cat "$tmp/b") calls, want 2 after 4"; fails=$((fails + 1)); fi

echo 0 > "$tmp/c"
retry 4 stub "$tmp/c" 0 7 2>/dev/null; rc=$?
if [ $rc -eq 0 ] && [ "$(cat "$tmp/c")" = 1 ]; then echo "ok   passes first time: one attempt"
else echo "FAIL passes first time: rc=$rc after $(cat "$tmp/c") calls"; fails=$((fails + 1)); fi

[ $fails -eq 0 ] && echo "ci_llvm retry: all pass" || { echo "ci_llvm retry: $fails failed"; exit 1; }
