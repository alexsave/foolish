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

## 3.3 With TLS

c_base now terminates TLS itself (`c_base/core/src/net/tls.c`, OpenSSL 3 on the loop thread, non-blocking): set `CBASE_TLS_CERT` and `CBASE_TLS_KEY` and the listener speaks only TLS 1.2+ (ECDHE with AES-GCM or ChaCha20 on 1.2; TLS 1.3 preferring AES-128-GCM), with session tickets for resumption and close_notify on every close (a client that sees a bare EOF throws its session away).
With it off nothing changes in foolish's code: the per-function disassembly proof (`tools/hotpath_proof.sh`) finds every function of `server.c` identical but the six that gained their TLS branch (a load and a branch on the connection's `tls` pointer in `flush_now` and `conn_io`, one on the batched path of `flush_dirty`, the connection's creation in `on_accept`, its close_notify in `conn_close`, its release in `conn_free`).
The page uses it the WebSocket way: one sign-in over HTTPS, then one `wss` socket that carries every action and every read (seeks, moves, offers, profiles, the leaderboard), so a session costs one TLS handshake, not one per request.

The same three loads with and without TLS, back to back under the quiet lock (the load generator speaks TLS too, on its one thread):

| config | | moves/s | round trip p50 / p99 | server CPU per move | RSS per socket |
|---|---|---|---|---|---|
| A: 2,000 sockets, blitz pace | plain | 499 | 0.18 / 0.74 ms | 45.6 us | 4.5 KB |
| | TLS | 499 | 0.30 / 1.06 ms | 84.2 us | 22.5 KB |
| B: 10,000 sockets | plain | 1,250 | 0.32 / 2.71 ms | 42.6 us | 3.0 KB |
| | TLS | 1,247 | 0.44 / 3.75 ms | 54.4 us | 22.8 KB |
| C: flat out | plain | 71,594 | 6.0 / 18.0 ms | 13.2 us | |
| | TLS | 48,866 | 11.5 / 39.7 ms | 16.8 us | |

Connections, one request each (`chessbench --handshakes`: connect, handshake, `GET /health`, close), from 6 threads, the server's CPU per connection:

| | server CPU per connection | connections per second per core |
|---|---|---|
| plaintext | 13.4 us | 74,660 |
| TLS 1.3, full handshake, X25519MLKEM768 (OpenSSL's default and what browsers offer) | 300 us | 3,336 |
| TLS 1.3, resumed (PSK with a fresh key exchange) | 237 us | 4,220 |
| TLS 1.3, full, X25519 only | 227 us | 4,406 |
| TLS 1.3, resumed, X25519 only | 176 us | 5,699 |

Reading it, honestly:

- **The bulk cost is small, the handshake is not.** Per move TLS adds 3.6 us at saturation (13.2 to 16.8, +27%) and 10 to 40 us at low rates, where each move's few records pay OpenSSL's per-record overhead; the AES itself (`aes_v8_encrypt`) is under 1% of the profile, the rest is OpenSSL's record layer and its allocations.
  A connection costs 230 to 300 us of server CPU against 13 us in plaintext: the profile is the key exchange (ML-KEM's NTT and Keccak, X25519's field arithmetic).
  A resumed handshake saves only a fifth, because TLS 1.3 resumption still does a fresh key exchange for forward secrecy.
  That is why the site's one-socket pattern matters: a player pays one handshake per visit, so at lichess's scale (a few thousand new connections a second at peak) TLS is about one core of handshakes, and the moves themselves barely notice it.
- **Flat-out throughput fell from 71,600 to 48,900 moves a second** in this run, but that is the one-thread load generator doing the client side of every TLS record for 2,000 sockets; the server stayed under one core (82%) and its CPU per move rose 27%.
- **Memory is the real cost: about 18 KB more per socket** (OpenSSL's connection, its record layer and its buffers), so 10,000 sockets take 230 MB instead of 30 MB. At 150,000 sockets that is 3.4 GB on one box; shrinking OpenSSL's per-connection state (or a lighter TLS stack) is the next lever if memory, not CPU, becomes the limit.
- The latency at low load rises by about 0.1 ms at the median and 0.3 to 1 ms at p99.

## 3.4 Stockfish on c_base

**The choice: Stockfish as child processes speaking UCI over pipes** (`server/src/engine.c`), a fixed pool of them run by one manager thread with `poll()`, in front of them a bounded queue (refusal past it, not growth), each search's answer handed back under the state's lock (`server/src/ai.c`).
Built from the official sf_19 tag with its own makefile (scratchpad, not this repository): 1.43 million nodes a second on one M1 performance core.

Why not in-process, measured:

- **Memory does not favour it.** Stockfish 19 keeps its 110 MB network in system-wide shared memory, so a second process does not load a second copy: `vmmap` of a running engine shows the network as a clean shared mapping and about 45 MB of its own dirty memory (16 MB of it the hash table, the rest search state; on macOS a further 110 MB of freed malloc pages stay counted until the system needs them). In-process would share the same network and still need a hash and search state per searcher.
- **Isolation favours the process.** A crash or a stuck search is a restarted child; in-process it is the server. The pipe costs a few microseconds per search, against searches of milliseconds to seconds.
- **Licensing is not a factor** (Alex: c_chess may be GPLv3); a subprocess keeps c_chess's own code separate anyway.

**The move path must not feel the engines.** Bench A (2,000 sockets, 500 moves a second) with Stockfish searching flat out on the same 8-core machine:

| engines busy | how they run | move round trip p50 / p99 / p999 |
|---|---|---|
| none | | 0.18 / 0.74 / 4.4 ms |
| 8 | nice 19 | 21.7 ms at p99 |
| 6 | nice 19 | 0.95 / 13.9 / 20.3 ms |
| 4 | nice 19 | 0.23 / 2.9 / 9.2 ms |
| 8 | macOS background QoS (`PRIO_DARWIN_BG`: the efficiency cores) | 0.22 / 0.80 / 4.0 ms |
| 6 | background QoS | 0.21 / 0.73 / 2.7 ms |
| 4 | background QoS | 0.22 / 0.70 / 1.3 ms |
| 8, the server's own pool, 24 bots playing level 8 | background QoS | 0.22 / 0.88 / 1.9 ms |
| 4, the server's own pool, 16 bots playing level 8 | background QoS | 0.24 / 0.88 / 2.2 ms |

On macOS `nice` does not protect the loop; background QoS does, because it keeps the engines off the performance cores the loop runs on, so the engines now set it on themselves.
The price is engine speed: an efficiency core does 258,000 nodes a second, a fifth of a performance core, and four of them share their cluster (217,000 each).
That is why a level is a node budget, not a time: level 8 is a million nodes whatever core it lands on (0.7 s on a performance core, 4 to 5 s on an efficiency core), and a 5 s cap only bounds a starved search.
On Linux the same isolation is the CPU set: the engines pin themselves to `CC_ENGINE_CPUS` (c_base's CPU knob keeps the loop on its own core) and run at full speed; this has not been measured on Linux yet.

**The eval cache.** A position answered at a level is answered again from memory (Zobrist key mixed with the level, 2^20 slots, 16 MB), so the openings that every game passes through cost one search.
Its hits are in `/c/stats` (`ai_cache_hits`).

**The analysis board is the server's engine, not the browser's.** Stockfish's own makefile has a `wasm32` target, but it builds with `NNUE_EMBEDDING_OFF`: the page would fetch Stockfish 19's network at run time, and that network is 110 MB, against a page of 172 KB.
The server's engines and the eval cache answer instead: the page sends the moves that reach the position it shows (`POST /c/eval` on the socket), the answer (white's score, mate, depth, the best line) comes back as a 48-byte push, and every position asked once is answered from memory after that.
A browser engine stays the right design for unlimited private analysis once a small network can be used (Stockfish 17's small net was about 6 MB); that is a later step, and the numbers below say when it pays.

What an analysed position costs the server (Stockfish 19's own `bench` over its 49 positions, one M1 performance core; an efficiency core is about 5.5 times slower):

| search | nodes per position | CPU per position | positions per core per day |
|---|---|---|---|
| depth 10 | 9,200 | 7.3 ms | 11.8 million |
| depth 14 | 80,000 | 58 ms | 1.5 million |
| depth 18 | 613,000 | 460 ms | 188,000 |
| depth 22 | 1.9 million | 1.52 s | 57,000 |
| 400,000 nodes (the site's setting, about depth 17) | 400,000 | 300 ms | 288,000 |

At lichess's scale (5 million games a day, about 70 positions a game: 350 million positions a day), if every game were analysed on the server at the site's setting, that is 105 million core-seconds a day, about 1,200 cores running all day; if one game in ten is, 120 cores; the eval cache takes the shared openings off the top (perhaps a fifth of positions).
Answering the positions players actually click on is cheap; analysing whole games for everyone is not, and that is the threshold at which a browser engine with a small network (zero server cost, a one-time download of a few MB) should take over.

## 3.5 Puzzles

**Made offline from the engine's own blunders** (`server/tools/puzzlegen.c`).
Two weakened Stockfish players (Skill Level 1 to 9, 3,000 to 23,000 nodes a move) play each other; after every move a quick look (50,000 nodes) asks whether the side to move is suddenly winning (at least +250, where a moment before it was at most +150).
A candidate gets a deep look (300,000 nodes, MultiPV 2), and its line is kept move by move only while the solver's move is the only good one (mate, or at least 250 centipawns better than the second best); the defender's replies are the engine's best.
A line of one move is kept only when it mates.
One puzzle a game: after a blunder the rest of a game is lopsided.
Cost: about 10 games and 70 engine-seconds on a performance core per puzzle (50 puzzles from 485 games in 14 minutes on 4 engines).
That is affordable for a starting set and slow for lichess's 5 million; the cheaper source is the site's own games, where only the scan and the deep look run (about 2.5 s a game), which is the next step once there are games to mine.

**The file is the puzzles; the database holds only what people did to them.** A puzzle is a fixed 128-byte record (`c/src/puzzle.h`: the FEN, up to 12 moves, a rating), read into memory at start (`CC_PUZZLES`).
Its id is a hash of its position and line, so a regenerated file keeps the ratings of the puzzles it kept; the ratings and play counts are journaled (`cc_puzzles`), and each player's puzzle rating sits beside the game rating (`cc_players`).
A 32-bit id collides about once in 9,000 puzzles at a million (113 of 1,000,000 dropped as repeats); a 64-bit id is the change if the set grows that far.

**The browser solves; the server hands out and rates.** The page's kernel (`cp_solve`, as wasm) checks each move against the line (any mating move counts), plays the reply, and says how it went once; the server accepts one answer per puzzle handed out, then moves both Glicko-2 ratings (the puzzle is the opponent).
A try costs the server nothing; an attempt is two socket requests.
The pick: puzzles live in buckets of ten rating points, and a pick is a random one of at least the 32 nearest the player's puzzle rating that is not among the last 16 it was shown.

| puzzles in memory | load | server memory | CPU per attempt (pick, answer, both ratings, rebucket) |
|---|---|---|---|
| 1,000 | 1 ms | 2.4 MB (the whole process) | 397 ns |
| 100,000 | 71 ms | 19.8 MB | 474 ns |
| 1,000,000 | 805 ms | 172 MB | 763 ns |

(`puzzlegen --bench`, one performance core, under the quiet lock.) The first version kept a sorted order and shifted a puzzle to its place when its rating moved: 4.4 us an attempt at 100,000, because a new puzzle's rating jumps by hundreds of points across thousands of places; buckets made it a swap and a push.
At lichess's puzzle traffic (on the order of a million attempts an hour at peak) that is under a quarter of a millisecond of CPU a second, beside the socket's own cost of the two requests.

The puzzles page: 175 KB over 25 requests (the site's runtime, shared with every page), first contentful paint 132 ms local and 276 ms on the 4G profile; the puzzle is on the board at 246 ms (local, then a 600 ms pause before the opponent's move is played, on purpose).

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

Arenas, chat with moderation, ratings and leaderboards per time control, a browser engine with a small network for private analysis, Linux measurements of the engine isolation.
