#!/bin/bash
# run.sh - build everything, import the region if needed, and start the server, the generator
# and the website, each in the background with its PID and log recorded under $RUN_DIR
# (default adventurer/data/run). `run.sh stop` stops exactly the PIDs it started.
#
#   adventurer/tools/run.sh            start (or restart) all three
#   adventurer/tools/run.sh stop
#   adventurer/tools/run.sh status
#
# Ports: the server on $ADV_PORT (8711), Next on $WEB_PORT (5711). Generator knobs: $GEN_ARGS
# (default "--users 3000 --rate 5 --threads 2 --backfill-users 40 --backfill-size 400").
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ADV=$(cd "$HERE/.." && pwd)
RUN_DIR=${RUN_DIR:-$ADV/data/run}
ADV_PORT=${ADV_PORT:-8711}
WEB_PORT=${WEB_PORT:-5711}
GRAPH=${GRAPH:-$ADV/data/region.graph}
DB=${DB:-$ADV/data/adventurer.sqlite}
GEN_ARGS=${GEN_ARGS:---users 3000 --rate 5 --threads 2 --backfill-users 40 --backfill-size 400}
mkdir -p "$RUN_DIR"

stop_one() {
    local name=$1 pidfile="$RUN_DIR/$1.pid"
    if [ -f "$pidfile" ]; then
        local pid
        pid=$(cat "$pidfile")
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
            local n=0
            until ! kill -0 "$pid" 2>/dev/null || [ $n -gt 50 ]; do sleep 0.2; n=$((n + 1)); done
            kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
            echo "stopped $name (pid $pid)"
        fi
        rm -f "$pidfile"
    fi
}

status() {
    for name in server generator web; do
        local pidfile="$RUN_DIR/$name.pid"
        if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then echo "$name: running, pid $(cat "$pidfile")"; else echo "$name: not running"; fi
    done
}

case "${1:-start}" in
stop) stop_one generator; stop_one web; stop_one server; exit 0 ;;
status) status; exit 0 ;;
start) ;;
*) echo "usage: run.sh [start|stop|status]"; exit 2 ;;
esac

stop_one generator
stop_one web
stop_one server

make -s -C "$ADV/server" -j8 all
[ -f "$GRAPH" ] || { "$HERE/fetch_osm.sh" && make -s -C "$ADV/server" graph; }
if [ ! -d "$ADV/web/node_modules" ]; then (cd "$ADV/web" && npm install --no-fund --no-audit); fi

(
    cd "$ADV/server"
    CBASE_DEV=1 CBASE_LISTEN=127.0.0.1:$ADV_PORT CBASE_DB="$DB" CBASE_LOG_LEVEL=info ADV_GRAPH="$GRAPH" \
        CBASE_MAX_BODY=2097152 ADV_WORKERS=${ADV_WORKERS:-2} \
        exec build/adventurer serve
) > "$RUN_DIR/server.log" 2>&1 &
echo $! > "$RUN_DIR/server.pid"
n=0
until grep -q "listening on" "$RUN_DIR/server.log" 2>/dev/null || [ $n -gt 300 ]; do sleep 0.1; n=$((n + 1)); done
grep -q "listening on" "$RUN_DIR/server.log" || { echo "the server did not start:"; tail -20 "$RUN_DIR/server.log"; exit 1; }

(cd "$ADV/server" && exec build/adv-gen "$GRAPH" --server 127.0.0.1:$ADV_PORT $GEN_ARGS) > "$RUN_DIR/generator.log" 2>&1 &
echo $! > "$RUN_DIR/generator.pid"

(cd "$ADV/web" && NEXT_PUBLIC_ADV_SERVER="http://127.0.0.1:$ADV_PORT" exec npx next dev -p "$WEB_PORT") > "$RUN_DIR/web.log" 2>&1 &
echo $! > "$RUN_DIR/web.pid"

echo "server    http://127.0.0.1:$ADV_PORT/adv/ops   pid $(cat "$RUN_DIR/server.pid")   log $RUN_DIR/server.log"
echo "generator pid $(cat "$RUN_DIR/generator.pid")   log $RUN_DIR/generator.log"
echo "web       http://127.0.0.1:$WEB_PORT/   pid $(cat "$RUN_DIR/web.pid")   log $RUN_DIR/web.log"
