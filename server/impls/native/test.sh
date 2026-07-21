#!/usr/bin/env bash
# Smoke test for the native C server: two humans + a bot play a real game,
# every rule decided by the kernel. Start the server first (`make run`).
set -euo pipefail
H="${1:-http://127.0.0.1:8099}"
tok() { grep -o "\"$1\":\"[^\"]*\"" | head -1 | cut -d'"' -f4; }

echo "── health"; curl -s "$H/health"; echo
AT=$(curl -s -XPOST "$H/auth/signup" -d '{"username":"alice"}' | tok token)
BT=$(curl -s -XPOST "$H/auth/signup" -d '{"username":"bob"}'   | tok token)
echo "── alice=$AT bob=$BT"

GID=$(curl -s -XPOST "$H/create" -H "Authorization: Bearer $AT" | tok game_id)
echo "── created game $GID"
curl -s -XPOST "$H/meta" -H "Authorization: Bearer $BT" -d "{\"type\":\"join\",\"game_id\":\"$GID\"}"; echo " (bob joined)"
curl -s -XPOST "$H/meta" -H "Authorization: Bearer $AT" -d "{\"type\":\"add-bot\",\"game_id\":\"$GID\",\"strategy\":\"cordite\"}"; echo " (bot added)"
curl -s -XPOST "$H/meta" -H "Authorization: Bearer $AT" -d "{\"type\":\"start\",\"game_id\":\"$GID\"}"; echo " (alice ready)"
curl -s -XPOST "$H/meta" -H "Authorization: Bearer $BT" -d "{\"type\":\"start\",\"game_id\":\"$GID\"}"; echo " (bob ready → deal)"

# Views are the PACKED kernel wire now (view.c state_put) — the client decodes
# them with its own reader (Swift MaskedView), not curl. We just show the bytes
# to prove they arrive masked per seat.
echo "── seat 0 (alice) packed view (hex head):"
curl -s "$H/state?game_id=$GID&seat=0" | od -A x -t x1z | head -3
echo "── seat 1 (bob) packed view (hex head):"
curl -s "$H/state?game_id=$GID&seat=1" | od -A x -t x1z | head -3
echo "── status: $(curl -s "$H/status?game_id=$GID")   (0 waiting / 1 playing / 2 over)"

# ── WebSocket smoke test ────────────────────────────────────────────────
# Exercises the ws.h/ws.c handshake (Sec-WebSocket-Accept) + frame I/O +
# the /ws action protocol end-to-end, via a tiny, tightly-scoped run of
# foolish_hammer's own --mode=ws (own signup/create/join/start, never
# touches the game above): persistent connections, real legal moves,
# server-side apply. See foolish_hammer.c's header comment for the protocol.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST="${H#*://}"; HOST="${HOST%%/*}"
WPORT="${HOST##*:}"
echo "── ws smoke test (mode=ws, tiny scale, port $WPORT)"
if [ ! -x "$DIR/foolish_hammer" ]; then (cd "$DIR" && make foolish_hammer >/dev/null); fi
WS_OUT=$("$DIR/foolish_hammer" --host=127.0.0.1 --port="$WPORT" --games=1 --seats=2 --secs=2 --mode=ws)
echo "$WS_OUT" | tail -12
APPLIED=$(echo "$WS_OUT" | grep 'applied(ok=true):' | grep -o 'applied(ok=true):[[:space:]]*[0-9]*' | grep -o '[0-9]*$' || true)
if [ -n "${APPLIED:-}" ] && [ "$APPLIED" -gt 0 ]; then
    echo "── ws smoke test: PASS (${APPLIED} legal moves applied over persistent WS connections)"
else
    echo "── ws smoke test: FAIL (no applied moves)"
    exit 1
fi

# ── Stage 4 smoke test: spectator + server-side octogen bot ──────────────
# Own tiny game (games=1 seats=1 server-bot=octogen spectators=1): proves
# (a) a server-side octogen bot, added via /meta add-bot and driven entirely
# inside bot_thread/bot_drive, actually decides + plays (octogen_decisions
# from GET /stats > 0 — see SERVER_SCALING.md "Stage 4"), and (b) a
# read-only spectator WS connection (/ws?game_id=..&spectator=1) receives
# masked VIEW_SPECTATOR pushes and never gets a move accepted, even when it
# deliberately probes with a well-framed one (spectator_worker's
# SPEC_MOVE_PROBE_N — see foolish_hammer.c).
echo "── stage4 smoke test (spectator + server-bot=octogen, tiny scale, port $WPORT)"
S4_OUT=$("$DIR/foolish_hammer" --host=127.0.0.1 --port="$WPORT" --games=1 --seats=1 --server-bot=octogen --spectators=1 --secs=10 --mode=ws)
echo "$S4_OUT" | tail -16
OCT_DEC=$(echo "$S4_OUT" | grep 'octogen_decisions_summary:' | grep -o 'decisions=[0-9]*' | grep -o '[0-9]*$')
SPEC_ACCEPT=$(echo "$S4_OUT" | grep 'spectator move probes sent:' | grep -o 'accepted(MUST be 0): [0-9]*' | grep -o '[0-9]*$')
if [ -n "${OCT_DEC:-}" ] && [ "$OCT_DEC" -gt 0 ] && [ -n "${SPEC_ACCEPT:-}" ] && [ "$SPEC_ACCEPT" -eq 0 ]; then
    echo "── stage4 smoke test: PASS (octogen decided server-side ${OCT_DEC} time(s); spectator move probes accepted=0)"
else
    echo "── stage4 smoke test: FAIL (octogen_decisions=${OCT_DEC:-?} spectator_move_accepted=${SPEC_ACCEPT:-?})"
    exit 1
fi
