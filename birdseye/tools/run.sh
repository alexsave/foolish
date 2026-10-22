#!/bin/bash
# run.sh - build everything and start the game server, the birdseye process (the simulator and
# the site's surface) and the website, each in the background with its PID and log recorded under
# $RUN_DIR (default birdseye/data/run). `run.sh stop` stops exactly the PIDs it started.
#
#   birdseye/tools/run.sh            start (or restart) all three
#   birdseye/tools/run.sh stop
#   birdseye/tools/run.sh status
#
# Ports: the game server on $GAME_PORT (8811), birdseye on $BE_PORT (8812), Next on $WEB_PORT
# (5811). BE_MODE picks what birdseye plays: "league" (the default: the server's bots against each
# other at tables of their own, BE_LEAGUE_TABLES at once), "tournament" (the stored tournament goes
# on, or a new one is drawn when none is running; the site shows the last one in every mode), or
# "mixed" (fake humans and some bots).
# The other knobs are the BE_* variables birdseye/server/src/birdseye_app.c lists (BE_PLAYERS,
# BE_TABLES, BE_THREADS, BE_THINK_MS, BE_BOT_SHARE, BE_PODKIDNOY, BE_HOSTILE_EVERY, BE_REMATCH,
# BE_SEED); WEB=0 leaves the site out.
#
# The game server is c_base's `cbase serve` with foolish's app, unchanged, on its own SQLite file,
# in dev mode with throwaway secrets, its rate limits lifted (every fake human shares 127.0.0.1)
# and a week-long token so the sim's sockets outlive an afternoon. Nothing here touches AWS.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
BE=$(cd "$HERE/.." && pwd)
ROOT=$(cd "$BE/.." && pwd)
RUN_DIR=${RUN_DIR:-$BE/data/run}
GAME_PORT=${GAME_PORT:-8811}
BE_PORT=${BE_PORT:-8812}
WEB_PORT=${WEB_PORT:-5811}
WEB=${WEB:-1}
GAME_DB=${GAME_DB:-$BE/data/game.sqlite}
BE_DB=${BE_DB:-$BE/data/birdseye.sqlite}
# Dev-mode secrets: at least 32 characters, used by nothing but these two local processes.
JWT_SECRET=${JWT_SECRET:-birdseye-local-dev-jwt-secret-0123456789abcdef}
ADMIN_TOKEN=${ADMIN_TOKEN:-birdseye-local-dev-admin-token-0123456789abcdef}
mkdir -p "$RUN_DIR" "$BE/data"
# A tournament plays at the kernel's own bots-only pace (300 ms a move) on 16 tables; the league at a
# human's pace (a dev-only think time of 1.5 to 5 s) on 40.
if [ "${BE_MODE:-league}" = tournament ]; then
    THINK_DEFAULT=""; TABLES_DEFAULT=16
else
    THINK_DEFAULT="1500-5000"; TABLES_DEFAULT=40
fi

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
    for name in game birdseye web; do
        local pidfile="$RUN_DIR/$name.pid"
        if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then echo "$name: running, pid $(cat "$pidfile")"; else echo "$name: not running"; fi
    done
}

case "${1:-start}" in
stop) stop_one birdseye; stop_one web; stop_one game; exit 0 ;;
status) status; exit 0 ;;
start) ;;
*) echo "usage: run.sh [start|stop|status]"; exit 2 ;;
esac

stop_one birdseye
stop_one web
stop_one game

make -s -C "$ROOT/c_base" -j4 build/cbase
make -s -C "$BE/server" -j4 all
if [ "$WEB" = 1 ] && [ ! -d "$BE/web/node_modules" ]; then (cd "$BE/web" && npm install --no-fund --no-audit); fi

wait_listening() {
    local log=$1 n=0
    until grep -q "listening on" "$log" 2>/dev/null || [ $n -gt 300 ]; do sleep 0.1; n=$((n + 1)); done
    grep -q "listening on" "$log" || { echo "$2 did not start:"; tail -20 "$log"; exit 1; }
}

(
    cd "$ROOT/c_base"
    CBASE_DEV=1 CBASE_LISTEN=127.0.0.1:$GAME_PORT CBASE_DB="$GAME_DB" CBASE_LOG_LEVEL=${GAME_LOG_LEVEL:-info} \
        CBASE_JWT_SECRET="$JWT_SECRET" CBASE_ADMIN_TOKEN="$ADMIN_TOKEN" CBASE_JWT_TTL=604800 \
        CBASE_RATED_MIN_AGE=0 CBASE_REQ_WORKERS=${GAME_REQ_WORKERS:-2} CBASE_BOT_WORKERS=${GAME_BOT_WORKERS:-2} \
        CBASE_LIMIT_SIGNUP_IP=1000000 CBASE_LIMIT_SIGNUP_ALL=1000000 CBASE_LIMIT_SIGNIN_IP=1000000 \
        CBASE_LIMIT_CREATE_USER=1000000 CBASE_LIMIT_BUMP_USER=1000000 CBASE_LIMIT_SUB_USER=1000000 CBASE_LIMIT_CHAT_USER=1000000 \
        CBASE_MAX_CONNS=16384 CBASE_LISTEN_BACKLOG=4096 CBASE_BOT_SPEEDUP=${GAME_BOT_SPEEDUP:-1} \
        CBASE_BOT_THINK_MS=${GAME_BOT_THINK_MS-$THINK_DEFAULT} \
        exec build/cbase serve
) > "$RUN_DIR/game.log" 2>&1 &
echo $! > "$RUN_DIR/game.pid"
wait_listening "$RUN_DIR/game.log" "the game server"

(
    cd "$BE/server"
    CBASE_DEV=1 CBASE_LISTEN=127.0.0.1:$BE_PORT CBASE_DB="$BE_DB" CBASE_LOG_LEVEL=${BE_LOG_LEVEL:-info} \
        CBASE_JWT_SECRET="$JWT_SECRET" CBASE_JWT_TTL=604800 CBASE_LIMIT_SIGNIN_IP=1000000 CBASE_LIMIT_SUB_USER=1000000 \
        BE_SERVER=127.0.0.1:$GAME_PORT BE_SERVER_PID=$(cat "$RUN_DIR/game.pid") \
        BE_MODE=${BE_MODE:-league} BE_LEAGUE_TABLES=${BE_LEAGUE_TABLES:-$TABLES_DEFAULT} BE_LEAGUE_PER_BOT=${BE_LEAGUE_PER_BOT:-4} BE_ADMIN_TOKEN="$ADMIN_TOKEN" BE_LEAGUE_FILE="${BE_LEAGUE_FILE:-$BE/data/league.bin}" BE_TOURNAMENT_FILE="${BE_TOURNAMENT_FILE:-$BE/data/tournament.bin}" \
        exec build/birdseye serve
) > "$RUN_DIR/birdseye.log" 2>&1 &
echo $! > "$RUN_DIR/birdseye.pid"
wait_listening "$RUN_DIR/birdseye.log" "birdseye"

if [ "$WEB" = 1 ]; then
    # The production build (no dev overlay, no compile on request); WEB_DEV=1 runs `next dev` instead.
    if [ "${WEB_DEV:-0}" = 1 ]; then
        (cd "$BE/web" && NEXT_PUBLIC_BE_SERVER="http://127.0.0.1:$BE_PORT" exec npx next dev -p "$WEB_PORT") > "$RUN_DIR/web.log" 2>&1 &
    else
        (cd "$BE/web" && NEXT_PUBLIC_BE_SERVER="http://127.0.0.1:$BE_PORT" npx next build > "$RUN_DIR/web-build.log" 2>&1) \
            || { echo "next build failed:"; tail -20 "$RUN_DIR/web-build.log"; exit 1; }
        (cd "$BE/web" && exec npx next start -p "$WEB_PORT") > "$RUN_DIR/web.log" 2>&1 &
    fi
    echo $! > "$RUN_DIR/web.pid"
fi

echo "game      http://127.0.0.1:$GAME_PORT/health   pid $(cat "$RUN_DIR/game.pid")   log $RUN_DIR/game.log"
echo "birdseye  http://127.0.0.1:$BE_PORT/be/ops     pid $(cat "$RUN_DIR/birdseye.pid")   log $RUN_DIR/birdseye.log"
[ "$WEB" = 1 ] && echo "web       http://127.0.0.1:$WEB_PORT/   pid $(cat "$RUN_DIR/web.pid")   log $RUN_DIR/web.log"
exit 0
