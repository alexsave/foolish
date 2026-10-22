# c_chess: design and measurements

A chess site designed from scratch for speed on c_base.
This file says what was built, how it is shaped for speed, and what it measures, with the numbers next to the lila-ws race (`lichess_ws/DESIGN.md` on the `lichess-ws` branch) and to lichess where the two are comparable.
Numbers were taken on 2026-10-22 on an Apple M1 (8 cores, 16 GB), shared with other agents' work; every run took the quiet lock and its load average is in `build/bench/*.txt`.

## 1. The shape

One process.
`build/c_chess serve` is a c_base app: c_base's HTTP and WebSocket server, its accounts (bcrypt, signed tokens), its realtime hub (`/ws`, stream frames, rooms), and SQLite as its store.
On top of it, about 2,500 lines of C:

| part | where | what it owns |
|---|---|---|
| the rules | `c/src/chess.c` | bitboards, legal moves (perft-checked), FEN, UCI, SAN, insufficient material, Zobrist keys |
| a game | `c/src/game.c` | the clock (from Black's first move), every ending, offers, the first-move abort |
| ratings | `c/src/glicko2.c`, `rating.c` | Glicko-2 from Glickman's paper, one rating period a game, the site's bounds |
| the wire | `c/src/wire.h` | fixed-layout request and push bodies, sizes pinned by `_Static_assert` |
| the client | `c/src/client.c` | the protocol's client side: the frames it sends, a mirror of the game it shows |
| state | `server/src/state.c` | players, live games, seeks, challenges, the clock thread, the finish (ratings) |
| routes | `server/src/api.c` | sign-up and sign-in, seek, challenge, accept, move, act, the reads |
| journal | `server/src/journal.c` | SQLite for players, game starts and ends, rating points; a flat file for moves |
| the site | `server/src/web.c` | the Next.js export, read into memory at start, served with gzip twins |

The browser runs `c/src/chess.c`, `game.c` and `client.c` compiled to wasm (`c/wasm/web.c`, 24 KB, 10 KB gzipped).
The page's TypeScript moves bytes between the socket and the kernel and draws the kernel's view, read through structgen's generated readers; no rule and no protocol byte is written in TypeScript.
The load generator (`server/tools/chessbench.c`) links the same client, so the bots play over exactly the protocol a person does.

## 2. Why it is fast

- **A move is one small request on an open socket.** The client sends a stream frame `SF_REQ POST /c/move` whose body is `cw_move_req` (game, ply, move: 8 bytes), 30 bytes on the wire with the WebSocket header.
  The server answers with an empty `SF_REPLY` and pushes `cw_state` (36 bytes) to the game's room.
  For comparison, lichess's move goes from the browser as JSON to lila-ws, over Redis to lila, and back as a move event of about 300 bytes of JSON with every legal destination listed.
- **It runs on the loop thread.** `/c/move` is a `CB_ROUTE_INLINE|HOME` route: the hub runs it on the loop that read the frame, the handler touches only memory under one lock, and the push goes into the hearers' buffers in the same pass, flushed once per socket per pass.
- **No legal-move list goes over the wire.** The browser has the rules: it computes legal moves itself, shows a move before the server answers, and the server's push confirms or corrects it.
- **Nothing on the move path waits for the disk.** The move is handed to the journal's writer as a 12-byte record (a memcpy under a mutex); the writer appends a batch with one `write()` every 2 ms and `fsync`s every 100 ms.
- **Clocks cost nothing per move.** One thread holds a min-heap of the games whose clock can end them, one entry per game moved in place on each move, and sleeps until the earliest flag.

## 3. The race: c_chess under its load generator

`tools/bench.sh <label> [chessbench args]`: a fresh server, the bots' accounts kept between runs, one `chessbench` run (one thread, every socket on c_base's loop), 15 to 20 s of warm-up, 60 s measured.
"Round trip" is a bot's move sent to the server's push of that ply arriving on the same socket; "relay" is the same move arriving on the opponent's socket.
The server's CPU is user plus system time over the window, all of its threads.

| config | sockets | think time | moves/s | round trip p50 / p99 / p999 | relay p50 / p99 / p999 | server CPU | CPU per move | RSS end |
|---|---|---|---|---|---|---|---|---|
| A: blitz pace | 2,000 | 1-3 s | 499 | 0.23 / 0.73 / 1.29 ms | 0.28 / 0.85 / 1.49 ms | 3.1% of a core | 62 us | 23 MB |
| B: many sockets | 10,000 | 2-6 s | 1,248 | 0.36 / 1.86 / 5.40 ms | 0.44 / 2.24 / 5.94 ms | 6.7% | 54 us | 50 MB |
| C: flat out | 2,000 | 2-10 ms | 89,593 | 4.6 / 12.0 / 24.2 ms | 4.9 / 13.4 / 26.4 ms | 90.5% | 10.1 us | 123 MB |

Reading it:

- **Throughput: about 90,000 moves a second on under one core.** In C every bot plays as fast as the protocol allows, and the server, including its journal, used 0.9 of a core for it.
  The load generator (one thread doing what 2,000 browsers would) was the limit, not the server.
- **CPU per move falls with load.** At a few hundred moves a second each move pays for waking threads and a kernel round trip of its own (54 to 62 us); at saturation the passes batch and a move costs 10 us.
  The lila-ws race measured 16 to 47 us of CPU per client frame for lila-ws alone, which relays a move and does none of the rules, clock or storage; c_chess's 10 us is the whole stack for a move (one frame in, two to three out).
- **Latency below saturation is sub-millisecond at the median**, through the real socket, the real hub and the real journal.
  At saturation the median is the queue in front of the one-thread load generator.
- **Memory: 3.2 to 5.6 KB per socket** (B: 30 MB for 10,000 sockets), and a live game is 2.6 KB (`cc_live`).
  C's end RSS includes the finished games kept in memory for a minute (480 a second ended).

### 3.1 The profile, and what it changed

`sample` of the server at saturation (first run, 60,000 moves a second at 1.6 cores) put most of the time in SQLite: `pwrite` 6,630 samples, `fsync` 1,318, `pread` 1,018, the WAL's checksums and B-tree; a move was an `INSERT` into `cc_moves`.
Three changes, each re-measured:

1. **Moves to a flat file** (`journal.c`): 12-byte records appended in batches, SQLite only for a game's start and end. 60,000 to 66,000 moves a second, CPU per move 26 to 17 us.
2. **One legality check, not a legal-move list** (`ch_is_legal` makes only the move asked about; `ch_has_legal` stops at the first legal reply): 66,000 to 78,500 moves a second, 17 to 10.5 us.
3. **The clock heap one entry a game** (it added an entry a move and kept the stale ones until their deadline: 75 MB at saturation): 78,500 to 89,600 moves a second, RSS at the end 211 to 123 MB.

What is left at saturation is the kernel: `sendto`, `recvfrom` and `kevent` on the loop thread.

### 3.2 Durability

A move is acknowledged before it is on disk.
A process crash loses at most the moves of the 2 ms gathering window (the smoke kills the server with SIGKILL mid-game and finds the game, its moves and its clocks); a power loss at most the 100 ms between `fsync`s.
lichess's round, for comparison, writes a game in play to MongoDB on a 30 s debounce, and at once only when its status changes.

## 4. The page

Measured by `scratchpad/cc/perf.mjs` (Chrome over CDP, cold cache, the lobby; "ready" is the socket accepted, which on a first visit includes making the guest account, a bcrypt on the server):

| | requests | transferred | first contentful paint | ready |
|---|---|---|---|---|
| c_chess, local | 14 | 172 KB | 88 ms | 240 ms |
| c_chess, 4G (85 ms, 9 Mbit/s, CPU / 4) | 14 | 172 KB | 288 ms | 800 ms |
| lichess.org home, same tool, no throttling | 7 | 232 KB | 411 ms | - |

Of the 172 KB, about 160 KB is the React and Next.js runtime, the framework's floor; the rules and the protocol are 10 KB of wasm, preloaded beside the scripts; the pieces are inlined in the stylesheet (2 KB gzipped).
lichess's number is over the internet from its CDN, so its paint includes real network latency; the bytes are the comparable part.

## 5. World scale

How big lichess is, from what could be found on 2026-10-22: its open database holds about 7.9 billion rated standard games (July 2026), recently about 90 to 100 million a month, so about 3 million rated standard games a day before casual games and variants, so 5 million a day overall is a fair round number; a forum post cites 147,000 concurrent players (January 2023).
Sources: the open database (https://database.lichess.org), lichess forum posts on peak concurrency, a 2026 statistics aggregator (https://voxbooster.com/blog/online-chess-statistics-2026); lichess has not published an official 2025 total.
At 5 to 10 million games a day and an average of 70 plies a game that is 4,000 to 8,000 moves a second on average, perhaps 20,000 at peak, with 100,000 to 150,000 sockets.

- **Moves:** c_chess carried 89,600 moves a second on 0.9 of an M1 core with every bot on the same machine; lichess's peak is under a quarter of that.
- **Sockets:** 150,000 sockets at 3 to 6 KB each is 450 to 900 MB.
- **Games:** 50,000 live games at 2.6 KB each is 130 MB.

So one 8-core box with a few GB holds lichess's live play several times over on these numbers; the reasons to have more than one are redundancy and geography, not load.
What this does not include yet: TLS on the socket (a few microseconds a frame with session keys cached), the engine (section 6, not built), and the cost of real browsers' networks; the load generator here is on loopback.

## 6. Not built yet

Premoves and lag compensation, the live games and leaderboard in the lobby, the engine (Stockfish as child processes for play against the computer, the eval cache, browser-side analysis), puzzles, arenas, chat.
