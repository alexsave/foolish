# `server/impls/native` — the Foolish server, in C

A second backend, next to [`../supabase`](../supabase). Where the Supabase impl
is TypeScript edge functions over Postgres, this is a **single long-lived C
process** that holds every game as a `Game` struct in RAM. It exists to prove
one claim from the A10 work: **the server API is language-agnostic, because the
kernel does the work.** The server just starts a socket, routes requests, and
hands them to the kernel — swap Postgres for a hash table and the edge runtime
for a thread pool, and the *game* is byte-for-byte the same.

This is the "in-memory authoritative state" design from
[`docs/ARCHITECTURE_AS_A_PATTERN.md`](../../../docs/ARCHITECTURE_AS_A_PATTERN.md)
("The C-server unlock"): no marshal in/out, no DB read on the hot path.

## The kernel drives everything

`foolish_server.c` reimplements **none** of Durak. Every rule is
[`../../../c/src/*.c`](../../../c/src) — the same sources the wasm and iOS builds
link:

| server does | kernel does |
|---|---|
| accept a socket, parse HTTP, auth token | — |
| hold `Game` structs + lobby identities in RAM | — |
| lock a mutex, pick the seat | — |
| — | `start_game` (deal), `handle_attack/cover/pass/pickup/good` (apply) |
| run the per-game bot game-loop thread | `bot_drive` (one paced cycle) + `bot_pacing_ms` (how long to wait) |
| — | `game_done` (who is the fool), `json_state_of` (masked per-seat view) |

### Bot pacing (the trampoline)

Bots don't resolve instantly — they think and throw in over time. Each game has
a loop thread that drives **one** `bot_drive` cycle, prices it with the kernel's
`bot_pacing_ms`, **releases the lock and `usleep`s that long**, then loops. The
kernel returns after every cycle (the `Game` struct is the continuation — no
suspended stack), so this is a *trampoline*, not a blocking hook inside the
kernel. It is the native twin of supabase's `await setTimeout(bot_pacing_ms…)`
loop and the phone's `Task.sleep` — same split everywhere: **kernel decides how
long, host decides how to wait.** `bash pacing_test.sh` shows the board
advancing at the ~3s "human watching" cadence.

Personal identity (usernames, tokens, `player_id`) lives in the server beside
the state blob, never inside it — exactly as `game.h` prescribes. Seat *kind*
(human vs bot) is not personal identity, though: it changes no rule but decides
whom the auto-driver may act for, so it rides the kernel's own `strategy_key`
(`STRATEGY_KEY_HUMAN` for a human), and the server asks `game_human_mask` for the
drive mask instead of keeping an `is_ai` array of its own.

## Build & run

```sh
make            # links foolish_server against c/src/*.c
make run        # ./foolish_server 8099
```

Requires a C compiler + `-framework Accelerate` (macOS; some kernel strategies
use LAPACK) + `libsqlite3` (present as a system package on Linux and macOS;
see [`DURABILITY.md`](DURABILITY.md)) + `libssl`/`libcrypto` (OpenSSL 3.x;
see [`TLS.md`](TLS.md)). No other external packages — the HTTP/1.1 layer is
hand-rolled (a real deployment would drop in mongoose/civetweb); auth is an
in-memory token map (no JWT). Concurrency: a dispatcher (the accept loop)
hands each connection off by `game_id`, and each game has its own lock
instead of one process-wide mutex — see [`SERVER_SCALING.md`](SERVER_SCALING.md)
("T2a") for the original design, the Helgrind-clean verdict, and measured
throughput/latency/memory vs. the old single-global-lock version. Plaintext
connections (both one-shot requests and `/ws`) are serviced by an
**epoll event loop per game-worker shard** — no thread per connection — a
`--tls` server keeps the original thread-per-connection design instead,
since non-blocking TLS wasn't attempted; see `SERVER_SCALING.md` "Stage 6"
for the epoll design, the game_workers tuning it needs, and why `--tls`
differs. Pool/shard sizes are runtime-configurable:
`./foolish_server 8099 --game-workers=N --meta-workers=N --create-workers=N`
(`--game-workers` sizes the epoll shard count in plaintext mode).

Durability: a background thread persists every game and user to a local
SQLite (WAL) database write-behind — the request path never blocks on disk,
and a crashed process (`kill -9`) recovers everything committed before the
crash, including resuming a game that was mid-play. DB ON by default
(`./foolish_server 8099 --db=./foolish.db`); `--no-db` opts out entirely for
tests/benchmarks that don't want a stray file. See
[`DURABILITY.md`](DURABILITY.md) for the write-behind design, the
crash-recovery test, and the measured overhead of persistence being on.

TLS: `--tls --cert=PATH --key=PATH` turns the WHOLE listen socket over to
TLS — every endpoint below speaks `https://` (one-shot requests) or `wss://`
(`/ws`) instead of plaintext; without `--tls` the server is plaintext
exactly as every earlier stage. One shared, read-only-after-setup
`SSL_CTX`, a fresh per-connection `SSL*` off it for every accepted socket.
See [`TLS.md`](TLS.md) for the I/O-abstraction design (`Conn`, `conn.c`/
`conn.h`), how to generate a test cert and run with it, the handshake/WSS
verification, and the measured TLS overhead. `foolish_hammer --tls` is the
matching load-test client.

## Endpoints

```
POST /auth/signup {username}            -> {token, user_id}     (also /auth/signin)
POST /create               (Bearer)     -> {game_id}            creator takes seat 0
POST /meta {type,game_id[,strategy]}    (Bearer)   type: join | add-bot | start | continue
POST /action?game_id=..  <awire bytes>  (Bearer)   applies, then runs the bots
GET  /state?game_id=..&seat=..          -> the kernel's masked view (packed)
GET  /status?game_id=..                 -> 0 waiting / 1 playing / 2 over
GET  /health
GET  /stats                             -> {bot_decisions, octogen_decisions} (Stage 4)
GET  /ws?game_id=..&seat=.. (Bearer, Upgrade: websocket) -> RFC 6455 WebSocket
GET  /ws?game_id=..&spectator=1 (Bearer, Upgrade: websocket) -> read-only spectator WebSocket (Stage 4)
```

Every path above is `http://`/`ws://` by default, or `https://`/`wss://`
when the server was started with `--tls` (see "TLS", above, and
[`TLS.md`](TLS.md)) — same paths, same request/response shapes, just over a
TLS-wrapped socket.

`start` deals once every seated human is ready (bots are always ready, 2+
seats). A move is the packed **awire** frame — `[kind, n, cards…(, attacks…)]`,
the SAME bytes the browser validates and the phone sends — POSTed as the raw
request body. The server enumerates no move types: it decodes with the kernel
(`awire_decode`) and applies through the kernel's one apply-entry
(`awire_apply`), so the move parser + dispatch switch a server used to carry
are gone.

### `/ws` — the persistent hot loop

`/ws` replaces the `/action` + `/state` round trip with ONE long-lived
connection per (authenticated, seated) client — see `ws.h`/`ws.c` for the
handshake (SHA-1 + base64, no external deps) and frame I/O, and
`PROFILE_HOTPATH.md`'s "T1b" section for why: thread-per-HTTP-request meant
a fresh `pthread_create` (a zeroed 8 MiB stack) on every single move; a
persistent connection pays that once per client SESSION instead. A
plaintext connection no longer even gets its own OS thread for that
session — since Stage 6 it's serviced by its game's epoll-worker shard
(see `SERVER_SCALING.md` "Stage 6"); `--tls` still uses one thread per
connection. **The protocol is push-only** (`PROFILE_HOTPATH.md` "T1f"):
after the upgrade the server pushes the current masked state, and thereafter
pushes fresh state to a seat/spectator whenever the game changes — any
seat's move, a bot move, or a round transition — via the game's epoll-worker
fanning out the per-version cached view (`worker_push_stale`). A client
therefore **never polls**: its only outgoing frames are real **awire** moves
(applied through the same `awire_decode`/`awire_apply` `/action` uses),
submitted at most once per pushed state version and only when the seat is
actually eligible. Every push is one binary frame,
`[ok:u8][state_put(seat) bytes]`. `foolish_hammer --mode=ws` is the
reference client: it decodes the pushed state, calls the kernel's own
`calculate_legal_moves` for its seat, and submits a randomly chosen LEGAL
move — so, unlike the HTTP load modes' mostly-illegal random frames, every
submitted move actually lands.

`/ws?game_id=..&spectator=1` (Stage 4, [`SERVER_SCALING.md`](SERVER_SCALING.md))
is the same upgrade for a READ-ONLY watcher: a valid Bearer token is still
required, but no seat ownership check — any authenticated user may spectate
any existing game. It receives `VIEW_SPECTATOR` pushes (every hand AND the
deck hidden), cached per game (not per seat, since every spectator of a game
sees the same bytes), and any frame it sends is silently ignored — a
spectator can never move. `foolish_hammer --spectators=N` is the reference
client.

## Smoke test

With the server running:

```sh
bash test.sh                 # signup 2 humans, add a cordite bot, deal, attack
bash crash_test.sh           # kill -9 a live server, restart against the same --db, verify recovery
bash tls_test.sh             # generate a throwaway cert, verify HTTPS + WSS over TLS
```

`test.sh` prints each seat's masked view (you see your own hand; opponents are
`null`), then plays the first attacker's opening card and shows the battle
appear on the defender's view with the bot's response — all decided by the
kernel. `crash_test.sh` starts its own server + its own scratch `--db`, drives
real gameplay, hard-kills the process, restarts it against the same DB file,
and asserts the recovered state matches — see
[`DURABILITY.md`](DURABILITY.md). `tls_test.sh` generates its own throwaway
self-signed cert, starts its own `--tls` server, and checks a real TLS
handshake, HTTPS, and a WSS `foolish_hammer --tls --mode=ws` run — see
[`TLS.md`](TLS.md).

## Status / scope

Proof-of-concept. Present: auth, lobby, deal, human + bot moves, masked views,
continue-to-lobby, per-game locking + work-queue thread routing (see
[`SERVER_SCALING.md`](SERVER_SCALING.md)), a persistent-connection `/ws` push
path for the action+state hot loop (see above), crash-safe SQLite
write-behind durability for every game and user (see
[`DURABILITY.md`](DURABILITY.md)), and TLS (HTTPS + WSS, see
[`TLS.md`](TLS.md)). Partially present since Stage 6 (plaintext only): when
a SERVER-SIDE BOT's move changes a game, the epoll worker that owns that
game's connections proactively pushes fresh state to all of them (see
`SERVER_SCALING.md` "Stage 6" — the epoll↔bot_thread wakeup seam); a HUMAN
move does NOT broadcast to other seats (deliberately — see that same
section for why fanning out on every human move measurably hurt
throughput) — every `/ws` client still polls its own seat for that case
(see `foolish_hammer.c`'s ws worker). The packed binary envelope the iOS
client expects (this speaks plain JSON over HTTP; `/ws` speaks the kernel's
own packed wire), cert rotation, connection
limits/backpressure beyond the work-queue's own bounded blocking push (see
`SERVER_SCALING.md`), and rate limits. The point is the architecture, not
full production readiness — see "Production readiness" below for a plain
tally of what each hardening stage did and didn't cover.

## Production readiness — what's done, what's still needed

Several stages, each documented in its own file (`SERVER_SCALING.md` covers
concurrency across multiple stages — 1, 5, and 6 below):

| stage | what it adds | doc |
|---|---|---|
| 1 | per-game locks + work-queue thread routing (replaces one process-wide lock; a dispatcher shards one-shot requests onto typed worker pools by `game_id`) | [`SERVER_SCALING.md`](SERVER_SCALING.md) |
| 2 | SQLite WAL write-behind persistence + crash recovery (a `kill -9` loses at most one write-behind interval, not everything) | [`DURABILITY.md`](DURABILITY.md) |
| 3 | TLS for every endpoint, including the `/ws` hot loop (HTTPS + WSS, not just the one-shot requests) | [`TLS.md`](TLS.md) |
| 5 | parallel bot compute — kernel globals on the `bot_drive`/`awire_apply` path made thread-local so games no longer serialize bot decisions through one process-wide lock | [`SERVER_SCALING.md`](SERVER_SCALING.md) |
| 6 | epoll-per-shard connection I/O for plaintext (replaces thread-per-`/ws`-connection — a game-worker thread now runs an epoll loop over its shard instead of one OS thread per live connection); `--tls` keeps the Stage-1/5 thread-per-connection design, since non-blocking TLS wasn't attempted this stage | [`SERVER_SCALING.md`](SERVER_SCALING.md) |

Still needed for a real production deployment, not attempted here (POC
scope, stated plainly rather than silently): cert rotation/ACME (a cert is
loaded once at startup, not reloaded), non-blocking TLS (a `--tls` server
still pays thread-per-connection — see `SERVER_SCALING.md` "Stage 6"),
connection-count limits or 503-style load shedding (the work queues apply
backpressure by blocking the accept loop, not by rejecting — see
`SERVER_SCALING.md`), rate limiting / abuse protection, JWT or another real
auth scheme (tokens are an in-memory opaque map), horizontal scale-out (one
process, one machine — the per-game lock design doesn't extend across
processes), and structured observability (metrics/tracing beyond the
stderr startup banner and the load tools' own stdout summaries).
