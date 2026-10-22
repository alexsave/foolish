# birdseye: design and measurements

Built 2026-10-22 on branch `foolish-birdseye`, as adventurer's sibling: a load simulator for foolish and the live picture of what it does to the server, on the same c_base platform, with nothing of foolish's server changed.
Every number below was measured on this Mac (Apple M-series, 8 cores, macOS) with another agent building and testing at the same time; the load average is stated beside each figure.

## The shape

Three processes, started by `tools/run.sh`:

- The game server: c_base's `cbase serve` with foolish's app, exactly the binary the deploy builds (`make -C c_base build/cbase`), on its own SQLite file, in dev mode with throwaway secrets and a week-long token, its rate limits lifted because every fake human arrives from 127.0.0.1.
- `birdseye serve`: one c_base process of its own (the platform linked by source, as adventurer links it), with the simulator's player threads, the aggregate of every table, the site's three HTTP routes, the platform's `GET /metrics`, and the platform's stream hub carrying the site's pushes.
- The site: Next.js, client-rendered where it talks to birdseye, decoding fixed binary layouts with readers structgen writes from the C headers.

### The fake humans

A player is an ordinary client of the game server.
It signs up over HTTP (`POST /v1/auth/signup`, the kernel's `NwRequest`), opens the platform's socket (`GET /ws`), says HELLO with its token, and from then on sends every request on that socket as an `SF_REQ` (the HTTP route's method, path and body: `/v1/create`, `/v1/meta` asks, `/v1/action` moves, a `/v1/games/<id>` read) and hears every push there.
It subscribes to its game's room as the web client does, applies its own seat's casts (`NW_WS_CAST`) to the kernel's client table (`client_cast_open`, `client_push_next`, `client_push_final`, delta casts rebuilt in an attached `ClientDelta`), drops the spectators' casts while seated, and reads the game whole (`GET /v1/games/<id>`) when a cast cannot be applied, which is counted as a resync.
Every byte is written and read by the kernel's own codecs (`netwire.h`, `ask.h`, `table.h`, `awire.h`, `client_table.h`) and the platform's stream frames (`shared/c/stream_frame.h`); the client library `netclient.h` is one client per process, so the players use the codecs directly, as the roundtrip bench and foolyard do.

A move is the kernel's: `calculate_legal_moves` on the masked board, the handwritten brain's choice (`handwritten_prod_strategy_choose`, the one brain that plays correctly on a client's masked board; the belief bots read hidden cards as if they were real) most of the time and a random legal move 15% of the time so games differ, validated by `client_validate`, encoded by `awire_encode` into a `table_request_encode` body with the board's version as the intent.
A think time (800 to 2500 ms by default) precedes each move.
One move in fifty is hostile: a card the seat does not hold, bytes that are not a move, an attack of 28 copies of one card, or a move sent for another live table; each must be refused, and one the server applies is counted as a finding and shown in red.

Tables are planned per thread: two seats 90% of the time, else three to six; a server bot (handwritten, robusta, cordite or firecracker, picked from `GET /v1/bots`) in 40% of tables; podkidnoy rules in 20%.
The creator creates and sets up one ask at a time (rules, bots), the other humans join, every human says START once the table is full, and the server deals.
After the fool is known the seats linger six seconds, then rematch (`ASK_CONTINUE`, 30%) or leave (`ASK_EXIT`; the last one out deletes the table), and the players rest a few seconds before their next table.
A table's humans all belong to one thread, so no player ever waits on another thread; each thread is one `poll` loop over its players' sockets with the think times as its timeouts, and one `LegalMoves` of scratch (140 KB) for all of them.

### The aggregate and the site's surface

`world.h` keeps every table as the site sees it: one `be_w_table` row (phase, seats with names and hand counts and the kernel's turn rule, the battles, the trump, the stock, the fool, the last event, the last move's latency), the hands the sim's own humans hold, and a ring of the last 32 events.
The creator of a table reports its board after every push it applies; every human reports its own hand.
Each change is one lock take and one publish.

The site's surface is `wire.h`: `GET /be/tables` (a head with the layout hash, the row count, the first row's offset and the row size, then the rows), `GET /be/table/<gid>` (the row, the known hands, the event tail), `POST /be/session` (a dev-mode token for the stream), and the platform's `GET /metrics`.
The decoders are structgen's, written from `wire.h` and from `c_base/core/src/metrics.h` by the compiler's own layout; the layout hash structgen prints is compiled into the server and stamped into every head, so a page built from another layout refuses the bytes.
No TypeScript spells a byte offset.

The pushes go through c_base realtime: birdseye declares one topic family, `s/{doc}`, and publishes every change to the room "all" (`cb_rt_publish`, from any thread; the hub copies the frame and posts it to the loop).
The site opens the platform's `/ws`, says HELLO with the session's token, subscribes to the room (`SF_SUB`, what `NW_SUB_GAME`, gid "all"), and hears three kinds of push: `[SF_V][BE_F_TABLE]` a row on every change, `[SF_V][BE_F_EVENT]` an event as it happens, `[SF_V][BE_F_OPS]` the platform's metrics report once a second.
The site writes and reads the socket's own frames through `shared/c/stream_frame.h` compiled to wasm (`server/wasm/sf_wasm.c`, 1.3 KB, bindings typed by structgen from the link line and checked against the built module), so the one owner of those bytes stays the header.

### The numbers

birdseye declares what it counts as a table of names (`be_metrics.h`: five histograms, five rates, five counters, ten gauges) in its `cb_app`; the platform keeps the storage, the once-a-second report and `GET /metrics` (c_base's metrics registry, lifted on branch `cbase-app-lifts`, with the histogram, the rate ring and the process readers in `shared/c/stats` and `shared/c/proc.h`).
The player threads bump their rows by index; the ops timer writes the gauges (the tables, the players, the sockets, the game server's resident set and its CPU over the last second through its pid) and pushes the report.
A histogram's percentile is the floor of the bucket the sample falls in, four buckets per octave, so a p50 reads up to 19% under the true value and never over.

## Measured

Two sizes of the sim, each after 180 s, against the unchanged game server (one `cbase serve`, `CBASE_REQ_WORKERS=2`, `CBASE_BOT_WORKERS=2`), with another agent's builds on the machine (load average 5 to 10 on 8 cores; the series at 400 players was cut short when Alex asked for the CPU back).
The sim's latencies are a move's request out on the player's socket to its reply in (`move ack`) and to the mover's own cast of it in (`move push`), over the platform's hub (an `SF_REQ` is handed to the request pool, answered, and the cast posted back to the loop).
The bystander is `cbase-bench roundtrip` (two more humans over plain HTTP, 100 moves, one connection) run beside the sim.

| | 60 players, 25 tables (4 threads) | 200 players, 84 tables (4 threads) |
|---|---|---|
| moves/s, games/min | 12.3, 13 | 39.0, 60 |
| pushes/s heard, bytes/s | 72, 3.3 KB | 231, 14.8 KB |
| move ack p50 / p90 / p99 / max | 256 us / 448 us / 3.1 ms / 12.9 ms | 192 us / 320 us / 4.1 ms / 25 ms |
| move push p50 / p90 / p99 | 192 us / 320 us / 3.1 ms | 160 us / 256 us / 4.1 ms |
| other request p50 / p99 (creates, asks, reads) | 384 us / 2.6 ms | 384 us / 14.3 ms |
| cast decode on the client table p50 / p99 | 7 us / 48 us | 4 us / 32 us |
| game length p50 / p99 | 56 s / 160 s | 64 s / 160 s |
| hostile moves sent / applied | 51 / 0 | 159 / 0 |
| legitimate moves refused, resyncs, stalls | 0, 0, 0 | 0, 0, 0 |
| game server RSS, CPU | 15 MB, 0.74% of a core | 21 MB, 0.62% of a core (one-second sample; see the gaps) |
| birdseye RSS, CPU over 180 s | 23 MB, 2.1 s | 30 MB, 4.4 s |
| bystander action p50 / p99 (HTTP) | 86 us / 180 us | 96 us / 131 us |
| bystander view read p50 / p99 | 72 us / 204 us | 69 us / 134 us |

The server's work per move is well under a millisecond; the p99s in the sim's own columns are the Mac's scheduler under a load average of 5 to 10 (the bystander, measured in the same minutes over HTTP, never saw them), and the "other request" p99 at 200 players is the creates and joins, which write SQLite, queued behind each other on the two request workers while 84 tables were being set up at once.
A 400-player run was started and not finished.

## Real, simplified, and missing

Real: the game server (the deployed binary, its protocol, its bots, its rate limits as knobs), every byte the fake humans send and read, the kernel's rules and brain on the client's masked board, the pushes through c_base realtime, the latencies as a client sees them.
Simplified: the fake humans share one process and one IP; think times are uniform; the handwritten brain plays every human seat (the belief bots cannot play a masked board); a table's humans come from one thread; only the sim's own tables are shown (a table made by a real client against the same server is not in the world, since birdseye learns tables from its players, not from the server); the site's token is a fixed dev account on birdseye's own store, until the platform's anonymous public-family subscription lands.
Missing: chat, ratings and the leaderboard, replays, sign-out and account deletion (the accounts accumulate in the game's SQLite file across runs and are signed into again by name), a spectator-only watcher of someone else's table.

## The league and the tournament (2026-10-22, second session)

Bots alone at a table cannot be dealt by any client request (the kernel deals only for a seated asker), so the game server has a dev-only `POST /dev/bot_table` (routed only under `CBASE_DEV=1`, behind the admin token) that runs the lobby's own kernel calls: a house account seats the bots, leaves, and the first bot readies (or seats the eighth), which deals.
The kernel paces bots-only tables at 300 ms a move, so the server also has a dev-only `CBASE_BOT_THINK_MS=min-max`: a bot waits a think time drawn per (game, version) after the last shown move.
`birdseye/tools/hotpath_proof.sh` compiles every server source before and after and compares each function's disassembly: the move and push path (functions_http.c, pushes.c, fanout.c, the journal, the store) is identical; bots.c's drive differs by the one added call.
The league (league.c) watches every table as one spectator account and records each finished game's rating stamp (before, after, delta by seat) as a result; the site draws them.
The tournament (tournament.c) is bookkeeping over the same engine; its bracket lives in its own file and is shown in every mode.
Measured with 40 league tables at the think pace: the game server at about 5% of a core (the gauge's ten-second window checked against the process's CPU time over the same ten seconds: 4.97% vs 5.03%, 4.77% vs 4.70%), birdseye about 1%; with 16 tournament tables at the bots-only pace about 13%.

## Known gaps

- A restart of the game server while league tables play leaves them on the server; birdseye takes its own tables up again across its own restart (`league.bin.live`), but a tournament's game in play at a stop is played again rather than watched (the server still rates the abandoned one, so the league history can hold a game the bracket does not).
- Bots that have never played are missing from the server's leaderboard, so on a fresh server the league shows their rating as 0 until their first game (the first stamp says the real one).
- The league's games/min in the top bar counts rated results over ten minutes; the platform's one-second rate of games is shown in Numbers only.
- The histogram's four buckets per octave make a p50 read up to 19% low.
- The 400-player mixed-mode measurement was never finished.
