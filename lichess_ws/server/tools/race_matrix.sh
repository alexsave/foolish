#!/bin/bash
# race_matrix.sh - the whole race under the quiet lock: for each JVM flag set, a fresh lila-ws
# (started here, warmed by race.sh's own warm-up pass), then every configuration of race_all.sh
# against it and against ours. Results land in build/race/<flagset>/.
#
#   tools/race_matrix.sh [flagset...]     flagsets: default zgc g1fixed (default: default zgc)
#
# The lock file tells the other agents on this Mac to pause builds and benchmarks; it is removed on
# any exit. ONLY= limits race_all.sh to some configurations (e.g. ONLY="D E").
set -euo pipefail
cd "$(dirname "$0")/.."
S=/private/tmp/claude-501/-Users-alex-Dev-foolish/8f0c400b-6797-4ece-ae66-85b191c32756/scratchpad
LOCK=$S/QUIET.lock
INFRA=$S/infra
STAGE=$S/lichess/lila-ws/target/universal/stage
LILAWS_PORT=${LILAWS_PORT:-9664}
sets=("$@"); [ ${#sets[@]} -eq 0 ] && sets=(default zgc)

jvm_pid=""
cleanup() { rm -f "$LOCK"; [ -n "$jvm_pid" ] && kill "$jvm_pid" 2>/dev/null || true; }
trap cleanup EXIT
echo "$$ lichess_ws race (lila-ws vs c_base gateway), started $(date)" > "$LOCK"

flags_for() {
  case $1 in
    default) echo "" ;;                                                     # G1, max heap 1/4 of RAM (4 GB here)
    zgc)     echo "-J-XX:+UseZGC -J-XX:+ZGenerational -J-Xms2g -J-Xmx2g" ;; # low-pause collector, fixed heap
    g1fixed) echo "-J-XX:+UseG1GC -J-Xms2g -J-Xmx2g -J-XX:MaxGCPauseMillis=20" ;;
    *) echo "unknown flagset $1" >&2; exit 2 ;;
  esac
}

start_lilaws() {   # <flagset>
  local fl; fl=$(flags_for "$1")
  # stop a lila-ws this script or its predecessor started (pid file only, never by pattern)
  if [ -f "$INFRA/lilaws/lila-ws.pid" ]; then kill "$(cat "$INFRA/lilaws/lila-ws.pid")" 2>/dev/null || true; sleep 3; fi
  ulimit -n 65536
  export JAVA_HOME=/opt/homebrew/opt/openjdk@21 PATH=/opt/homebrew/opt/openjdk@21/bin:$PATH
  # shellcheck disable=SC2086
  "$STAGE/bin/lila-ws" $fl -Dhttp.port=$LILAWS_PORT -Dredis.uri=redis://127.0.0.1:9679 \
    "-Dmongo.uri=mongodb://127.0.0.1:27117/lichess?appName=lila-ws" -Dcsrf.origin=http://localhost:9663 \
    > "$INFRA/lilaws/lila-ws-$1.log" 2>&1 &
  local launcher=$!
  for i in $(seq 1 60); do
    jvm_pid=$(pgrep -P "$launcher" java 2>/dev/null || true); [ -z "$jvm_pid" ] && jvm_pid=$launcher
    nc -z 127.0.0.1 "$LILAWS_PORT" 2>/dev/null && break; sleep 1
  done
  nc -z 127.0.0.1 "$LILAWS_PORT" || { echo "lila-ws did not start"; tail "$INFRA/lilaws/lila-ws-$1.log"; exit 1; }
  echo "$jvm_pid" > "$INFRA/lilaws/lila-ws.pid"
  echo "lila-ws ($1: ${fl:-stock flags}) pid $jvm_pid: $(ps -p "$jvm_pid" -o command= | tr ' ' '\n' | grep -E '^-X|^-XX' | tr '\n' ' ')"
}

for s in "${sets[@]}"; do
  start_lilaws "$s"
  RACE_OUT=build/race/$s tools/race_all.sh
done
echo "matrix done $(date)"
