#!/bin/bash
# mono_swap.sh - puts our gateway in lila-ws's place inside the lichess mono container
# (ghcr.io/lichess-org/lila-docker:main: real lila, lila-ws, MongoDB, Redis and Caddy under
# supervisord), or puts lila-ws back. Run from lichess_ws/server after tools/build_linux.sh.
#
#   tools/mono_swap.sh ours      stop supervisord's lila-ws, start dist/lws-linux-arm64 on its port
#   tools/mono_swap.sh theirs    stop ours, start lila-ws again
#   tools/mono_swap.sh status
set -euo pipefail
cd "$(dirname "$0")/.."
NAME=${MONO_NAME:-lichess-mono}
case "${1:-status}" in
  ours)
    docker cp dist/lws-linux-arm64 "$NAME:/usr/local/bin/lws"
    docker exec "$NAME" supervisorctl stop lila-ws
    docker exec "$NAME" sh -c 'pkill -x lws 2>/dev/null || true'
    docker exec -d -e CBASE_DEV=1 -e CBASE_LISTEN=0.0.0.0:9664 -e CBASE_DB=/tmp/lws.sqlite -e CBASE_LOG_LEVEL=info -e CBASE_REQ_WORKERS=4 \
      -e LWS_REDIS_PORT=6379 -e LWS_MONGO_PORT=27017 -e LWS_CSRF_ORIGIN="${LILA_URL:-http://localhost:8080}" \
      "$NAME" sh -c '/usr/local/bin/lws serve > /tmp/lws.log 2>&1'
    sleep 2
    docker exec "$NAME" sh -c 'tail -3 /tmp/lws.log; curl -s http://127.0.0.1:9664/lws/stats'
    ;;
  theirs)
    docker exec "$NAME" sh -c 'pkill -x lws 2>/dev/null || true'
    docker exec "$NAME" supervisorctl start lila-ws
    ;;
  status)
    docker exec "$NAME" supervisorctl status lila-ws || true
    docker exec "$NAME" sh -c 'pgrep -x lws > /dev/null && curl -s http://127.0.0.1:9664/lws/stats || echo "ours: not running"'
    ;;
  *) echo "usage: mono_swap.sh ours|theirs|status"; exit 2 ;;
esac
