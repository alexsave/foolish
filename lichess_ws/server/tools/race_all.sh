#!/bin/bash
# race_all.sh - the race configurations of ../DESIGN.md, one tools/race.sh each, under the same
# Redis and MongoDB, with the real lila-ws already running natively (its pid in $LILAWS_PID).
# Sizes stay under the Mac's 16k ephemeral ports (one per client socket). Run from lichess_ws/server.
set -euo pipefail
cd "$(dirname "$0")/.."
D=${DURATION:-60}; W=${WARMUP:-15}
ONLY=${ONLY:-A B C D E F}
want() { case " $ONLY " in *" $1 "*) return 0 ;; esac; return 1; }
# A: a quiet site: 200 games with 2 watchers each, 1000 site sockets, 200 lobby sockets (2000 sockets)
want A && tools/race.sh A --games 200 --watchers 2 --site 1000 --lobby 200 --move-ms 2000 --ping-ms 2000 --duration "$D" --warmup "$W"
# B: a busy site: 1000 games, 4000 site, 500 lobby (8500 sockets), moves every 2 s
want B && tools/race.sh B --games 1000 --watchers 2 --site 4000 --lobby 500 --move-ms 2000 --ping-ms 2000 --duration "$D" --warmup "$W"
# C: many sockets: 1500 games, 5000 site, 500 lobby (11500 sockets)
want C && tools/race.sh C --games 1500 --watchers 2 --site 5000 --lobby 500 --move-ms 2000 --ping-ms 2000 --duration "$D" --warmup "$W" --connect-rate 400
# D: the move path at speed: 500 games moving every 200 ms, 2 watchers, no site traffic (2000 sockets, 2500 moves/s offered)
want D && tools/race.sh D --games 500 --watchers 2 --site 0 --lobby 0 --move-ms 200 --ping-ms 2000 --duration "$D" --warmup "$W"
# E: the move path at volume: 2000 games, each player moving every 240 ms (under the round socket's 100 moves per 20 s
# rate limit that both servers enforce), no watchers, no site (4000 sockets, 8300 moves/s offered)
want E && tools/race.sh E --games 2000 --watchers 0 --site 0 --lobby 0 --move-ms 120 --ping-ms 2000 --duration "$D" --warmup "$W"
# F: saturation: 6000 games at the same per-socket rate (12000 sockets, 25000 moves/s offered, more than the
# one-thread load generator can carry, so the server's CPU per message is the number to read)
want F && tools/race.sh F --games 6000 --watchers 0 --site 0 --lobby 0 --move-ms 120 --ping-ms 2000 --duration "$D" --warmup "$W" --connect-rate 400
exit 0
