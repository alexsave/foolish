#!/usr/bin/env bash
# Shows bot pacing: a game with two bots progresses OVER TIME (the game-loop
# waits bot_pacing_ms between visible cycles) instead of resolving in one instant.
set -u
H="${1:-http://127.0.0.1:8099}"
tok(){ grep -o "\"$1\":\"[^\"]*\"" | head -1 | cut -d'"' -f4; }
num(){ grep -o "\"$1\":[0-9-]*" | head -1 | grep -o '[0-9-]*$' || true; }

AT=$(curl -s -XPOST "$H/auth/signup" -d '{"username":"alice"}' | tok token)
GID=$(curl -s -XPOST "$H/create" -H "Authorization: Bearer $AT" | tok game_id)
curl -s -XPOST "$H/meta" -H "Authorization: Bearer $AT" -d "{\"type\":\"add-bot\",\"game_id\":\"$GID\",\"strategy\":\"cordite\"}"    >/dev/null
curl -s -XPOST "$H/meta" -H "Authorization: Bearer $AT" -d "{\"type\":\"add-bot\",\"game_id\":\"$GID\",\"strategy\":\"firecracker\"}">/dev/null
curl -s -XPOST "$H/meta" -H "Authorization: Bearer $AT" -d "{\"type\":\"start\",\"game_id\":\"$GID\"}"                              >/dev/null
echo "game $GID dealt: alice + cordite + firecracker. Polling /state each second:"
for i in $(seq 0 8); do
  V=$(curl -s "$H/state?game_id=$GID&seat=0")
  DECK=$(echo "$V" | num deckCount); DISC=$(echo "$V" | num discardCount)
  NB=$(echo "$V" | grep -o '"attack"' | wc -l | tr -d ' '); ST=$(echo "$V" | num status)
  printf '  t=%ss  deck=%s discard=%s battles=%s status=%s\n' "$i" "$DECK" "$DISC" "$NB" "$ST"
  sleep 1
done
