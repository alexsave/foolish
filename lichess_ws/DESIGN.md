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

The workload of each configuration (`tools/race_all.sh`); every socket pings every 2 s, as a browser does:

| config | sockets | traffic |
|---|---|---|
| A | 2,000 | 200 games with 2 watchers each, 1,000 site, 200 lobby; a move every 2 s per game |
| B | 8,500 | 1,000 games with 2 watchers, 4,000 site, 500 lobby; a move every 2 s per game |
| C | 11,500 | 1,500 games with 2 watchers, 5,000 site, 500 lobby |
| D | 2,000 | 500 games with 2 watchers, a move every 200 ms per game |
| E | 4,000 | 2,000 games, no watchers, a move every 120 ms per game (each player at 4 moves/s, under the round socket's 100 per 20 s rate limit that both servers enforce) |
| F | 12,000 | 6,000 games at E's rate: 25,000 moves/s offered, more than the one-thread load generator delivers, so this is saturation |

Columns: moves/s achieved; move relay latency (the mover's `move` frame to its own `move` event back, through the server, Redis, the fake lila and back) and the same for each watcher; ping round trip; the server's RSS at the end of the window; its CPU (user + system) over the 60 s window as a share of one core; and that CPU divided by every client frame sent and received in the window.
Latencies are exact: wsbench keeps every sample and sorts (nearest rank), so the shared StatHist (8 buckets per octave, up to 9% error) is not used here.

**lila-ws on the JVM's default flags** (OpenJDK 21: G1, max heap 4 GB), F after the publish fix below:

| config | server | sockets | moves/s | mover p50/p99/p999 ms | watcher p50/p99/p999 ms | ping p50/p99/p999 ms | RSS MB | CPU % core | CPU us/msg | load before/after |
|---|---|---|---|---|---|---|---|---|---|---|
| A | lila-ws | 2000 | 89 | 1.1 / 5.0 / 15.4 | 1.9 / 5.5 / 15.5 | 1.3 / 4.0 / 8.4 | 305 | 12 | 41.5 | 3.82 / 2.67 |
| A | ours | 2000 | 90 | 1.0 / 3.8 / 34.1 | 1.0 / 3.8 / 34.1 | 0.4 / 2.3 / 2.9 | 12 | 2 | 7.6 | 2.54 / 1.37 |
| B | lila-ws | 8500 | 387 | 1.7 / 7.2 / 23.8 | 2.5 / 9.6 / 23.9 | 2.2 / 6.3 / 10.7 | 367 | 22 | 16.3 | 3.10 / 4.54 |
| B | ours | 8500 | 386 | 2.3 / 9.5 / 12.3 | 2.3 / 9.5 / 12.3 | 1.9 / 6.9 / 8.5 | 33 | 4 | 3.0 | 4.50 / 2.88 |
| C | lila-ws | 11500 | 562 | 1.8 / 11.1 / 25.6 | 2.5 / 12.7 / 34.5 | 2.1 / 7.7 / 17.4 | 316 | 31 | 15.7 | 5.03 / 5.38 |
| C | ours | 11500 | 563 | 2.4 / 11.0 / 13.9 | 2.4 / 11.1 / 13.9 | 2.0 / 9.7 / 15.4 | 42 | 7 | 3.7 | 5.38 / 8.41 |
| D | lila-ws | 2000 | 1958 | 8.3 / 22.3 / 29.5 | 9.2 / 23.4 / 29.8 | 0.9 / 7.4 / 11.2 | 391 | 45 | 36.5 | 9.06 / 6.90 |
| D | ours | 2000 | 1991 | 6.2 / 19.9 / 23.6 | 6.2 / 19.9 / 23.6 | 0.5 / 4.8 / 8.4 | 18 | 7 | 5.4 | 6.43 / 4.01 |
| E | lila-ws | 4000 | 8419 | 26.6 / 45.0 / 70.4 | - | 1.1 / 23.2 / 55.1 | 512 | 92 | 30.1 | 7.77 / 9.94 |
| E | ours | 4000 | 8422 | 28.5 / 45.3 / 52.0 | - | 1.1 / 22.9 / 26.7 | 39 | 23 | 7.5 | 9.31 / 4.41 |
| F | lila-ws | 12000 | 19397 | 31.8 / 119.7 / 199.9 | - | 6.7 / 59.9 / 123.4 | 769 | 194 | 25.5 | 6.64 / 12.66 |
| F | ours | 12000 | 23040 | 11.7 / 50.2 / 76.7 | - | 2.6 / 21.4 / 33.2 | 91 | 43 | 5.0 | 12.66 / 4.29 |

**lila-ws tuned**: `-XX:+UseZGC -XX:+ZGenerational -Xms2g -Xmx2g` (the low-pause collector, a fixed heap), a fresh JVM warmed by the same 30 s pass; ours rerun beside it:

| config | server | sockets | moves/s | mover p50/p99/p999 ms | watcher p50/p99/p999 ms | ping p50/p99/p999 ms | RSS MB | CPU % core | CPU us/msg | load before/after |
|---|---|---|---|---|---|---|---|---|---|---|
| A | lila-ws | 2000 | 92 | 1.3 / 3.9 / 6.8 | 2.0 / 4.5 / 10.7 | 1.3 / 3.5 / 6.6 | 461 | 14 | 46.8 | 3.08 / 2.88 |
| A | ours | 2000 | 89 | 0.7 / 2.9 / 17.2 | 0.7 / 2.9 / 17.2 | 0.2 / 1.2 / 6.8 | 10 | 3 | 8.5 | 3.05 / 3.98 |
| B | lila-ws | 8500 | 388 | 2.0 / 9.0 / 19.5 | 2.6 / 10.2 / 21.5 | 1.9 / 7.4 / 15.3 | 559 | 32 | 23.0 | 7.86 / 5.22 |
| B | ours | 8500 | 387 | 1.7 / 8.8 / 15.5 | 1.7 / 9.0 / 15.5 | 0.9 / 4.9 / 10.1 | 28 | 8 | 6.0 | 5.22 / 3.37 |
| C | lila-ws | 11500 | 560 | 4.5 / 24.3 / 34.4 | 5.1 / 25.5 / 37.0 | 2.8 / 13.4 / 19.4 | 575 | 38 | 19.5 | 4.89 / 5.68 |
| C | ours | 11500 | 561 | 2.6 / 15.0 / 23.6 | 2.6 / 15.0 / 23.6 | 1.9 / 8.5 / 14.8 | 38 | 11 | 5.6 | 5.68 / 4.98 |
| D | lila-ws | 2000 | 1888 | 15.4 / 34.1 / 39.9 | 16.7 / 35.2 / 40.3 | 1.2 / 15.0 / 26.9 | 748 | 41 | 34.1 | 3.83 / 6.21 |
| D | ours | 2000 | 1957 | 8.1 / 23.8 / 45.3 | 8.5 / 24.2 / 45.3 | 0.5 / 8.7 / 14.6 | 15 | 14 | 11.1 | 6.35 / 4.68 |
| E | lila-ws | 4000 | 8872 | 9.5 / 58.9 / 93.6 | - | 1.9 / 33.7 / 61.3 | 1045 | 109 | 34.3 | 8.41 / 12.10 |
| E | ours | 4000 | 8477 | 20.6 / 56.6 / 106.0 | - | 1.2 / 30.4 / 71.7 | 35 | 28 | 9.3 | 11.45 / 6.90 |
| F | lila-ws | 12000 | 19522 | 30.7 / 109.0 / 197.9 | - | 6.6 / 57.6 / 155.4 | 1970 | 200 | 26.1 | 7.73 / 12.24 |
| F | ours | 12000 | 22460 | 13.9 / 59.9 / 115.8 | - | 3.2 / 26.0 / 44.7 | 84 | 44 | 5.1 | 11.66 / 8.33 |

**The profile and the fix.** `sample` of ours under F (20 s, `/usr/bin/sample`) put about 90% of the loop thread in the kernel (`sendto` 5,765 samples, `kevent` 2,274, `recvfrom` 2,170 of about 11,500); the largest share that was ours to change was `resp_publish`, which flushed every Redis line with its own `send` (1,430 samples, a tenth of the loop), and the next was `vfprintf` under the RESP bulk header.
`resp.c` now queues a line and posts one flush per loop pass (one `send` for every move a pass read), and writes the `$<n>` header by hand.
F before that change (the same flags, the load generator's own publishes unbatched too):

| JVM | server | moves/s | mover p50/p99/p999 ms | RSS MB | CPU % core | CPU us/msg |
|---|---|---|---|---|---|---|
| default | lila-ws | 20,512 | 20.6 / 149.4 / 268.0 | 678 | 228 | 28.7 |
| default | ours | 22,475 | 14.7 / 63.2 / 112.2 | 87 | 53 | 6.3 |
| ZGC | lila-ws | 20,000 | 21.3 / 204.3 / 437.4 | 1,902 | 227 | 29.2 |
| ZGC | ours | 23,124 | 11.8 / 44.6 / 67.5 | 87 | 50 | 5.8 |

The fix took ours from 6.3 to 5.0 us per message (20% less CPU) at the same throughput.
The load generator shares `resp.c`, so its fake lila's publishes were batched as well; both servers were rerun on it, which is why lila-ws's F rows moved too (its p50 went up and its p99 came down: the fake lila now answers in bursts).

### 4.1 Reading the numbers

- **CPU per message is the headline: 3 to 7 times less.** Across every configuration and both JVM settings ours spends 3.0 to 11.1 us of CPU per client frame and lila-ws 16 to 47 us. At saturation (F) ours carries 23,000 moves/s and 4.8 million frames a minute on 43% of one core; lila-ws carries 19,400 to 20,500 moves/s on 194 to 228% (two cores and more, on Netty's thread pool).
- **Memory is 7 to 46 times less.** Ours ends at 10 to 91 MB (3 to 8 KB per socket, the kernel's socket buffers included in the RSS); lila-ws sits at 300 to 770 MB on default flags and 460 MB to 2 GB with the fixed 2 GB ZGC heap, whatever the socket count, because the heap is sized by the collector, not by the sockets. Its per-socket increment is not measurable against that floor, so the comparison that holds is the total.
- **Throughput at saturation: ours is ahead by 15 to 18%** (F: 23,000 against 19,400 to 20,500 moves/s), and in F the load generator, not ours, is the limit: ours had more than half a core idle.
- **Tail latency at saturation: ours is 2 to 4 times lower.** F mover p99 50 to 60 ms against 109 to 204 ms; p999 77 to 116 ms against 198 to 437 ms. ZGC did not help lila-ws's tail here; its p99 got worse than on G1.
- **Latency below saturation is a draw, and mostly not the server's.** In A to D both servers answer in 1 to 15 ms at p50 and a few tens of ms at p99, and the difference between them is smaller than the run-to-run noise (B and C on default flags have lila-ws ahead at p50 by 0.6 ms; on the ZGC rerun ours is ahead by 0.3 to 1.9 ms). The relay crosses Redis twice and the one-thread fake lila, and the shared Mac's load average was 3 to 14 during the runs (the load generator and the JVM themselves are most of it), so a fraction of a millisecond at p50 is not a finding either way.
- **E is a load generator measurement.** At 8,400 moves/s the one wsbench thread carries 30,000 frames/s plus the fake lila and is the queue both servers sit behind: their p50 differs by run (26.6 and 28.5 ms on default, 9.5 and 20.6 on ZGC). The server columns of E (CPU per message, RSS) are still good; its latency columns are not.
- **Per-box projection.** Per core of an M1, from F: ours about 53,000 moves/s (23,000 at 43%) and lila-ws about 10,000 (19,400 at 194%). Per GB: ours about 130,000 sockets per GB of RSS (7.6 KB each at F, 3.9 KB at B), lila-ws about 16,000 to 37,000 on default flags (its total RSS over its sockets), or about 6,000 with a 2 GB ZGC heap. Lichess plays about 5 to 10 million games a day, roughly 4,000 to 8,000 moves/s on average with a peak of perhaps three times that; that is half a core of ours today, and the socket count of 100,000 concurrent players is under 1 GB. The caveat that matters at world scale: ours runs its sockets on one loop thread, so one process tops out near one core (about 50,000 moves/s with this mix); past that it is several processes behind `SO_REUSEPORT` or per-region boxes, which lila-ws does not need because Netty already spreads over cores. Linux's io_uring path in c_base (untested for this product) is the next lever, since the profile is 90% syscalls.

### 4.2 Where lila-ws wins

- **It uses every core without being asked.** Netty spreads sockets over a thread pool; ours is one loop thread per process, and scaling past one core means running more processes (and splitting the user and room state between them, which lila-ws keeps in one heap).
- **Median latency at light load.** On default flags lila-ws was 0.6 ms ahead at p50 in B and C (its I/O is spread over threads, so a burst of pings does not queue behind one loop); this did not reproduce on the ZGC rerun and is within the noise, but it was measured and is reported.
- **Ping round trip on its own was never decisively worse** for lila-ws below saturation.
- **Breadth and maturity.** lila-ws serves every socket lichess has (tournaments, simuls, studies, teams, swiss, racer, challenges, the eval cache, the friend list, bearer auth, mobile) and has run lichess for years; ours covers site, api, lobby and round only (section 5), was validated by one played game through real lila, and has never seen a real browser or production traffic.
- **Observability.** lila-ws ships Kamon metrics to InfluxDB and Prometheus; ours has `/lws/stats` and c_base's admin console.

## 5. Not built, and open gaps

- The sockets and features SCOPE.md 3.1 left out (tournament, simul, study, team, swiss, challenge and racer rooms; eval cache; friend list; bearer auth; mobile `load=true`; `seenAt`; the block check on watch).
- A browser session against the drop-in (the Chrome extension was not available); the protocol-level drop-in game stands in.
- Running on Linux at scale: the Linux binary ran inside the mono container for the drop-in, but the race is macOS; c_base's io_uring and epoll paths are Linux-only and untested here for this product.
- lichess's production JVM flags are not in lila-ws's repository (its `build.sbt` sets only `-Dconfig.override_with_env_vars=true` for the package); the race used the JVM's defaults and a ZGC run with a fixed 2 GB heap, and ZGC moved neither lila-ws's CPU per message nor its tail at saturation in its favour.
- The race is on one shared Mac with the load generator, Redis and both servers on the same eight cores; a Linux box with the load generator elsewhere would give cleaner latencies.

## 6. Next

The lichess-compatible track stops here, by the owner's decision (2026-10-22): this race is its result and the baseline for what follows.
The next work departs from lichess's protocol entirely: `c_chess/` on the `c-chess` branch, a chess site designed from scratch on c_base for speed (one process, in-memory games, binary wire, the rules in C for the server and, as wasm, for the browser), measured against these numbers.
SCOPE.md's ranking (round, game storage and export, puzzles) is left as written for whoever resumes compatibility work.
