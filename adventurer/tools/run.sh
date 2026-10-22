#!/bin/bash
# run.sh - build everything, import the region if needed, and start the server, the generator
# and the website, each in the background with its PID and log recorded under $RUN_DIR
# (default $DATA/run). `run.sh stop` stops exactly the PIDs it started.
#
#   adventurer/tools/run.sh            start (or restart) all three
#   adventurer/tools/run.sh stop
#   adventurer/tools/run.sh status
#
# Knobs, all environment variables:
#   ADV_PORT (8711), WEB_PORT (5711)   the server's and Next's ports
#   GEN_ARGS                           the generator's arguments (default: 20 users whose maps
#                                      visibly grow, no backfill; the load shape is
#                                      "--users 3000 --rate 5 --threads 2 --backfill-users 40 --backfill-size 400")
#   FRESH=1                            move the journal aside (to $DATA/journal-<stamp>/) so every
#                                      user starts with an empty map; the server does not do this
#                                      by itself, and a journal from another graph must not be replayed
#   B (build), DATA (adventurer/data), WEB (adventurer/web)
#                                      a second instance beside the live one: `B=build-exp DATA=../data-exp
#                                      WEB=web-exp ADV_PORT=8712 WEB_PORT=5712 run.sh` builds, imports and
#                                      serves without touching what the live ports run from (the web copy:
#                                      `cp -R web web-exp` minus node_modules, which may be a symlink)
#   RUN_DIR ($DATA/run), GRAPH ($DATA/region.graph), DB ($DATA/adventurer.sqlite), PBF ($DATA/region.osm.pbf)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ADV=$(cd "$HERE/.." && pwd)
B=${B:-build}
DATA=${DATA:-$ADV/data}
mkdir -p "$DATA"
DATA=$(cd "$DATA" && pwd)
WEB=${WEB:-web}
WEB_DIR=$(cd "$ADV" && cd "$WEB" && pwd)
RUN_DIR=${RUN_DIR:-$DATA/run}
ADV_PORT=${ADV_PORT:-8711}
WEB_PORT=${WEB_PORT:-5711}
GRAPH=${GRAPH:-$DATA/region.graph}
DB=${DB:-$DATA/adventurer.sqlite}
PBF=${PBF:-$DATA/region.osm.pbf}
GEN_ARGS=${GEN_ARGS:---users 20 --rate 5 --threads 2 --backfill-users 0}
FRESH=${FRESH:-0}
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

# The decoder the web copy reads is written by this build, so a wire change reaches the pages this
# instance serves and no other's.
make -s -C "$ADV/server" -j4 B="$B" WIRE_TS="$WEB_DIR/src/gen/wire.ts" all
if [ ! -f "$GRAPH" ]; then
    [ -f "$PBF" ] || DATA="$DATA" "$HERE/fetch_osm.sh"
    make -s -C "$ADV/server" B="$B" DATA="$DATA" PBF="$PBF" GRAPH="$GRAPH" graph
fi
if [ ! -d "$WEB_DIR/node_modules" ]; then (cd "$WEB_DIR" && npm install --no-fund --no-audit); fi

if [ "$FRESH" = 1 ] && [ -e "$DB" ]; then
    aside="$DATA/journal-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$aside"
    mv "$DB" "$aside/"
    for f in "$DB-wal" "$DB-shm" "$DB.serve-lock"; do [ -e "$f" ] && mv "$f" "$aside/" || true; done
    echo "journal moved aside to $aside"
fi

(
    cd "$ADV/server"
    CBASE_DEV=1 CBASE_LISTEN=127.0.0.1:$ADV_PORT CBASE_DB="$DB" CBASE_LOG_LEVEL=info ADV_GRAPH="$GRAPH" \
        CBASE_MAX_BODY=2097152 ADV_WORKERS=${ADV_WORKERS:-2} \
        exec "$B/adventurer" serve
) > "$RUN_DIR/server.log" 2>&1 &
echo $! > "$RUN_DIR/server.pid"
n=0
until grep -q "listening on" "$RUN_DIR/server.log" 2>/dev/null || [ $n -gt 300 ]; do sleep 0.1; n=$((n + 1)); done
grep -q "listening on" "$RUN_DIR/server.log" || { echo "the server did not start:"; tail -20 "$RUN_DIR/server.log"; exit 1; }

(cd "$ADV/server" && exec "$B/adv-gen" "$GRAPH" --server 127.0.0.1:$ADV_PORT $GEN_ARGS) > "$RUN_DIR/generator.log" 2>&1 &
echo $! > "$RUN_DIR/generator.pid"

(cd "$WEB_DIR" && NEXT_PUBLIC_ADV_SERVER="http://127.0.0.1:$ADV_PORT" exec npx next dev -p "$WEB_PORT") > "$RUN_DIR/web.log" 2>&1 &
echo $! > "$RUN_DIR/web.pid"

echo "server    http://127.0.0.1:$ADV_PORT/adv/ops   pid $(cat "$RUN_DIR/server.pid")   log $RUN_DIR/server.log"
echo "generator pid $(cat "$RUN_DIR/generator.pid")   log $RUN_DIR/generator.log"
echo "web       http://127.0.0.1:$WEB_PORT/   pid $(cat "$RUN_DIR/web.pid")   log $RUN_DIR/web.log"
