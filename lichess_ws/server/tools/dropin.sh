#!/bin/bash
# dropin.sh - the drop-in check against the real lila (../DESIGN.md): with our gateway running in
# lila-ws's place inside the lichess mono container (tools/mono_swap.sh), two seeded users log in
# through lila's own HTTP, one challenges the other through lila's API, the other accepts, and
# build/dropin plays the first moves over the two players' WebSockets through Caddy. Run from
# lichess_ws/server. Needs the mono container on MONO_PORT (default 8080) and build/dropin.
set -euo pipefail
cd "$(dirname "$0")/.."
PORT=${MONO_PORT:-8080}
C=http://localhost:$PORT
A=${USER_A:-ana}; B=${USER_B:-lola}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

login() {   # <user> -> the lila2 cookie value
  curl -s -c "$tmp/$1.cookies" -X POST "$C/login" -H "Origin: $C" -H "X-Requested-With: XMLHttpRequest" -H "Accept: application/json" \
    -d "username=$1&password=${PASSWORD:-password}" -o /dev/null
  awk '$6 == "lila2" {print $7}' "$tmp/$1.cookies"
}
ca=$(login "$A"); cb=$(login "$B")
[ -n "$ca" ] && [ -n "$cb" ] || { echo "login failed (is the mono container up on $PORT?)"; exit 1; }
cid=$(curl -s -X POST "$C/api/challenge/$B" -H "Authorization: Bearer lip_$A" -d "clock.limit=600&clock.increment=0&color=white&rated=false" |
      python3 -c 'import sys,json; print(json.load(sys.stdin)["id"])')
curl -s -X POST "$C/api/challenge/$cid/accept" -H "Authorization: Bearer lip_$B" > /dev/null
sleep 1
fa=$(curl -s "$C/api/account/playing" -H "Authorization: Bearer lip_$A" | python3 -c "import sys,json; print([g['fullId'] for g in json.load(sys.stdin)['nowPlaying'] if g['gameId']=='$cid'][0])")
fb=$(curl -s "$C/api/account/playing" -H "Authorization: Bearer lip_$B" | python3 -c "import sys,json; print([g['fullId'] for g in json.load(sys.stdin)['nowPlaying'] if g['gameId']=='$cid'][0])")
echo "game $cid: $A plays $fa, $B plays $fb"
build/dropin --port "$PORT" --full-a "$fa" --cookie-a "$ca" --full-b "$fb" --cookie-b "$cb"
