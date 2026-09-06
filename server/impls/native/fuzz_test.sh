#!/usr/bin/env bash
# fuzz_test.sh — the adversarial-input gate. Builds the AddressSanitizer+UBSan
# server (foolish_server_asan) and the hostile client (fuzz_client), points the
# client at the server for a fixed burst, and FAILS if any of these happen:
#   1) AddressSanitizer or UBSan reports anything (heap/stack overflow,
#      use-after-free, signed-shift/overflow UB, ...) in the server log;
#   2) the server dies (a spike in connect failures, or health stops answering);
#   3) a /state seat=-2 disclosure anomaly (full-state leak) is detected;
#   4) a legitimate request can't get through after the storm.
#
# The fuzzer throws malformed HTTP, junk/oversized signups, forged tokens, meta
# abuse (bot-spam seat overflow, starting nonexistent games), unparseable binary
# /action bodies, hostile /state seats, wrong methods / non-existent routes, and
# malformed WebSocket handshakes+frames. See fuzz_client.c for the full menu.
#
# Nothing here is committed: the ASan binary, the fuzz client, and any db live
# under the build dir / a scratch tmp and are gitignored.
set -uo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

PORT="${1:-8399}"
THREADS="${2:-16}"
SECS="${3:-20}"
LOG="$(mktemp /tmp/foolish_fuzz.XXXXXX.log)"
PASS=true

cleanup() { [ -n "${SRV_PID:-}" ] && kill -9 "$SRV_PID" >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "== fuzz_test.sh: port=$PORT threads=$THREADS secs=$SECS log=$LOG =="

echo "-- building foolish_server_asan + fuzz_client (this can take a minute)"
make foolish_server_asan fuzz_client >/dev/null || { echo "FAIL: build"; exit 1; }

# detect_leaks=0: the server intentionally never frees its global tables and is
# killed with SIGKILL, so LeakSanitizer's exit-time scan would only report
# non-bugs. halt_on_error=0: keep running and collect EVERY sanitizer hit rather
# than dying on the first, so one run surfaces all of them.
ASAN_OPTIONS=detect_leaks=0:halt_on_error=0:abort_on_error=0 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 \
  ./foolish_server_asan "$PORT" --no-db > "$LOG" 2>&1 &
SRV_PID=$!

# wait for readiness
for _ in $(seq 1 50); do
    curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break
    sleep 0.1
done
if ! kill -0 "$SRV_PID" 2>/dev/null; then echo "FAIL: server did not start"; cat "$LOG"; exit 1; fi
echo "-- server up (pid=$SRV_PID), unleashing the fuzzer"

FZ_OUT="$(./fuzz_client 127.0.0.1 "$PORT" "$THREADS" "$SECS")"
echo "   $FZ_OUT"

# 1) server still alive?
if kill -0 "$SRV_PID" 2>/dev/null; then
    echo "PASS: server survived the fuzz"
else
    echo "FAIL: server process died during the fuzz"; PASS=false
fi

# 2) legit request still served?
POST="$(curl -s --max-time 3 -XPOST "http://127.0.0.1:$PORT/auth/signup" -d '{"username":"survivor"}')"
if echo "$POST" | grep -q '"token"'; then
    echo "PASS: a legitimate signup still succeeds after the storm"
else
    echo "FAIL: legitimate signup failed after the storm ($POST)"; PASS=false
fi

# 3) disclosure anomaly reported by the fuzzer?
if echo "$FZ_OUT" | grep -q "DISCLOSURE ANOMALY"; then
    echo "FAIL: /state seat=-2 full-state disclosure detected"; PASS=false
else
    echo "PASS: no /state disclosure anomaly"
fi

kill -9 "$SRV_PID" >/dev/null 2>&1; SRV_PID=""

# 4) any sanitizer report in the server log?
if grep -qE "runtime error|ERROR: AddressSanitizer|SUMMARY: (Address|Undefined)|heap-buffer-overflow|stack-buffer-overflow|use-after-free" "$LOG"; then
    echo "FAIL: sanitizer reported a problem:"
    grep -nE "runtime error|AddressSanitizer|SUMMARY:|overflow|use-after-free" "$LOG" | head -20
    PASS=false
else
    echo "PASS: clean under AddressSanitizer + UBSan (no reports)"
fi

echo
if $PASS; then
    echo "=== fuzz_test.sh: PASS — server withstands adversarial input, sanitizer-clean ==="
    exit 0
else
    echo "=== fuzz_test.sh: FAIL — see above (server log: $LOG) ==="
    exit 1
fi
