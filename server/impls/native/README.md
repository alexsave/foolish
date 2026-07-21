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
see [`DURABILITY.md`](DURABILITY.md)). No other external packages — the
HTTP/1.1 layer is hand-rolled (a real deployment would drop in
mongoose/civetweb); auth is an in-memory token map (no JWT). Concurrency: a
dispatcher (the accept loop) routes one-shot requests onto small typed
worker pools sharded by game_id, and each game has its own lock instead of
one process-wide mutex — see [`SERVER_SCALING.md`](SERVER_SCALING.md)
("T2a") for the design, the Helgrind-clean verdict, and measured
throughput/latency/memory vs. the old single-global-lock version. Pool
sizes are runtime-configurable:
`./foolish_server 8099 --game-workers=N --meta-workers=N --create-workers=N`.

Durability: a background thread persists every game and user to a local
SQLite (WAL) database write-behind — the request path never blocks on disk,
and a crashed process (`kill -9`) recovers everything committed before the
crash, including resuming a game that was mid-play. DB ON by default
(`./foolish_server 8099 --db=./foolish.db`); `--no-db` opts out entirely for
tests/benchmarks that don't want a stray file. See
[`DURABILITY.md`](DURABILITY.md) for the write-behind design, the
crash-recovery test, and the measured overhead of persistence being on.

## Endpoints

```
POST /auth/signup {username}            -> {token, user_id}     (also /auth/signin)
POST /create               (Bearer)     -> {game_id}            creator takes seat 0
POST /meta {type,game_id[,strategy]}    (Bearer)   type: join | add-bot | start | continue
POST /action?game_id=..  <awire bytes>  (Bearer)   applies, then runs the bots
GET  /state?game_id=..&seat=..          -> the kernel's masked view (packed)
GET  /status?game_id=..                 -> 0 waiting / 1 playing / 2 over
GET  /health
GET  /ws?game_id=..&seat=.. (Bearer, Upgrade: websocket) -> RFC 6455 WebSocket
```

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
persistent connection pays that once per client SESSION instead. After the
upgrade the server immediately pushes the current masked state, then loops:
a client's binary frame is either a real **awire** move (applied through the
same `awire_decode`/`awire_apply` `/action` uses) or empty (just "send me
the current state" — a seat with no move yet still needs to notice when
another seat's move changes its own eligibility). Every reply is one binary
frame, `[ok:u8][state_put(seat) bytes]`. `foolish_hammer --mode=ws` is the
reference client: it decodes the pushed state, calls the kernel's own
`calculate_legal_moves` for its seat, and submits a randomly chosen LEGAL
move — so, unlike the HTTP load modes' mostly-illegal random frames, every
submitted move actually lands.

## Smoke test

With the server running:

```sh
bash test.sh                 # signup 2 humans, add a cordite bot, deal, attack
bash crash_test.sh           # kill -9 a live server, restart against the same --db, verify recovery
```

`test.sh` prints each seat's masked view (you see your own hand; opponents are
`null`), then plays the first attacker's opening card and shows the battle
appear on the defender's view with the bot's response — all decided by the
kernel. `crash_test.sh` starts its own server + its own scratch `--db`, drives
real gameplay, hard-kills the process, restarts it against the same DB file,
and asserts the recovered state matches — see
[`DURABILITY.md`](DURABILITY.md).

## Status / scope

Proof-of-concept. Present: auth, lobby, deal, human + bot moves, masked views,
continue-to-lobby, per-game locking + work-queue thread routing (see
[`SERVER_SCALING.md`](SERVER_SCALING.md)), a persistent-connection `/ws` push
path for the action+state hot loop (see above), and crash-safe SQLite
write-behind durability for every game and user (see
[`DURABILITY.md`](DURABILITY.md)). Not present (deliberately): broadcasting a
game's state to every seat's connection when ANY seat moves (`/ws` clients
each poll their own seat instead — see `foolish_hammer.c`'s ws worker), the
packed binary envelope the iOS client expects (this speaks plain JSON over
HTTP; `/ws` speaks the kernel's own packed wire), TLS, rate limits. The point
is the architecture, not production readiness.
