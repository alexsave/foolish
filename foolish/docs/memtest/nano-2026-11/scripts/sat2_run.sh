#!/bin/bash
# sat2_run.sh N [keep]: one sat2 run (8 pairs, think 0-1 ms, 25 s, BOARDS 0) on a FRESH server and fresh DB.
# Stops whatever server is on 8080 first, starts cbase serve with the start2.sh env plus the
# RATED/MAX_CONNS/BACKLOG keys of srv.sh, samples its CPU and RSS once a second, runs load_sat2.sh, stops
# the server again unless "keep" is given.
set -u
N=$1; KEEP=${2:-}
R=/Users/alex/Dev/foolish-bench-results
RAW=$R/raw/sat2
L=sat2_r$N
mkdir -p "$RAW"
. /tmp/foolish-bench/secrets.env
B=/Users/alex/Dev/foolish-bench/c_base/build/cbase
# 1. load gate
n=0
load1() { uptime | sed -E 's/.*load averages?: ([0-9.]+).*/\1/'; }
while [ "$(echo "$(load1) > 3" | bc)" = "1" ] && [ $n -lt 60 ]; do sleep 5; n=$((n+1)); done
echo "waited_s=$((n*5))" > "$RAW/$L.gate"
# 2. stop the old server on 8080 (the only process that listens there)
OLD=$(awk -F': ' '/^pid:/{print $2}' /tmp/foolish-bench/SERVER_READY)
if [ -n "$OLD" ] && kill -0 "$OLD" 2>/dev/null; then
  kill "$OLD"; k=0; while kill -0 "$OLD" 2>/dev/null && [ $k -lt 100 ]; do sleep 0.2; k=$((k+1)); done
  kill -9 "$OLD" 2>/dev/null
fi
DB=/tmp/foolish-bench/sat2_r$N.sqlite; rm -f "$DB" "$DB-wal" "$DB-shm" "$DB"-journal*
# 3. fresh server, start2.sh env + srv.sh keys
ulimit -n 65536
( cd /tmp/foolish-bench && exec env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin HOME=/tmp/foolish-bench/home TMPDIR=/tmp/foolish-bench \
  CBASE_LISTEN=127.0.0.1:8080 CBASE_STORE=sqlite CBASE_DB="$DB" \
  CBASE_JWT_SECRET="$CBASE_JWT_SECRET" CBASE_ADMIN_TOKEN="$CBASE_ADMIN_TOKEN" CBASE_PUBLIC_URL=http://127.0.0.1:8080 \
  CBASE_RATED_MIN_AGE=0 CBASE_MAX_CONNS=16384 CBASE_LISTEN_BACKLOG=4096 \
  CBASE_LIMIT_SIGNIN_IP=1000000 CBASE_LIMIT_SIGNIN_NAME=1000000 CBASE_LIMIT_SIGNUP_IP=1000000 CBASE_LIMIT_SIGNUP_ALL=1000000 \
  CBASE_LIMIT_REFRESH_IP=1000000 CBASE_LIMIT_PASSWORD_USER=1000000 CBASE_LIMIT_CREATE_USER=1000000 CBASE_LIMIT_CHAT_USER=1000000 \
  CBASE_LIMIT_BUMP_USER=1000000 CBASE_LIMIT_ID_MISS_IP=1000000 \
  "$B" serve > "$RAW/$L.server.log" 2>&1 ) &
# the subshell's exec makes its pid the server's pid: find the cbase process started just now
sleep 0.5; PID=$(pgrep -n -f "$B serve")
h=0; until curl -s http://127.0.0.1:8080/health | grep -q "^ok" || [ $h -gt 150 ]; do sleep 0.2; h=$((h+1)); done
echo "pid=$PID health=$(curl -s http://127.0.0.1:8080/health)" > "$RAW/$L.start"
# server pid into a file for the sampler and the report
echo "$PID" > "$RAW/$L.pid"
{
  echo "label: $L"; echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"; echo "server_pid: $PID"; echo "db: $DB"
  echo "uptime_before: $(uptime)"; echo "top_cpu_before:"; ps -Ao pcpu,pid,comm -r | head -6
  echo "server_ps_before: $(ps -o pcpu=,rss=,vsz= -p $PID)"
} > "$RAW/$L.env"
# 4. sampler (libproc CPU and RSS, 1 s) and a thread count near the middle of the run
python3 "$R/scripts/sampler.py" "$PID" "$RAW/$L.server.tsv" 1 &
SP=$!
( sleep 12; ps -M -p "$PID" | wc -l | sed 's/^/threads_at_12s_including_header: /' > "$RAW/$L.threads" ) &
START=$(date +%s)
"$R/scripts/load_sat2.sh" run "$L" 127.0.0.1 8 0 1 100000 25000 > "$RAW/$L.out" 2>&1
RC=$?
END=$(date +%s)
touch "$R/scripts/STOP.$PID"; wait $SP 2>/dev/null; rm -f "$R/scripts/STOP.$PID"
{
  echo "exit_code: $RC"; echo "wall_s: $((END-START))"; echo "uptime_after: $(uptime)"
  echo "top_cpu_after:"; ps -Ao pcpu,pid,comm -r | head -6
  echo "server_ps_after: $(ps -o pcpu=,rss=,vsz= -p $PID)"
  echo "threads_after: $(ps -M -p "$PID" | wc -l)"
} >> "$RAW/$L.env"
if [ -z "$KEEP" ]; then
  kill "$PID" 2>/dev/null; k=0; while kill -0 "$PID" 2>/dev/null && [ $k -lt 100 ]; do sleep 0.2; k=$((k+1)); done
  kill -9 "$PID" 2>/dev/null
  echo "server stopped"
fi
echo "done $L rc=$RC wall=$((END-START))s pid=$PID"
