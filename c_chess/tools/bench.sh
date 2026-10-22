#!/bin/bash
# bench.sh - c_chess under its load generator, under the quiet lock: a fresh server (the accounts
# kept in BENCH_DB between runs), one chessbench run per configuration, the load average beside each.
#
#   tools/bench.sh <label> [chessbench args...]      (from c_chess/; results in build/bench/<label>.txt)
#
# Environment: PORT (8901), BENCH_DB (the server's database, kept: the bots' accounts live there),
# ACCOUNTS (the bots' cached sessions), PROFILE=1 (also `sample` the server for 20 s mid-run).
set -euo pipefail
cd "$(dirname "$0")/.."
label=$1; shift
S=/private/tmp/claude-501/-Users-alex-Dev-foolish/8f0c400b-6797-4ece-ae66-85b191c32756/scratchpad
LOCK=$S/QUIET.lock
PORT=${PORT:-8901}
BENCH_DB=${BENCH_DB:-$S/cc/bench.sqlite}
ACCOUNTS=${ACCOUNTS:-$S/cc/bench_accounts.bin}
mkdir -p build/bench
server=""
cleanup() { rm -f "$LOCK"; [ -n "$server" ] && kill "$server" 2>/dev/null || true; }
trap cleanup EXIT
[ "${NOLOCK:-0}" = 1 ] || echo "$$ c_chess bench $label, started $(date)" > "$LOCK"
cp build/c_chess "$S/cc/c_chess-bench"
CBASE_DEV=1 CBASE_LISTEN=127.0.0.1:$PORT CBASE_DB=$BENCH_DB CBASE_JWT_SECRET=bench-secret-bench-secret-bench-0123 CBASE_JWT_TTL=604800 \
  CBASE_ADMIN_TOKEN=bench-admin-token-0123456789abcdef0123 CBASE_LOG_LEVEL=warn CBASE_REQ_WORKERS=${WORKERS:-4} CBASE_MAX_CONNS=40000 \
  "$S/cc/c_chess-bench" serve > "$S/cc/bench-server.log" 2>&1 &
server=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" > /dev/null && break; sleep 0.2; done
out=build/bench/$label.txt
{
  echo "== $label"
  echo "before: $(uptime)"
  if [ "${PROFILE:-0}" = 1 ]; then
    ( sleep "${PROFILE_AT:-40}"; /usr/bin/sample "$server" 20 -file "build/bench/$label.sample.txt" > /dev/null 2>&1 ) &
  fi
  build/chessbench --port "$PORT" --pid "$server" --accounts "$ACCOUNTS" --quiet "$@"
  echo "after:  $(uptime)"
} | tee "$out"
