# lichess_ws: design and results

The first component of SCOPE.md's ranking, lila-ws, rebuilt on c_base and raced against the real one.
Sections: what was built (1), how it is validated (2), the c_base change and its proof (3), the race (4), what is not built (5), and the next component (6).
Numbers were taken on 2026-10-22 on an Apple M1 (8 cores, 16 GB, macOS 27.0.1) shared with two other agents' builds; the load average is written next to every run.

## 1. What was built

`server/` is a second product on c_base, built the way adventurer is (its Makefile compiles c_base's `core/src` and `providers/local/src` by source; no file of c_base's is copied).
About 3,100 lines of C in `server/src/`:

| file | lines | what it owns |
|---|---|---|
| `hub.c` | 1,000 | the loop-thread half: every socket after its upgrade, every Redis line, every timer; sections named after lila-ws's files (Users, RoundCrowd, History, Fens, Lobby, Lag) |
| `routes.c` | 420 | the request-pool half: lila-ws's endpoints, CSRF, the session cookie, the MongoDB reads and their caches, the upgrade |
| `json_wire.c` | 300 | the browser's JSON frames, read and written; the one file that reads or writes a JSON byte on the socket side |
| `lila_wire.c` | 230 | lila's Redis lines, both directions, and the round-robin lane rule |
| `resp.c` | 300 | a RESP2 pub/sub client over c_base's loop: a subscribing and a publishing connection, reconnect, resubscribe |
| `mongo.c` | 230 | OP_MSG `find` (limit 1) and `insert`, one blocking connection per pool thread |
| `bson.c` | 170 | the BSON writer and reader those need |
| `smap.c` | 110 | the string-keyed hash map (users, rooms, sris) |
| `lws_app.c`, `lws.h` | 200 | the cb_app wiring, the shared types, `/lws/stats` |

The threading is c_base's: a socket's upgrade runs on the request pool, where the MongoDB reads block only that thread (one connection per thread, the game, troll and name caches behind one mutex), and `cb_ws_upgrade` is pure, so the loop takes the socket over once the 101 is written.
From `on_open` on, everything is the loop thread: a frame is parsed, acted on, and its fan-out written into each hearer's send buffer before the next frame is read; the loop flushes once per pass.
The hub holds no lock.

What a socket costs: `lws_conn` is 480 bytes plus the sri, the user and the name it carries; c_base's connection owns no read buffer while idle (the loop reads into one scratch), so a parked socket is the kernel's socket plus about 600 bytes of ours.
The measured number is in section 4.

### 1.1 Behaviour matched

Everything SCOPE.md 3.1 lists, checked by the smoke (`test/smoke.c`, 118 checks, a game played with this test as lila on Redis) and by the drop-in game against real lila (2.2):

- The four socket kinds (site, api, lobby, round play and watch), the query (`sri`, `v`, `flag`), the `lila2` cookie's `sessionId=` and the `security` and `user4` reads, the CSRF origin rule, the per-endpoint credit rate limit with free pings, the 30 s broom.
- The client vocabulary of those sockets, including the old `from`/`to`/`promotion` move form and `drop`, the base-36 move time, the ack and the PING frame with the clock on every move, `startWatching` with its 16-game bound, `talk` to the player or watcher chat (and to the tournament, swiss or simul chat for a game inside one), `flag`, `bye2`, `berserk`, `hold`, `rep`, the lobby's `join` with the anonymous flood rules, the forwards.
- The Redis vocabulary: every `LilaIn` line those sockets produce (with `-`, `+`, commas, the lane hash) and every `LilaOut` line they consume; the `boot` handshake both ways (`room/versions` on lila's boot, `r/versioning-ready`, the critical-message buffer while lila is away after `lila/stop`, `serverRestart` to the players, `emitAllOnline` 15 s after lila is back).
- History: 20 events per game and per room, the exact `getFrom` rule (nothing asked: the newest 5; a version: everything after it when it is all there, nothing when current, `resync` when a hole), the per-socket `skip`, `noDests` and `full` forms by the event's flags and the socket's color.
- Crowds: players and bots counted by color, watchers by user (anonymous watchers are not in `nb`, as lila-ws's `Output.size` is), the 700 ms batch, `r/ons` to lila, the names of up to 10 watchers (the title and name read from `user4` at connect), only sent when changed.
- Fens: the position parsed from a move event the way `Fens.scala`'s regexes do (uci, fen, the integer part of each clock), pushed to watchers and on a new watch, `finish` with the winner.
- Lags: the EWMA of PING/PONG round trips per user (capped 5 s, decaying 0.1), the client-reported lags batched every 947 ms, `r/latency` from lila's pong, the frame lag in centis on each `r/move`.
- Users: `connect/user` on a user's first socket (silent for the lobby), `disconnect/users` batched every 5 s, `tell/users`, `tell/room/user(s)` to that user's sockets in that game, kicks (4010), troll marks live.
- The closes: 1003 for a binary frame, 1008 over the rate limit, 1011 for a message nobody knows, 4010 for lila's stops and kicks, 1001 for the broom.

### 1.2 Deviations, each deliberate

- `tell/sri` forwards carry the client's JSON bytes as received; lila-ws re-serializes the parsed object (`Json.stringify`), which only differs for a client that sent uncanonical JSON.
- A `mlat` value is forwarded as lila wrote it (an integer); lila-ws parses it to a double and may print it differently.
- The sri is capped at 63 bytes (lila-ws accepts any length without a space).
- OAuth bearer auth (the mobile app), `load=true` (the mobile app's full game state through `r/get`), `seenAt` writes, the relation block check on `/watch`, `userTv`, eval cache, friend list, streamers, announce, TV channels, storm, impersonation and the tournament reminder are not served (SCOPE.md 3.1 says which and why).
- The `r/ons` line lila-ws sends is one per 700 ms batch; ours is the same batch rule, but the batch is flushed at 512 dirty games (lila-ws's `groupedWithin(512, 700.millis)`).

## 2. Validation

### 2.1 Tests

`make check`: four unit programs (`unit_json_wire` 79 checks, `unit_lila_wire` 56, `unit_bson` 24, `unit_smap` 27,516) and the smoke (118 checks: the boot handshake, a site socket's pings, a lobby socket's auth and forwards, a game with a player and a watcher and this test as lila on Redis, the resync rules, the fen watchers, the rate limit and the close codes, the stats line).
`make check-asan` runs all of it under ASan and UBSan: clean.
The unit tests also pass on Linux/musl (run inside the Alpine build container).

Every test was mutation-checked (`tools/mutation_check.sh`, 21 mutations plus the room-namespace one by hand): each mutation turns its test red, and the script itself had two lessons worth the file comment: a restored file with its old mtime left the mutated object in place, and make's one-second mtime granularity let a mutation made within the second of the previous build go unbuilt, so the script now drops the object around each mutation.

### 2.2 Drop-in with real lila

`ghcr.io/lichess-org/lila-docker:main` (the lichess mono image: real lila, lila-ws, MongoDB, Redis, Caddy under supervisord, linux/arm64) runs as the container `lichess-mono` on 127.0.0.1:8080.
`tools/mono_swap.sh ours` stops its lila-ws and starts `dist/lws-linux-arm64` (our gateway, statically linked for Linux arm64 by `tools/build_linux.sh`, the same Alpine recipe c_base uses for its Lambda binary) on lila-ws's port inside the container: same Caddy, same lila, same MongoDB, same Redis, no configuration changed.
`tools/dropin.sh` then plays a game the way a browser would: two seeded users log in through lila's `/login`, one challenges the other through lila's API and the other accepts, and `build/dropin` opens both players' WebSockets through Caddy with their real session cookies and sends moves as the browser does.

Result (2026-10-22): real lila logged `Remote socket boot` for our `boot` lines and nothing else about us; the game `ph7bfE0K` was played through our gateway: the crowd frame to both players, white's `e2e4` acked and relayed by lila as `r/ver ... 1 W move ...`, delivered to white without `dests` and to black with them, black's `e7e5` the same way round, a `p` answered `0`, a chat line delivered to the opponent as lila's `message` event, a resignation ending the game with lila's `endData` to the opponent.

The drop-in found one defect the smoke had not: a challenge's id is its game's id, and lila versions the challenge's own events (`tell/room/version <id> ...` on `chal-out`) before the game starts; our one room per id then made the game's room a plain room, which lost the crowd and the dests filtering.
lila-ws keeps `History.room` and `History.round` apart; so do we now (a room that is not a game is keyed with a leading `@`), and the smoke replays the sequence (its mutation: collapsing the namespaces fails 18 checks).

A browser session was not driven (the Chrome extension was not connected in this session); the drop-in game exercises the same HTTP login, the same cookie, the same socket URL, frames and events the browser uses.

## 3. The c_base change and its proof

One change to c_base, in `core/src/net/ws.h` and `server.c`: `cb_ws_handlers` gained an optional `on_pong(ws, data, len, ud)`, called in the PONG branch of `ws_frames` when set.
lichess's round socket times its peer by sending a PING with the clock in it and reading the clock back from the PONG; c_base swallowed pongs.
The four existing initializers of `cb_ws_handlers` (rt_local.c and three tests) were made designated so the new field reads as absent.

`tools/hotpath_proof.sh` compiles two copies of c_base's tree, one with the changed files as origin/main has them, with c_base's own compile command, and compares every function's disassembly:

```
DIFFERS: core/src/net/server.c
  functions: 38 identical, 1 changed (_ws_frames), 0 removed, 0 added
DIFFERS: providers/local/src/realtime/rt_local.c
  functions: 27 identical, 0 changed, 0 removed, 0 added
```

The one changed function differs by exactly the twelve instructions of the new PONG branch (load the handlers pointer, test `on_pong`, call it), a block only a PONG opcode reaches; the TEXT and BIN path's instructions are unchanged, byte for byte.
rt_local.c's code is identical; its `WS_HUB` constant gained a NULL field.
foolish's hot path is as it was.

## 4. The race

Setup (SCOPE.md 3.3): both servers native on this Mac, no Docker between the load generator and either server.
The real lila-ws is f0e2e0e (2026-10-18) built from source with `sbt stage` (Netty's kqueue transport for macOS, as its build picks), on OpenJDK 21.0.12 with the JVM's default flags (4 GB max heap), `ulimit -n 65536`, run as `target/universal/stage/bin/lila-ws -Dhttp.port=9664 -Dredis.uri=redis://127.0.0.1:9679 -Dmongo.uri=mongodb://127.0.0.1:27117/lichess -Dcsrf.origin=http://localhost:9663`.
Ours is `build/lws serve` (clang -O2, the Makefile's flags) with `CBASE_REQ_WORKERS=4` (the pool the MongoDB reads run on) on 9665.
Redis 8 from Homebrew on 9679, MongoDB 7.0 in Docker on 27117 (read by both only at connect time), the same seeded users, sessions and games.
`tools/wsbench` is the load generator and the fake lila (one process on c_base's loop, `tools/wsbench.c`): it plays the browsers and answers every `r/move` on Redis with a 300-byte move event, every `ping` with a `pong`, and lila's boot handshake.
Each race (`tools/race.sh`) warms lila-ws up for 30 s under the same workload first, then runs the measured pass against lila-ws, then against ours, each with 15 s of warm-up traffic before `duration` seconds of measurement.
The server's RSS and CPU time come from `proc_pid_rusage` on its pid over the measured window; "CPU per message" divides that by the client frames sent and received in the window.

RESULTS_TABLES

### 4.1 Reading the numbers

READING

### 4.2 Where lila-ws wins

WHERE_THEIRS_WINS

## 5. Not built, and open gaps

- The sockets and features SCOPE.md 3.1 left out (tournament, simul, study, team, swiss, challenge and racer rooms; eval cache; friend list; bearer auth; mobile `load=true`; `seenAt`; the block check on watch).
- A browser session against the drop-in (the Chrome extension was not available); the protocol-level drop-in game stands in.
- Running on Linux at scale: the Linux binary ran inside the mono container for the drop-in, but the race is macOS; c_base's io_uring and epoll paths are Linux-only and untested here for this product.
- The lila-ws race is against a stock JVM configuration; lichess's production flags are not published in the repo, and a tuned JVM (a bigger young generation, ZGC) would move lila-ws's p99 but not its memory per connection or its CPU per message by much.

## 6. Next

SCOPE.md's order: round (the move engine, clocks, finish and Glicko-2) behind the same `r-in`/`r-out` protocol, raced through Redis against real lila's round; then game storage and export; then puzzles.
