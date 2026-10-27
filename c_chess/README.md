# c_chess

A chess website built from scratch for speed, on c_base.
One C process holds every game in memory and journals to SQLite; moves travel over c_base's `/ws` socket as fixed-layout binary frames; the browser runs the same C rules and protocol client compiled to WebAssembly, so the page holds no game logic.

| Folder | What it is |
| --- | --- |
| `c/src` | The kernel: chess rules (`chess.c`), a game with its clock and endings (`game.c`), Glicko-2 (`glicko2.c`, `rating.c`), an arena's scoring (`arena.c`), a chat line's rules and the word filter (`chat.c`), PGN export (`pgn.c`), the wire (`wire.h`) and the protocol client (`client.c`) shared by the browser and the load generator. |
| `c/wasm` | The browser's kernel: the client and rules as wasm exports, and the structs the page reads through structgen's generated readers. |
| `server` | The c_base app: state, arenas, chat, routes, pushes, the SQLite journal, the site served from memory; `tools/chessbench.c` is the load generator and house bot. |
| `web` | The Next.js site (a static export the server serves). |

## Build and run

    make -j4                 # build/c_chess and build/chessbench
    make check               # unit tests and the smoke (a real server played over its real socket)
    (cd web && npm ci && npm run build)   # the site, with chess.wasm, into web/out
    CBASE_DEV=1 CBASE_LISTEN=127.0.0.1:8900 CBASE_DB=/tmp/c_chess.sqlite CC_WEB_DIR=web/out build/c_chess serve

The server keeps an Hourly Bullet, Blitz and Rapid arena coming up (`CC_ARENA_SCHEDULE=0` turns that off).
Bots to play against or to fill an arena:

    build/chessbench --port 8900 --bots 2 --forever --tc 300+3 --think 250 --spread 500 --prefix housebot   # seek and play, forever
    build/chessbench --port 8900 --bots 12 --forever --arena 4 --prefix arenabot                           # join arena 4 and play its pairings
    build/chessbench --port 8900 --bots 200 --arena-make 90 --tc 60+0 --duration 60                        # make a 90 s arena, fill it, measure
    build/chessbench --port 8900 --bots 6 --forever --tc 180+2 --think 1500 --spread 3000 --chat            # the bots say gl hf and gg in their games

Any of them takes `--chat`: a bot says "gl hf" when a game starts and "gg" when it ends, and the run counts the lines said, refused and heard.
A player is muted from the console (`CBASE_ADMIN_TOKEN` set): `POST /admin/api/mute` with a `cw_mute_req` (wire.h: the name, then the minutes as a little-endian u32; 0 lifts it), answered in plain text.

## License

c_chess is free software under the GNU General Public License, version 3 or later (GPLv3), because it is meant to link the Stockfish engine (GPLv3) and ships the "cburnett" pieces (GPLv2 or later).
c_base (`../c_base`) and the shared code (`../shared`) are separate works that c_chess builds against; they keep their own terms, and nothing GPL is placed in them.
