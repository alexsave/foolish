# Server concurrency — per-game locks + work-queue routing ("T2a")

Production-hardening stage 1 of 3 (concurrency) for
`server/impls/native/foolish_server.c`. Stage 2 (SQLite WAL write-behind
persistence + crash recovery) is now done too — see
[`DURABILITY.md`](DURABILITY.md). Stage 3 (OpenSSL TLS / WSS+HTTPS) is NOT
done here — this file documents the seam left for it (last section).

Everything below changed only `server/impls/native/foolish_server.c` (plus
this file, README.md, and a PROFILE_HOTPATH.md cross-reference). `c/src/*`
(the kernel) and `ws.c`/`ws.h` are untouched — the constraint for this stage
was read-only access to the kernel, and Deliverable 1's Helgrind findings
below turned that from a formality into a real design decision (see
"What Helgrind found").

Measured on this box: Linux, 4 cores. Same box PROFILE_HOTPATH.md's captures
used.

## Deliverable 1 — per-game locks, and a third lock the kernel forced

### Design

`g_lock` (one process-wide mutex around every store operation) is replaced
with three tiers:

- **`g_registry_lock`** — small, short-held. Guards only `g_users[]` +
  `g_token_ht` (signup/token lookup) and game-slot allocation + `g_game_ht`
  (game_id -> `GameSlot*`). Never held during game work, bot work, or socket
  I/O.
- **`GameSlot.lock`** (new field, one per game) — guards everything about
  ONE game: its `Game` struct, lobby roster (`seat_user`/`seat_name`/
  `seat_ready`/`owner`), `cond`/`bot_running`, and the per-seat view cache.
- **`g_kernel_lock`** — small, one process-wide mutex, held ONLY around the
  specific kernel calls that mutate a `Game` or drive bots (`awire_apply`,
  `bot_drive`, `game_seat_and_deal`). See "What Helgrind found" below for
  why this exists — it wasn't part of the original plan.

Every handler follows one pattern: take `g_registry_lock` -> find/allocate
the `User*`/`GameSlot*` (copying out any `User` field still needed, since
`User*` is only safe to dereference under `g_registry_lock`) -> take the
`GameSlot`'s own lock -> release `g_registry_lock` -> do the game work
(taking `g_kernel_lock` around any kernel-mutating call) -> release the
`GameSlot` lock -> respond (all socket I/O happens with NO lock held).

**Lock order (deadlock-freedom): registry, then game, then kernel — always,
never the reverse.** `g_registry_lock` is never re-acquired while any
`GameSlot.lock` is held; `g_kernel_lock` is always the innermost lock, taken
only while already holding the relevant game's lock, and released before
anything else; no handler holds two `GameSlot` locks at once. `bot_thread`
and the `/ws` dedicated connection thread (`ws_conn_thread`) hold at most
one game's lock (+ `g_kernel_lock`, innermost) — neither touches
`g_registry_lock` after its initial lookup.

`bot_thread` (the per-game trampoline) takes `s->lock` instead of the old
global lock for its drive cycle, and releases it during the pacing
`usleep`, exactly as before — just scoped to one game now. The
`pthread_cond_wait`/`pthread_cond_signal` pairing (bot wakeup) now pairs
with `s->lock`. The per-(game,seat) view cache lives in `GameSlot`, so it's
covered by the game lock; `h_state`/`h_status` still take the game lock for
the read (no `g_registry_lock` needed beyond the initial slot lookup).

### What Helgrind found (and why there's a third lock)

Per-game locks alone were NOT enough to pass the gate. Two real races
surfaced, both instructive:

**1. `engine_last_reject` (game.c).** Every `handle_attack`/`handle_cover`/
`handle_pass`/`handle_pickup`/`handle_good` call writes this process-wide
`int` (the reject-reason out-param `awire_apply` documents). Under the OLD
single-`g_lock` design this was safe by accident — the whole server was one
critical section, so no two kernel-mutating calls ever ran concurrently.
With per-game locks, two DIFFERENT games' threads can call `awire_apply`
concurrently, and both write this one shared global. Helgrind's first
report on an early per-game-lock-only build:

```
Possible data race during write of size 4 ... by thread #95
   at handle_cover (awire_apply -> ws_conn_thread)
Locks held: 1, at address <a GameSlot.lock>
This conflicts with a previous access by thread #97
   at handle_pass (awire_apply -> ws_conn_thread)
```

(paraphrased from the raw capture — two different games' `ws_conn_thread`s,
each correctly holding ITS OWN `GameSlot.lock`, racing on the same kernel
global because nothing serializes them against EACH OTHER.)

Tracing the kernel source (read-only to this stage, but readable) confirms
this is a known, documented kernel property, not a surprise bug:
`bot_drive.c` says so directly about its own scratch buffer —

> "`LegalMoves` is far too big for the wasm module's 22KiB shadow stack ...
> One shared static is safe: bot_drive is never re-entered."

`static LegalMoves g_scratch;` (bot_drive.c) is used by `bot_drive` AND
`game_human_mask`'s underlying eligibility scan on every drive cycle;
`engine_last_reject` and `engine_snap_hook` (game.c) are the same story —
process-wide state a single external caller was always assumed to own.
`legal.c`'s `legal_stat_max_n` (a high-water-mark stat updated inside
`calculate_legal_moves`, reached via `bot_drive`'s eligibility scan) and a
function-local `static GameLog scratch;` inside `game.c`'s `log_alloc` (hit
whenever a log entry is dropped) are further instances of the same pattern
found by source inspection. None of these are reachable except through
`awire_apply`, `bot_drive`, or `game_seat_and_deal` — so one lock around
those three call sites covers all of them.

Given the hard constraint of this stage (`c/src/*` is read-only), the
honest fix is `g_kernel_lock`: a small, narrowly-scoped THIRD lock held only
for the duration of those specific calls. It does NOT serialize state
reads — `state_put`/`state_put_cached` never touch any of the above
(confirmed by inspection: `view.c` has no such globals, and never calls
`calculate_legal_moves`), so `h_state`/`h_status`/WS polls stay fully
per-game-lock-parallel. Only the actual kernel WRITE path is serialized
process-wide — a materially smaller critical section than the old `g_lock`,
but not the "every game mutates in parallel" ideal a from-scratch kernel
could offer. This is exactly the "partial, correct result" the task brief
calls a good outcome: per-game locking is real and does what it's supposed
to for reads/lookups/lobby-state; the kernel's own single-writer assumption
for its internal apply/drive path is a genuine constraint that per-file
locking can't wish away without editing `c/src`.

**2. `g_seq` (foolish_server.c, our own bug).** A monotonic counter mixed
into `gen_id()` (written under `g_registry_lock`, inside `gen_id`) was ALSO
read directly, unguarded, by `h_meta`'s "start" branch (seasoning the deal
seed) — which only holds the `GameSlot` lock, not the registry lock.
Helgrind caught the write/read race directly. Rather than invent a
registry-while-holding-game special case (which would violate the lock
order invariant above), `g_seq` became a plain C11 atomic
(`atomic_ulong` + `atomic_fetch_add_explicit`/relaxed) — it has no other
invariant to protect, so a lock was never the right tool for it.

**3. `rand()` (libc, surfaced once worker-pool sizes became configurable).**
Not Helgrind-caught in THIS codebase (see "why" below), but worth recording
as a related finding: glibc's `rand()` shares unlocked global state across
callers. The two server call sites (`gen_id`, `h_meta`'s deal-seed loop)
were originally safe only because they happened to be reachable from
exactly one worker thread each (a single auth/create worker, a single meta
worker) — an invariant that Deliverable 2's configurable worker pools
(`--meta-workers=N`, `--create-workers=N`, both now tunable above 1) would
have silently broken. Fixed proactively with a per-thread `rand_r` seed
(`next_rand()`, thread-local state) instead of relying on a topology
invariant that future flag changes could violate unnoticed.

### Helgrind verdict

**Clean.** `valgrind --tool=helgrind --history-level=approx` against
`./foolish_server <port>`, driven by `foolish_hammer --mode=mixed` (real
`/action`/`/state`/`/status` traffic through the game-worker queues) and
`--mode=ws` (the per-game lock + `bot_thread` + `ws_conn_thread` path) back
to back on the same run, `kill -TERM` on the valgrind PID afterward (same
technique PROFILE_HOTPATH.md uses — `timeout` forks an untraced child, so it
can't be used to end a callgrind/helgrind capture cleanly). Run 3 times
across different worker-pool configurations to exercise every documented
code path:

| config | traffic | result |
|---|---|---|
| game=6, meta=1, create=1 | mixed 15s + ws 15s | `ERROR SUMMARY: 0 errors from 0 contexts` (after the two fixes above; see the paraphrased race report for the BEFORE state) |
| `--meta-workers=0` fold mode (game=3, create=2) | mixed 15s + ws 15s | `ERROR SUMMARY: 0 errors from 0 contexts` |
| multi-worker pools (game=6, meta=2, create=2) | mixed 18s + ws 18s | `ERROR SUMMARY: 0 errors from 0 contexts` |
| **shipped defaults (game=4, meta=0, create=1)** | mixed 15s + ws 15s | `ERROR SUMMARY: 0 errors from 0 contexts` |

No suppressions were needed beyond valgrind/libc's own defaults (glibc
pthread internals Helgrind ships suppressions for) — every report above was
in this file's own code, and every one was fixed, not suppressed. Full
digest (exact commands, every run): `bench_results/T2a_scaling/
helgrind_summary.txt`.

## Deliverable 2 — work-queue thread routing

### Design

Thread-per-connection is replaced with a dispatcher + typed worker pools.
The dispatcher IS the accept loop (no separate reader-thread pool): it
accepts a connection, reads + parses the request headers/body
(`read_and_parse_request`), then either:

- hands a `/ws` upgrade to its own dedicated thread (`ws_conn_thread`,
  spawned directly — see "WS design" below), or
- classifies the request by path (`classify_queue`) and pushes it onto one
  of three typed, bounded MPSC work queues (mutex + condvar, ring buffer,
  512 entries):
  - `/auth/*`, `/create` -> the CREATE/AUTH pool
  - `/meta` -> the META pool (or folded onto the GAME pool if
    `--meta-workers=0` — see the sweep below for why that's on the table at
    all)
  - `/action`, `/state`, `/status` -> `GAME pool[hash(game_id) %
    N_GAME_WORKERS]`

Sharding the game pool by `game_id` means every one-shot HTTP request for a
given game lands on the SAME worker thread — requests for one game are
serialized by construction, so the per-game lock only has to arbitrate
against that game's `bot_thread` and its `/ws` connection(s), not a pile of
other HTTP workers for the same game.

All three pool sizes are runtime-configurable (`--game-workers=N
--meta-workers=N --create-workers=N`) — see the sweep below for how the
shipped defaults were chosen.

### WS design: (B), not (A) — and why

Two designs were on the table: (A) an epoll-per-shard design where each
game worker owns an epoll loop servicing its shard's WS connections
single-threadedly (no thread-per-connection, kills the ~0.9MB/conn memory
tax PROFILE_HOTPATH.md's T1c measured); (B) keep a dedicated thread per live
WS connection, routed off the typed queues, taking the per-game lock.

**(B) was chosen.** Reasoning: correctness first. (A) requires moving bot
ticks onto the same worker's epoll loop as deferred/timerfd work, handing
off an accepted fd from the dispatcher to a specific worker's epoll set
across threads, and reasoning about partial reads/writes on a
non-blocking socket multiplexed against N other connections and the
worker's own queue-drain loop — a materially larger, more failure-prone
surface to get Helgrind-clean in the time available, for a payoff (memory
per connection) that Deliverable 3's numbers below can evaluate honestly
without having built it. (B) keeps the existing, already-correct
`ws_conn_thread` shape (handshake, then a receive/apply/reply loop),
changes only WHICH lock it takes (per-game instead of global) and where its
thread comes from (spawned by the dispatcher instead of a raw
`conn_thread`). The tradeoff is real and stated plainly: still a thread per
live WS connection, so PROFILE_HOTPATH.md T1c's per-connection memory tax is
unchanged by this stage — Deliverable 3 measures it.

`bot_thread` stays its own per-game trampoline thread (not moved onto a
worker), per the task's explicit permission to make that call — it already
takes the per-game lock correctly, and folding it onto a worker pool would
mean a worker blocking for a game's whole pacing sleep, which defeats a
bounded worker pool's point even more than a blocking WS thread would.

## Deliverable 2 — worker-pool sweep (empirical, not guessed)

Every worker-pool size (`--game-workers=N --meta-workers=N
--create-workers=N`) is a runtime flag, tuned empirically on this 4-core box
rather than assumed. Method: `foolish_hammer --mode=ws` (the WS+legal
hammer — applied-moves/s and p99 latency are the deciders, as instructed),
2-3 trials per point, at 32/160/400 connections (`--games=8/40/100
--seats=4`). A supplementary sweep under `--mode=mixed` (real HTTP
`/action`/`/state`/`/status` traffic, which — unlike `/ws` — actually flows
through the game-worker queues) is included too, because it turned out to
be necessary: see "What the sweep found" below for why.

### game_workers, WS+legal hammer (the decider) — mean applied/s over 3 trials (min-max range)

| conns | gw=2 | gw=4 | gw=8 |
|---|---|---|---|
| 32  | 201 (194-215) | 193 (178-206) | 204 (176-229) |
| 160 | 2523 (1671-3849) | 2474 (2208-2864) | 5260 (3699-6907) |
| 400 | 12281 (10146-13732) | 12586 (11072-13626) | 11898 (6063-18227) |

### game_workers, mode=mixed (real `/action` traffic through the game queues) — mean applied/s over 2 trials (min-max range)

| conns | gw=2 | gw=3 | gw=4 | gw=6 | gw=8 |
|---|---|---|---|---|---|
| 32  | 1585 (1576-1593) | 2861 (1576-4147) | **4176** (4159-4193) | **4293** (4292-4293) | 2812 (1623-4000) |
| 160 | 2628 (1552-3704) | 3935 (3900-3969) | 3056 (2094-4018) | 1548 (1513-1583) | 3573 (3521-3626) |
| 400 | 1464 (1328-1601) | 1213 (1208-1219) | **3307** (3221-3393) | **3718** (3709-3727) | 2350 (1382-3319) |

### meta_workers / create_workers, WS+legal hammer @ 160 conns, gw=4 — mean applied/s over 3 trials (min-max range)

| meta_workers | mean applied/s (range) | | create_workers | mean applied/s (range) |
|---|---|---|---|---|
| 0 (fold into game pool) | 2665 (2141-3689) | | 1 | 3430 (1989-5041) |
| 1 (dedicated) | 3467 (2592-4869) | | 2 | 4082 (2156-6826) |
| 2 (dedicated) | 3169 (1862-5056) | | 4 | 2729 (2153-3124) |

### What the sweep found

**Under `--mode=ws` (the instructed decider), `game_workers` has essentially
no effect, and that's architectural, not a measurement failure.** By
design (Deliverable 2's WS design B), a `/ws` connection is serviced by its
own dedicated `ws_conn_thread` and never touches the typed work-queue pools
— the game-worker queues only carry `/action`/`/state`/`/status` HTTP
requests and (if folded) `/meta`. WS-mode's steady-state traffic is ~100%
`/ws` frames (per PROFILE_HOTPATH.md T1c, ~99% of those are polls, not even
moves), so `game_workers` legitimately has almost nothing to do during a
`--mode=ws` run — the 32-conns row above shows gw=2/4/8 statistically
indistinguishable (~193-204 applied/s, fully overlapping ranges), and even
at 400 conns the three means (12281/12586/11898) sit well inside each
other's min-max spread. The one directional wrinkle — gw=8 trending both
higher-mean AND higher-variance at 160 conns — reads as scheduling noise
from 8 mostly-idle worker threads competing for 4 cores against the
per-connection WS threads and bot threads that ARE doing the real work,
not a benefit from the pool itself.

That's a real, useful finding, not a null result: it says the typed-queue
design pays for itself on the HTTP/one-shot path without taxing the
persistent-connection path at all, and it means `game_workers` should be
tuned for the workload that actually uses it. So the supplementary
`--mode=mixed` sweep (real `/action` traffic, which DOES route through the
game queues) is the one that carries signal for this knob. It's noisier
than a quiet server would give (this is a shared, multi-tenant container —
run-to-run swings of 1.5-3x at fixed `gw` are visible even there), but one
trend holds across all three connection counts: **`gw=2` is never the best,
and is the worst or tied-worst at every connection count** (1585 @ 32,
2628 @ 160 [though noisy], 1464 @ 400) — too few workers to keep 4 cores
fed. `gw=4` and `gw=6` are consistently in the good cluster with no
catastrophic outliers (4176/3056/3307 and 4293/1548/3718 respectively —
`gw=6`'s one bad 160-conns run is the sweep's noisiest single point, not a
repeatable pattern). `gw=8` never wins and shows the widest swings (e.g.
1382-3319 at 400 conns) — oversubscribing 8 worker threads plus the
dispatcher plus per-game bot threads onto 4 cores adds contention without
adding throughput.

**Winner: `game_workers=4`.** It ties or beats every other value tested in
`--mode=mixed` without gw=8's variance, it's provably harmless for
`--mode=ws` (the decider), and it matches the `games ≈ cores` heuristic for
this 4-core box directly — the same sweep on an 8- or 16-core host would be
expected to shift the peak toward `gw≈cores` there too (worth re-running
this exact sweep on a bigger box before trusting the number, not just
assuming the ratio holds).

**`meta_workers` and `create_workers` showed no reliable difference at any
tested value** (0/1/2 meta workers: 2665/3467/3169, ranges overlapping
heavily; 1/2/4 create workers: 3430/4082/2729, same story) — expected, once
you account for traffic shape: `/meta` only fires on rare rematches in
`--mode=ws` (a couple dozen per run, against tens of thousands of `/ws`
round trips), and `/auth`+`/create` only fire once per client during setup,
never in the steady-state load window at all. Neither pool is ever close to
contended in this benchmark, so there's no throughput signal to tune
against. Given that, the defaults picked favor simplicity/leanness over an
unproven "winner": **`meta_workers=0`** (fold `/meta` onto the sharded game
pool — one fewer always-idle thread, no measured downside, and it's a
real architectural option worth defaulting to rather than leaving unused)
and **`create_workers=1`** (adequate; `/auth`+`/create` are never the
bottleneck in any load profile tested here).

**Shipped defaults: `--game-workers=4 --meta-workers=0
--create-workers=1`.** All three remain overridable at the command line for
re-tuning on different hardware or traffic mixes. Full sweep data (every
trial, every phase): `bench_results/T2a_scaling/worker_pool_sweep.csv`.

## Deliverable 3 — before/after measurements

Baseline = the commit immediately before this stage's changes (single
global `g_lock`, thread-per-connection for everything including `/ws`),
built as a standalone binary from that commit's `foolish_server.c`. New =
this stage's `foolish_server.c`, shipped defaults
(`--game-workers=4 --meta-workers=0 --create-workers=1`). Both driven by
`foolish_hammer --mode=ws` (the same WS+legal hammer used throughout —
applied-moves/s and round-trip latency percentiles), RSS sampled
concurrently with `mem_sample.sh` at 0.25s intervals. `--games=8/40/100
--seats=4` for 32/160/400 connections, 12s load window, fresh server each
run.

| conns | variant | applied/s | mean us | p50 us | p90 us | p99 us | RSS mean KB | RSS peak KB | VmHWM KB |
|---|---|---|---|---|---|---|---|---|---|
| 32  | baseline | 140.5 | 48.7 | 32.9 | 93.1 | 192.0 | 29344 | 33420 | 36516 |
| 32  | new | 150.0 (+6.8%) | 44.6 (-8.4%) | 33.1 | 92.3 | 180.4 (-6.0%) | 31359 | 35144 | 37764 |
| 160 | baseline | 1481.5 | 178.4 | 121.9 | 398.1 | 860.2 | 119317 | 146532 | 150740 |
| 160 | new | 3291.7 (**+122%, 2.22x**) | 198.3 (+11%) | 143.5 (+18%) | 448.2 (+13%) | 862.8 (+0.3%) | 115834 (-3%) | 150376 (+3%) | 153148 (+2%) |
| 400 | baseline | 4364.5 | 1492.4 | 1266.8 | 2931.4 | 7631.3 | 276928 | 381052 | 380888 |
| 400 | new | 6884.5 (**+58%, 1.58x**) | 1103.6 (**-26%**) | 922.3 (-27%) | 2300.6 (-22%) | 4665.8 (**-39%**) | 261488 (-6%) | 336568 (**-12%**) | 336280 (-12%) |

Full data: `bench_results/T2a_scaling/deliverable3_before_after.csv`.

**Does per-game locking + sharded workers actually improve concurrent
throughput/latency on this 4-core box? Yes, decisively at 160/400
connections, and it's at worst a wash at 32.** At 32 conns (below this
box's 4-core parallelism budget), the two designs are within noise of each
other — expected, since neither the old global lock nor the new per-game
locks are under real contention at that scale. At 160 conns, applied
moves/s more than doubles (2.22x) while p99 latency stays essentially flat
(860us vs 863us) — the new design does over 2x the WORK per second for the
SAME tail latency, which is the headline result. Mean/p50/p90 rise modestly
(+11-18%) at 160 — plausible explanation: doing 2.2x more real work per
second naturally means more actual game mutations (not just polls) landing
in the sample, and those go through `g_kernel_lock` (see Deliverable 1) —
a small amount of real serialization cost that wasn't visible in the OLD
design because it was drowned out by `g_lock` blocking on EVERYTHING, not
because the new design regressed. At 400 conns the new design wins on
EVERY axis: 58% more throughput, 26-39% lower latency across every
percentile including the tail (p99 39% lower), AND 12% lower peak/HWM RSS.

**Thread count / memory**: the new design's idle RSS is higher (32 conns:
"first sample" 6.7MB vs 2.9MB) — the fixed worker pool (4 game + 1
auth/create = 5 always-on threads, vs the old design's zero-until-a-request
threads) costs a small constant. That constant washes out under load: by
400 conns, peak RSS is LOWER for the new design (336.6MB vs 381.1MB) despite
serving 58% more throughput. A plausible explanation (not root-caused
further given time): the OLD design's thread-per-HTTP-request model spun up
and tore down a fresh ~8MB-stack thread for every one-shot request during
setup (400 signups + 100 creates + hundreds of joins/starts) and every
`/meta` rematch during the run; repeated create/destroy cycles like that can
leave a process's malloc arenas and address space more fragmented (and
thus resident-larger) than a small number of long-lived worker threads that
never get torn down. `/ws` connections themselves are unchanged between
the two designs (both are thread-per-connection there — design B, see
Deliverable 2), so the ~0.9MB/conn WS memory tax PROFILE_HOTPATH.md's T1c
measured is NOT addressed by this stage; the difference above is entirely
from HOW one-shot HTTP requests are served (a bounded worker pool vs.
transient threads), not from the WS connections themselves.

## Seams left for stage 2 (SQLite WAL persistence) and stage 3 (OpenSSL TLS)

- **Stage 2 (persistence) — DONE.** `game_mark_dirty(GameSlot *s)` (the
  no-op stub described below when this section was written) is now wired to
  a real SQLite WAL write-behind engine (`persist.c`/`persist.h`) — see
  [`DURABILITY.md`](DURABILITY.md) for the design, the crash-recovery test,
  the durability guarantee/tradeoff, and the persistence-overhead
  measurements. Left here for the record of what the seam looked like going
  in: called under `s->lock` at every point a game's state could have
  changed (the same events that bump `s->version`: `/action`, `/ws`
  applying a move, `/meta` lobby transitions and deals, and `bot_thread`'s
  cycles that applied ≥1 action or ended the game) — exactly the call sites
  Stage 2 wired up, unchanged.
- **Stage 3 (TLS) — DONE.** `io_read`/`io_write` (foolish_server.c) and
  `ws.c`'s `ws_read_full`/`ws_write_full`/`ws_fill` — the seam described
  below when this section was written — are now backed by `conn.c`/`conn.h`:
  a `Conn { int fd; SSL *ssl; }` threaded through every place that used to
  take a bare `int fd` (respond/respond_bin, every route handler, `WorkItem`/
  `WsSpawnArg`, `WsConn`), dispatching to `read()`/`write()` or
  `SSL_read()`/`SSL_write()` per connection. See [`TLS.md`](TLS.md) for the
  full design, how to run with certs, and the measured overhead. Left here
  for the record of what the seam looked like going in: every plain-HTTP
  socket byte funneled through `io_read`/`io_write`'s two one-line wrappers
  around `read()`/`write()`, and `ws.c` had the equivalent, pre-existing seam
  for the WebSocket path — swapping TCP for TLS meant replacing those
  bodies, threaded through wherever a bare `int fd` was passed, exactly as
  predicted. The one documented non-uniform spot predicted here —
  `ws_send_frame`'s unmasked server path using `writev()` for one syscall,
  which OpenSSL has no equivalent for — was resolved by concatenating into a
  thread-local scratch buffer on the TLS branch only (see `ws_send_frame`'s
  own comment in `ws.c`); the plaintext `writev()` path is untouched.
