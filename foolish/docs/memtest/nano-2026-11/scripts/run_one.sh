#!/bin/bash
# run_one.sh LABEL -- CMD...
# Waits (up to 5 min) for 1-minute load average <= 3, records uptime and top CPU users,
# samples the server process (pid from SERVER_READY) once a second, runs CMD, and writes
# raw/LABEL.out (the tool's verbatim stdout+stderr), raw/LABEL.env (context) and raw/LABEL.server.tsv.
set -u
R=/Users/alex/Dev/foolish-bench-results
LABEL="$1"; shift
[ "$1" = "--" ] && shift
PID=$(awk -F': ' '/^pid:/{print $2}' /tmp/foolish-bench/SERVER_READY)
BASE=$(awk -F': ' '/^base_url:/{print $2}' /tmp/foolish-bench/SERVER_READY)
mkdir -p "$R/raw"
n=0
load1() { uptime | sed -E 's/.*load averages?: ([0-9.]+).*/\1/'; }
while [ "$(echo "$(load1) > 3" | bc)" = "1" ] && [ $n -lt 60 ]; do sleep 5; n=$((n+1)); done
{
  echo "label: $LABEL"
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "uptime: $(uptime)"
  echo "waited_for_load_s: $((n*5))"
  echo "server_pid: $PID  base_url: $BASE"
  echo "server_ps_before: $(ps -o pcpu=,rss=,vsz= -p $PID)"
  echo "top_cpu_before:"; ps -Ao pcpu,pid,comm -r | head -6
  echo "command: $*"
} > "$R/raw/$LABEL.env"
python3 "$R/scripts/sampler.py" "$PID" "$R/raw/$LABEL.server.tsv" 1 &
SP=$!
START=$(date +%s)
"$@" > "$R/raw/$LABEL.out" 2>&1
RC=$?
END=$(date +%s)
touch "$R/scripts/STOP.$PID"; wait $SP 2>/dev/null; rm -f "$R/scripts/STOP.$PID"
{
  echo "exit_code: $RC"
  echo "wall_s: $((END-START))"
  echo "uptime_after: $(uptime)"
  echo "top_cpu_after:"; ps -Ao pcpu,pid,comm -r | head -6
  echo "server_ps_after: $(ps -o pcpu=,rss=,vsz= -p $PID)"
} >> "$R/raw/$LABEL.env"
echo "done $LABEL rc=$RC wall=$((END-START))s"
