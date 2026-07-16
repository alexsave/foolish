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
curl -s "$H/state?game_id=$GID&seat=0" | xxd | head -3
echo "── seat 1 (bob) packed view (hex head):"
curl -s "$H/state?game_id=$GID&seat=1" | xxd | head -3
echo "── status: $(curl -s "$H/status?game_id=$GID")   (0 waiting / 1 playing / 2 over)"
