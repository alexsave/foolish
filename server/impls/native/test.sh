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

echo "── seat 0 (alice) view:"
V0=$(curl -s "$H/state?game_id=$GID&seat=0")
echo "$V0"

# The first attacker (alice) opens with her first hand card — the kernel decides
# whether it's legal and, if so, the defender/bot respond.
S=$(echo "$V0" | grep -o '"hand":\[{"s":[0-9-]*' | head -1 | grep -o '[0-9-]*$')
VV=$(echo "$V0" | grep -o '"hand":\[{"s":[0-9-]*,"v":[0-9]*' | head -1 | grep -o '[0-9]*$')
echo "── alice attacks {s:$S,v:$VV}"
curl -s -XPOST "$H/action" -H "Authorization: Bearer $AT" \
  -d "{\"game_id\":\"$GID\",\"move\":{\"type\":\"attack\",\"cards\":[{\"s\":$S,\"v\":$VV}]}}"; echo

echo "── seat 1 (bob, defender) view after the attack:"
curl -s "$H/state?game_id=$GID&seat=1"; echo
