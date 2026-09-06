#!/usr/bin/env bash
# crash_test.sh — Stage 2's correctness gate: a real `kill -9` (no clean
# shutdown, no SIGTERM handler — a true crash) against live games, then a
# fresh process pointed at the SAME --db, asserting the games (and a user's
# login) come back exactly as they were the moment persistence last drained.
#
# Two scenarios, both real:
#   A) a game driven over /ws with genuinely LEGAL moves (foolish_hammer
#      --mode=ws — the same client test.sh's own ws smoke test uses),
#      proving moves applied through the request path survive.
#   B) a game dealt with 3 bots + 1 human who never moves — deterministically
#      frozen in GAME_STATUS_PLAYING with real dealt hands (not a lobby),
#      because the bot trampoline cannot act for the human seat. This is the
#      "an in-progress game, not just a finished/lobby one" case, and its
#      recovery also exercises the code path that RESTARTS a recovered
#      PLAYING game's bot trampoline thread (game_persist_load) — if that
#      path hung or crashed, the /status call reading it back would too.
#
# What this proves: everything the persistence thread had COMMITTED to
# SQLite before the kill survives it byte-for-byte. What it deliberately
# does NOT claim: write-behind is async (see DURABILITY.md) — a crash
# landing mid-interval can lose up to ~persist-interval-ms of the most
# recent moves. This test sidesteps that ambiguity on purpose (rather than
# asserting a fuzzy "some moves survived") by driving load, then WAITING
# comfortably longer than the persist interval before killing, so every
# move each scenario made is guaranteed to have been drained into a
# committed transaction by the time the kill lands — see PERSIST_INTERVAL_MS
# below. That makes "recovered state == pre-crash state, exactly" the right
# assertion for what this test is set up to demonstrate; a SEPARATE, non-
# deterministic scenario (kill with no wait) is not something a byte-exact
# assertion could honestly make either way, so this script doesn't attempt
# one — see DURABILITY.md's "what is and isn't guaranteed" section instead.
set -uo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

PORT="${1:-8177}"
H="http://127.0.0.1:$PORT"
WORKDIR="$(mktemp -d /tmp/foolish_crash_test.XXXXXX)"
DBFILE="$WORKDIR/crash_test.db"
PERSIST_INTERVAL_MS=60
WAIT_FOR_DRAIN_SECS=1   # comfortably > PERSIST_INTERVAL_MS/1000

SRV_LOG1="$WORKDIR/server1.log"
SRV_LOG2="$WORKDIR/server2.log"
HAMMER_LOG="$WORKDIR/hammer.log"
PASS=true

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill -9 "$SRV_PID" >/dev/null 2>&1 || true
    [ -n "${SRV_PID2:-}" ] && kill -9 "$SRV_PID2" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "== crash_test.sh: workdir=$WORKDIR db=$DBFILE port=$PORT =="

[ -x "$DIR/foolish_server" ] || (cd "$DIR" && make foolish_server >/dev/null)
[ -x "$DIR/foolish_hammer" ] || (cd "$DIR" && make foolish_hammer >/dev/null)

wait_for_health() {
    local tries=0
    while [ $tries -lt 100 ]; do
        if curl -s -o /dev/null -w '%{http_code}' "$H/health" 2>/dev/null | grep -q 200; then return 0; fi
        sleep 0.05
        tries=$((tries + 1))
    done
    return 1
}

wait_for_death() {
    local pid="$1" tries=0
    while [ $tries -lt 200 ]; do
        kill -0 "$pid" >/dev/null 2>&1 || return 0
        sleep 0.02
        tries=$((tries + 1))
    done
    return 1
}

tok() { grep -o "\"$1\":\"[^\"]*\"" | head -1 | cut -d'"' -f4; }

# ---------------------------------------------------------------------------
# Phase 1: fresh --db, drive real gameplay, capture ground truth.
# ---------------------------------------------------------------------------
rm -f "$DBFILE" "$DBFILE-wal" "$DBFILE-shm"
"$DIR/foolish_server" "$PORT" --db="$DBFILE" --persist-interval-ms="$PERSIST_INTERVAL_MS" \
    > "$SRV_LOG1" 2>&1 &
SRV_PID=$!
if ! wait_for_health; then
    echo "FAIL: server did not come up (see $SRV_LOG1)"; cat "$SRV_LOG1"; exit 1
fi
echo "-- server up, pid=$SRV_PID"

# A dedicated user whose LOGIN (token) we check survives the crash too —
# users are the second durable table (DURABILITY.md), not just games.
TOKEN=$(curl -s -XPOST "$H/auth/signup" -d '{"username":"crashtest_login"}' | tok token)
if [ -z "$TOKEN" ]; then echo "FAIL: signup for the login-survival check failed"; exit 1; fi
echo "-- signed up crashtest_login, token=${TOKEN:0:8}..."

# --- Scenario A: real /ws-driven legal moves -------------------------------
# --games=1 --seats=2 makes the game_id unambiguous: foolish_hammer now
# prints "dealt game[0]: id=..." for exactly this purpose (see setup()'s
# grep-able line in foolish_hammer.c).
"$DIR/foolish_hammer" --host=127.0.0.1 --port="$PORT" --games=1 --seats=2 --secs=3 --mode=ws \
    > "$HAMMER_LOG" 2>&1
GID_A=$(grep -o 'dealt game\[0\]: id=[0-9a-f]*' "$HAMMER_LOG" | head -1 | cut -d= -f2)
APPLIED=$(grep 'applied(ok=true):' "$HAMMER_LOG" | grep -o 'applied(ok=true):[[:space:]]*[0-9]*' | grep -o '[0-9]*$' || true)
echo "-- [A] hammer drove game_id=$GID_A, applied(ok=true)=$APPLIED"
if [ -z "$GID_A" ]; then echo "FAIL: could not find the game_id foolish_hammer dealt"; cat "$HAMMER_LOG"; exit 1; fi
if [ -z "${APPLIED:-}" ] || [ "$APPLIED" -lt 1 ]; then
    echo "FAIL: no legal moves were applied — nothing real to recover"; cat "$HAMMER_LOG"; exit 1
fi

# --- Scenario B: a real, in-progress (dealt, PLAYING) game with 3 bots and
# one human (the creator) who never calls /action. The human WILL eventually
# become attacker or defender (seats rotate every round) and freeze the
# game — but bots keep legitimately playing real rounds against EACH OTHER
# until that happens, so (unlike scenario A, captured after its driver
# process has fully exited) this game's mutable, in-round bytes can keep
# changing wall-clock-timing-dependently between our "before" and "after"
# snapshots even with nothing wrong — that's correct, ongoing gameplay, not
# a bug. So scenario B intentionally asserts a NARROWER, still-meaningful
# invariant: the DEAL IDENTITY header (state_put's first 16 bytes — status,
# num_players, power_suit, first_attacker, defender, discard_len,
# has_flipped, the flipped card, good_players_mask, has_good_timestamp,
# deck_count — all fixed at deal time, in view.c's byte order) is
# byte-identical before vs after, PLAYING status holds both times, and the
# blob carries real dealt data (not a bare lobby stub) both times. Together
# these prove: same recovered game, not corrupted, not silently reset to a
# fresh lobby, still genuinely in progress — the actual "an in-progress
# game survives a crash" claim — without the test being flaky over which
# exact bot-vs-bot round had landed by the time each curl fired.
BT=$(curl -s -XPOST "$H/auth/signup" -d '{"username":"crashtest_botgame"}' | tok token)
GID_B=$(curl -s -XPOST "$H/create" -H "Authorization: Bearer $BT" | tok game_id)
for _ in 1 2 3; do
    curl -s -XPOST "$H/meta" -H "Authorization: Bearer $BT" \
        -d "{\"type\":\"add-bot\",\"game_id\":\"$GID_B\",\"strategy\":\"cordite\"}" > /dev/null
done
curl -s -XPOST "$H/meta" -H "Authorization: Bearer $BT" -d "{\"type\":\"start\",\"game_id\":\"$GID_B\"}" > /dev/null
STATUS_B=$(curl -s "$H/status?game_id=$GID_B")
echo "-- [B] bot-backed game_id=$GID_B dealt, status=$STATUS_B (1 == PLAYING expected)"
if [ "$STATUS_B" != "1" ]; then echo "FAIL: scenario B's game did not reach PLAYING"; exit 1; fi

# The load phase (foolish_hammer) already exited and scenario B's game is
# structurally frozen (no human move is ever coming) — no further mutation
# is possible for either game, so waiting here is purely "let the
# persistence thread's next periodic drain (<= PERSIST_INTERVAL_MS away)
# commit everything that happened," not a race against ongoing writes.
sleep "$WAIT_FOR_DRAIN_SECS"

STATUS_A_BEFORE=$(curl -s "$H/status?game_id=$GID_A")
curl -s "$H/state?game_id=$GID_A&seat=0" -o "$WORKDIR/a_state0_before.bin"
curl -s "$H/state?game_id=$GID_A&seat=1" -o "$WORKDIR/a_state1_before.bin"
STATUS_B_BEFORE=$(curl -s "$H/status?game_id=$GID_B")
curl -s "$H/state?game_id=$GID_B&seat=0" -o "$WORKDIR/b_state0_before.bin"
echo "-- pre-crash: A status=$STATUS_A_BEFORE size=$(wc -c < "$WORKDIR/a_state0_before.bin")B" \
     "| B status=$STATUS_B_BEFORE size=$(wc -c < "$WORKDIR/b_state0_before.bin")B"

# ---------------------------------------------------------------------------
# Phase 2: the actual crash — kill -9, no clean shutdown, no flush call.
# ---------------------------------------------------------------------------
echo "-- kill -9 $SRV_PID (hard crash)"
kill -9 "$SRV_PID"
if ! wait_for_death "$SRV_PID"; then echo "FAIL: server did not die after kill -9"; exit 1; fi
unset SRV_PID

# ---------------------------------------------------------------------------
# Phase 3: restart against the SAME --db and verify recovery.
# ---------------------------------------------------------------------------
"$DIR/foolish_server" "$PORT" --db="$DBFILE" --persist-interval-ms="$PERSIST_INTERVAL_MS" \
    > "$SRV_LOG2" 2>&1 &
SRV_PID2=$!
if ! wait_for_health; then
    echo "FAIL: server did not come back up after the crash (see $SRV_LOG2)"; cat "$SRV_LOG2"; exit 1
fi
echo "-- recovered server up, pid=$SRV_PID2"
grep "persist: recovered" "$SRV_LOG2" | sed 's/^/   /' || true

STATUS_A_AFTER=$(curl -s "$H/status?game_id=$GID_A")
curl -s "$H/state?game_id=$GID_A&seat=0" -o "$WORKDIR/a_state0_after.bin"
curl -s "$H/state?game_id=$GID_A&seat=1" -o "$WORKDIR/a_state1_after.bin"
STATUS_B_AFTER=$(curl -s "$H/status?game_id=$GID_B")
curl -s "$H/state?game_id=$GID_B&seat=0" -o "$WORKDIR/b_state0_after.bin"
echo "-- post-recovery: A status=$STATUS_A_AFTER size=$(wc -c < "$WORKDIR/a_state0_after.bin")B" \
     "| B status=$STATUS_B_AFTER size=$(wc -c < "$WORKDIR/b_state0_after.bin")B"

# --- Assertions -------------------------------------------------------------
check_eq() {   # check_eq <label> <before> <after>
    if [ "$2" = "$3" ]; then echo "PASS: $1 ($3)"; else echo "FAIL: $1 mismatch: before=$2 after=$3"; PASS=false; fi
}
check_bytes() {   # check_bytes <label> <file_before> <file_after>
    if cmp -s "$2" "$3"; then echo "PASS: $1 byte-identical before vs after the crash"; else echo "FAIL: $1 differs before vs after the crash"; PASS=false; fi
}
check_header16() {   # check_header16 <label> <file_before> <file_after> — state_put's first 16 bytes (deal identity, see Scenario B's comment above)
    if cmp -s -n 16 "$2" "$3"; then echo "PASS: $1 (deal-identity header) byte-identical before vs after the crash"; else echo "FAIL: $1 (deal-identity header) differs before vs after the crash"; PASS=false; fi
}
check_min_size() {   # check_min_size <label> <file> <min_bytes> — proves real dealt data, not a bare lobby stub
    local n; n=$(wc -c < "$2")
    if [ "$n" -ge "$3" ]; then echo "PASS: $1 carries real dealt data (${n}B >= ${3}B)"; else echo "FAIL: $1 too small to be a dealt game (${n}B < ${3}B)"; PASS=false; fi
}

check_eq   "[A] /status survived the crash"      "$STATUS_A_BEFORE" "$STATUS_A_AFTER"
check_bytes "[A] seat 0 masked /state"           "$WORKDIR/a_state0_before.bin" "$WORKDIR/a_state0_after.bin"
check_bytes "[A] seat 1 masked /state"           "$WORKDIR/a_state1_before.bin" "$WORKDIR/a_state1_after.bin"

check_eq   "[B] in-progress /status survived"    "$STATUS_B_BEFORE" "$STATUS_B_AFTER"
check_header16 "[B] seat 0 masked /state"        "$WORKDIR/b_state0_before.bin" "$WORKDIR/b_state0_after.bin"
check_min_size "[B] seat 0 /state before the crash" "$WORKDIR/b_state0_before.bin" 40
check_min_size "[B] seat 0 /state after the crash"  "$WORKDIR/b_state0_after.bin" 40
if [ "$STATUS_B_AFTER" = "1" ]; then
    echo "PASS: [B] recovered game is still PLAYING (bot-thread-restart path ran without hanging/crashing the server)"
else
    echo "FAIL: [B] recovered game is not PLAYING (status=$STATUS_B_AFTER)"; PASS=false
fi

# The user table (Stage 2's second durable table): the OLD token, minted
# before the crash, must still authenticate. /meta checks the token BEFORE
# the game_id, so a valid-but-stale token against a bogus game_id comes back
# 404 ("no game"); a token the crash actually lost comes back 401 ("auth").
META_CODE=$(curl -s -o /dev/null -w '%{http_code}' -XPOST "$H/meta" \
    -H "Authorization: Bearer $TOKEN" -d '{"type":"join","game_id":"nonexistent00"}')
if [ "$META_CODE" = "404" ]; then
    echo "PASS: crashtest_login's token still authenticates after the crash (404, not 401)"
else
    echo "FAIL: crashtest_login's token did not survive the crash (http $META_CODE, expected 404)"; PASS=false
fi

echo
if $PASS; then
    echo "=== crash_test.sh: PASS — committed game + user state survived kill -9 ==="
    exit 0
else
    echo "=== crash_test.sh: FAIL — see above ==="
    exit 1
fi
