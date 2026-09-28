#!/usr/bin/env bash
# Shows bot pacing: a game with two bots progresses OVER TIME (the game-loop
# waits bot_pacing_ms between visible cycles) instead of resolving in one instant.
set -u
H="${1:-http://127.0.0.1:8099}"
# The control plane is packed bytes, not JSON - see ctl.sh / ctl_wire.h.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/ctl.sh"

AT=$(ctl_signup "$H" alice)
GID=$(ctl_create "$H" "$AT")
ctl_meta "$H" "$AT" "$CTL_META_ADD_BOT" "$GID" cordite
ctl_meta "$H" "$AT" "$CTL_META_ADD_BOT" "$GID" firecracker
ctl_meta "$H" "$AT" "$CTL_META_START"   "$GID"
echo "game $GID dealt: alice + cordite + firecracker. Polling each second"
echo "(views are packed now — we watch the status + the packed view's byte length,"
echo " which changes as battles/hands change, proving the board advances over time):"
for i in $(seq 0 8); do
  ST=$(ctl_status "$H" "$GID")
  BYTES=$(curl -s "$H/state?game_id=$GID&seat=0" -H "Authorization: Bearer $AT" | wc -c | tr -d ' ')
  printf '  t=%ss  status=%s  packed_view_bytes=%s\n' "$i" "$ST" "$BYTES"
  sleep 1
done
