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

Identity (names, is-ai, tokens) lives in the server beside the state blob, never
inside it — exactly as `game.h` prescribes.

## Build & run

```sh
make            # links foolish_server against c/src/*.c
make run        # ./foolish_server 8099
```

Requires a C compiler + `-framework Accelerate` (macOS; some kernel strategies
use LAPACK). No external packages — the HTTP/1.1 layer is hand-rolled (a real
deployment would drop in mongoose/civetweb); auth is an in-memory token map
(no JWT). Concurrency is thread-per-connection under one global lock
(single-writer per store op) — enough for a POC.

## Endpoints

```
POST /auth/signup {username}            -> {token, user_id}     (also /auth/signin)
POST /create               (Bearer)     -> {game_id}            creator takes seat 0
POST /meta {type,game_id[,strategy]}    (Bearer)   type: join | add-bot | start | continue
POST /action?game_id=..  <awire bytes>  (Bearer)   applies, then runs the bots
GET  /state?game_id=..&seat=..          -> the kernel's masked view (packed)
GET  /status?game_id=..                 -> 0 waiting / 1 playing / 2 over
GET  /health
```

`start` deals once every seated human is ready (bots are always ready, 2+
seats). A move is the packed **awire** frame — `[kind, n, cards…(, attacks…)]`,
the SAME bytes the browser validates and the phone sends — POSTed as the raw
request body. The server enumerates no move types: it decodes with the kernel
(`awire_decode`) and applies through the kernel's one apply-entry
(`awire_apply`), so the move parser + dispatch switch a server used to carry
are gone.

## Smoke test

With the server running:

```sh
bash test.sh                 # signup 2 humans, add a cordite bot, deal, attack
```

It prints each seat's masked view (you see your own hand; opponents are `null`),
then plays the first attacker's opening card and shows the battle appear on the
defender's view with the bot's response — all decided by the kernel.

## Status / scope

Proof-of-concept. Present: auth, lobby, deal, human + bot moves, masked views,
continue-to-lobby, basic concurrency. Not present (deliberately): durability
(WAL/snapshot — state is RAM-only), realtime push (clients poll `/state`), the
packed binary envelope the iOS client expects (this speaks plain JSON), TLS,
rate limits. The point is the architecture, not production readiness.
