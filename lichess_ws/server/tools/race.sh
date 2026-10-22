#!/bin/bash
# race.sh - one race: the same wsbench workload against the real lila-ws and against our gateway,
# on this machine, the same Redis and MongoDB, the load average recorded next to every run.
#
#   tools/race.sh <label> [wsbench args...]
#
# Environment: LILAWS_PID (a file holding the real lila-ws's pid; it must be running on LILAWS_PORT,
# default 9664), LWS_PORT (ours, default 9665, started here from a copy of build/lws so a rebuild
# never touches a running image), REDIS_PORT (9679), MONGO_PORT (27117), RACE_OUT (results dir,
# default build/race). The real lila-ws gets a warm-up pass first (the JIT), then each server gets
# the measured pass; the JVM is left warm for the next label.
set -euo pipefail
cd "$(dirname "$0")/.."
label=$1; shift
LILAWS_PORT=${LILAWS_PORT:-9664}; LWS_PORT=${LWS_PORT:-9665}; REDIS_PORT=${REDIS_PORT:-9679}; MONGO_PORT=${MONGO_PORT:-27117}
RACE_OUT=${RACE_OUT:-build/race}; mkdir -p "$RACE_OUT"
INFRA=${INFRA:-/private/tmp/claude-501/-Users-alex-Dev-foolish/8f0c400b-6797-4ece-ae66-85b191c32756/scratchpad/infra}
LILAWS_PID=${LILAWS_PID:-$INFRA/lilaws/lila-ws.pid}
lilaws_pid=$(cat "$LILAWS_PID")
kill -0 "$lilaws_pid" 2>/dev/null || { echo "lila-ws (pid $lilaws_pid) is not running"; exit 1; }

# ours, from a copy of the binary
mkdir -p "$INFRA/lws"
cp build/lws "$INFRA/lws/lws-$label"
CBASE_DEV=1 CBASE_LISTEN=127.0.0.1:$LWS_PORT CBASE_DB="$INFRA/lws/lws-$label.sqlite" CBASE_LOG_LEVEL=warn CBASE_REQ_WORKERS=${LWS_WORKERS:-4} \
  LWS_REDIS_PORT=$REDIS_PORT LWS_MONGO_PORT=$MONGO_PORT LWS_CSRF_ORIGIN=http://localhost:9663 \
  "$INFRA/lws/lws-$label" serve > "$INFRA/lws/lws-$label.log" 2>&1 &
lws_pid=$!
trap 'kill $lws_pid 2>/dev/null || true' EXIT
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$LWS_PORT/lws/stats" > /dev/null && break; sleep 0.2; done
curl -sf "http://127.0.0.1:$LWS_PORT/lws/stats" > /dev/null || { echo "our gateway did not start: $(tail -3 "$INFRA/lws/lws-$label.log")"; exit 1; }

run() {   # <name> <port> <pid> <extra args...>
  local name=$1 port=$2 pid=$3; shift 3
  local out="$RACE_OUT/$label-$name.txt"
  {
    echo "== $label / $name on port $port (pid $pid)"
    echo "before: $(uptime)"
    build/wsbench --port "$port" --redis-port "$REDIS_PORT" --mongo-port "$MONGO_PORT" --pid "$pid" --quiet "$@"
    echo "after:  $(uptime)"
  } | tee "$out"
}

echo "warm-up pass on lila-ws (not recorded)"
build/wsbench --port "$LILAWS_PORT" --redis-port "$REDIS_PORT" --mongo-port "$MONGO_PORT" --quiet "$@" --duration "${WARM_DURATION:-30}" > "$RACE_OUT/$label-lilaws-warmup.txt" 2>&1 || true
sleep 3
run lilaws "$LILAWS_PORT" "$lilaws_pid" "$@"
sleep 3
run lws "$LWS_PORT" "$lws_pid" "$@"
kill $lws_pid; wait $lws_pid 2>/dev/null || true
trap - EXIT
echo "results in $RACE_OUT/$label-*.txt"
