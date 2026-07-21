# Server durability — SQLite WAL write-behind persistence ("Stage 2")

Production-hardening stage 2 of 3 (durability) for
`server/impls/native/foolish_server.c`. Stage 1 (per-game locks + work-queue
routing, "T2a") is done — see [`SERVER_SCALING.md`](SERVER_SCALING.md).
Stage 3 (OpenSSL TLS / WSS+HTTPS) is not done here; this file documents the
seam left for it in the same spot Stage 1 left it (unchanged by this stage).

Everything below touches only `server/impls/native/` — two new files
(`persist.c`/`persist.h`), `foolish_server.c` (wiring + the blob codec),
`foolish_hammer.c` (one grep-able debug line, see "Testing" below),
`Makefile`, `.gitignore`, and this file. `c/src/*` (the kernel) is untouched
and read-only, same constraint every stage in this series has followed.

## The problem

Before this stage, `foolish_server.c` was exactly what its own header
comment says: "state is RAM-only." A crash (`kill -9`, an OOM kill, a
container restart) lost every game and every signed-up user, instantly and
totally. `game_mark_dirty(GameSlot *s)` existed as a no-op stub, called
under `s->lock` at every point a game's state could change — Stage 1 left
it there on purpose as the seam for this stage.

## Design: write-behind, async, batched

**Engine and mode**: SQLite (the only crash-safe embedded DB actually
installed on this box — LMDB/RocksDB are not present, and the task was
explicit not to try adding them), one file, **WAL journal mode +
`synchronous=NORMAL`**. This is the documented "mostly in memory, persist
when you get a chance, still crash-safe" sweet spot: WAL means readers never
block writers and a commit is one sequential append to the WAL file instead
of a full B-tree rewrite; `synchronous=NORMAL` means SQLite does not force
an `fsync` on *every single* commit (full `FULL` sync would), while still
guaranteeing a committed transaction survives a **process crash** — the
guarantee this stage is built around. What `NORMAL` does *not* guarantee is
survival of true OS-level power loss / an unclean host shutdown at exactly
the wrong instant (SQLite's own docs describe this precisely: "synchronous
is NORMAL... the database is safe from an application crash... but not
necessarily against a power failure or hard reset"). That distinction is
exactly right for this project's contract: a `kill -9` is the crash this
stage's own test simulates; a data-center power outage is out of scope.

**Two durable tables**, `games` and `users`, same schema:

```sql
CREATE TABLE games(id TEXT PRIMARY KEY, blob BLOB NOT NULL, updated_us INTEGER);
CREATE TABLE users(id TEXT PRIMARY KEY, blob BLOB NOT NULL, updated_us INTEGER);
```

**One dedicated persistence thread owns the one `sqlite3*` connection** —
SQLite handles aren't safe to share across threads without serializing
every call yourself, so instead of adding a second lock around every
`sqlite3_*` call, exactly one thread ever touches the connection, for its
entire lifetime (`persist.c`'s `persist_thread`).

**The request path never blocks on disk.** `game_mark_dirty(GameSlot *s)`
(and the new user-side call in `h_signup`) do exactly one thing: flip one
`bool` in a fixed-size dirty bitmap (`persist.c`'s `PersistTable.dirty[]`,
sized `MAX_GAMES`/`MAX_USERS` — no growth, no allocation, marking the same
slot dirty any number of times between drains costs the same as marking it
once), under a small mutex held only long enough for that write, then
signal a condvar. That is the entire cost `/action`, `/ws`, `/meta`,
`/create`, and `bot_thread` pay — the same handful of instructions
regardless of whether the DB is fast, slow, or (in a real outage) wedged.

**The persistence thread wakes every `--persist-interval-ms` (default 75,
50-100ms is the documented sweet spot) or on a signal**, whichever comes
first (`pthread_cond_timedwait`), and for each dirty table:

1. **Snapshot phase** (`persist.c`'s `drain_and_persist`): lock the small
   dirty-set mutex just long enough to copy out which slots are dirty and
   clear their flags, then release it — microseconds, bounded by
   `MAX_GAMES`/`MAX_USERS`.
2. **Per-slot capture** (`foolish_server.c`'s `game_persist_snapshot` /
   `user_persist_snapshot`, called back into from the engine): take THAT
   ONE game's own `s->lock` (or, for a user, the small `g_registry_lock`),
   `memcpy` out a private copy of the live data, release the lock
   immediately — then, with **no lock held at all**, run the slower,
   structured serialization (`serialize_slot`/`serialize_user`) against the
   private copy. A game lock is held only for a fixed-size, bounded
   `memcpy` — never for serialization, never for SQLite I/O.
3. **One transaction**: `BEGIN IMMEDIATE; ` an upsert per dirty row (plus
   any explicit deletes, see below) `; COMMIT;` — every row from this pass
   commits atomically together, or (on a DB error) none of them do and
   they're retried the next drain (still marked live in memory either way).

Never hold a game lock during disk I/O; never hold the engine's own small
dirty-set mutex during a `memcpy`, a serialize call, or any `sqlite3_*`
call. That split is the entire point of "write-behind" here.

### A real bug this design surfaced (and fixed) — see "Helgrind" below

The very first version of `game_persist_snapshot` did
`memcpy(&snap, s, sizeof snap)` — the WHOLE `GameSlot`, including `s->lock`
and `s->cond` **themselves**, while holding `s->lock`. That's undefined
behavior: POSIX doesn't guarantee a live mutex's raw bytes are safe to
copy while another thread might be locking/unlocking that same mutex, even
from the thread currently holding it. Helgrind caught it as a real
data race. Fixed by copying only `offsetof(GameSlot, lock)` bytes — the
`used`/`id`/`game`/`owner`/`seat_user`/`seat_name`/`seat_ready` prefix
`serialize_slot` actually reads, which the struct layout keeps entirely
before `lock`.

### Deletion

`persist_delete(table, id)` exists in `persist.h`/`persist.c` and is wired
into the same drain/transaction path as an upsert — but nothing in
`foolish_server.c` currently calls it, because `g_games[]`/`g_users[]`
themselves are an insert-only store with no GC (see their own "insert-only,
no delete" comment in `foolish_server.c`, predating this stage). If that
ever changes, deleting a finished/GC'd game's row is a one-line
`persist_delete(g_game_table, s->id)` call away — the schema and the write
path both already support it honestly rather than silently dropping
deletes.

## The blob format

Versioned with a leading byte (`PERSIST_GAME_BLOB_VERSION` /
`PERSIST_USER_BLOB_VERSION`) so a future layout change can detect — and
refuse, rather than misinterpret — an old row.

**Games** (`serialize_slot`/`deserialize_slot`, `foolish_server.c`):

```
[0]                                  version
[1..2]                               state_len, uint16 LE
[3 .. 3+state_len)                   state_put(&game, VIEW_UNMASKED, ..)
next ID_LEN+1 bytes                  id
next ID_LEN+1 bytes                  owner
next MAX_PLAYERS*(ID_LEN+1) bytes    seat_user[]
next MAX_PLAYERS*24 bytes            seat_name[]
next MAX_PLAYERS bytes               seat_ready[]  (1 byte each, 0/1)
```

The `Game` itself is serialized with the kernel's own existing, exact
round-trip codec — `state_put(&g->game, VIEW_UNMASKED, ..)` /
`state_get(.., masked=0)` (`c/src/view.c`, already used elsewhere in this
server for `/state`/`/ws`) — plus the lobby/identity fields that codec
deliberately never carries (`game.h`: identity lives with the host, never
in the state blob). Worst case: 3 + 690 (`state_put`'s own documented
worst case — see `VIEW_CACHE_CAP`'s comment in `foolish_server.c`) + 13×2 +
8×13 + 8×24 + 8 = **1023 bytes**. `PERSIST_GAME_BLOB_CAP` (2048) gives real
margin, the same discipline `VIEW_CACHE_CAP` uses. In practice, measured
rows are far smaller (see the crash test output below — dealt games ran
30-70 bytes on the wire).

One deliberate scope note: `state_put`/`state_get` do not carry the game's
**log** (`g->logs[]` — used for replay/animation, not for resuming play).
A recovered game resumes exactly where play was, byte-identical on the
wire the client already reads (`/state`); its move-by-move log history
does not survive a crash. That is an existing property of the kernel's own
codec (this server already used it, unchanged, for `/state`/`/ws` before
this stage), not something Stage 2 introduces.

**Users** (`serialize_user`/`deserialize_user`): fixed-width — version byte
+ `token[33]` + `user_id[13]` + `username[24]` = 71 bytes, no length field
needed.

### The round-trip gate

`persist_self_test()` runs once at every server startup (`--no-db` or not —
it's pure in-memory, not a DB operation): builds a synthetic `GameSlot`
with non-default values in every field family (deck, battles, hands, lobby
roster, mixed human/bot seats), `serialize_slot`s it, `deserialize_slot`s
the result into a fresh struct, `serialize_slot`s THAT, and requires the
two serializations to be byte-for-byte identical — or the process exits
before it ever opens a socket:

```
persist self-test: OK (games: 376-byte round-trip byte-identical)
```

A regression here (a field silently dropped in either direction, an
off-by-one in the length header) fails loudly at startup instead of
silently corrupting or truncating every future snapshot.

## Crash recovery

On startup (`persist_start`, `persist.c`), before any worker pool or the
accept loop exists (`main()` calls it first): open the DB (or skip
straight to "disabled" for `--no-db`), ensure the schema, then — if rows
already exist — call each table's load callback **synchronously, once per
row**, with no locking needed (nothing else is running yet):

- `user_persist_load`: finds a free `g_users[]` slot, `deserialize_user`s
  the row into it, inserts it into `g_token_ht`.
- `game_persist_load`: finds a free `g_games[]` slot, `memset`s it,
  `deserialize_slot`s the row into it, `pthread_mutex_init`/`cond_init`s
  its lock/cond (never restored FROM the blob — these are runtime-only,
  reinitialized fresh, same as a brand-new game in `h_create`), inserts it
  into `g_game_ht`, and — **if the recovered game's status is
  `GAME_STATUS_PLAYING`** — calls `start_bot_loop(s)` under `s->lock`,
  exactly like a freshly dealt game. A game that was mid-play when the
  process died resumes paced bot ticks immediately on restart.

After recovery, `main()` proceeds to spawn the worker pools and start
accepting connections — the server resumes exactly where it was.

## Configuration

```
--db=<path>              default ./foolish.db — DB ON by default
--no-db                  pure in-memory, no persistence at all (tests/benchmarks)
--persist-interval-ms=N  default 75 (50-100ms is the documented sweet spot)
```

A requested (non-`NULL`) `--db` that fails to open/configure is **fatal**
(the process exits) rather than silently downgrading to in-memory — a
requested durability guarantee that can't be met should not pretend to be
met.

## Testing

### Crash-recovery test (`crash_test.sh`)

Two scenarios against one server, one `--db`:

- **[A]** a game driven entirely over `/ws` with genuinely legal moves
  (`foolish_hammer --mode=ws`, the same client `test.sh`'s own WS smoke
  test uses) — proves moves applied through the real request path survive.
- **[B]** a game dealt with 3 bots + 1 human who never calls `/action` —
  deterministically stuck in `GAME_STATUS_PLAYING` with real dealt hands
  (not a lobby stub), because the bot trampoline structurally cannot act
  for a human seat. This is the "an in-progress game, not just a finished
  or lobby one" case, and its recovery exercises the code path that
  restarts a recovered `PLAYING` game's bot thread — if that path hung or
  crashed, the `/status` read after restart would too. (Scenario B
  intentionally checks the **deal-identity header** — `state_put`'s first
  16 bytes: status/num_players/power_suit/first_attacker/defender/
  discard_len/has_flipped/flipped/good_players_mask/has_good_timestamp/
  deck_count, all fixed at deal time — rather than full-byte equality: with
  3 bots and a human who never moves, bots keep legitimately playing real
  rounds *against each other* until the human's seat comes up, so the
  mutable in-round bytes can keep changing between the "before" and
  "after" snapshots even with nothing wrong — that's correct ongoing
  gameplay, not a bug, and asserting exact equality there would make the
  test flaky over real, harmless timing.)

Sequence: drive both scenarios, **wait 1 full second (comfortably longer
than the 60ms `--persist-interval-ms` this test runs with)** so everything
is guaranteed drained into a committed transaction, capture `/status` +
`/state` for both games plus a dedicated test user's login token, **`kill
-9`** the server (no clean shutdown, no flush call — a true crash),
restart against the same `--db`, and assert:

```
PASS: [A] /status survived the crash (0)
PASS: [A] seat 0 masked /state byte-identical before vs after the crash
PASS: [A] seat 1 masked /state byte-identical before vs after the crash
PASS: [B] in-progress /status survived (1)
PASS: [B] seat 0 masked /state (deal-identity header) byte-identical before vs after the crash
PASS: [B] seat 0 /state before the crash carries real dealt data (65B >= 40B)
PASS: [B] seat 0 /state after the crash carries real dealt data (66B >= 40B)
PASS: [B] recovered game is still PLAYING (bot-thread-restart path ran without hanging/crashing the server)
PASS: crashtest_login's token still authenticates after the crash (404, not 401)

=== crash_test.sh: PASS — committed game + user state survived kill -9 ===
```

Run repeatedly (4 separate ports/runs during this stage's own validation):
PASS every time. `foolish_hammer` was extended with one grep-able debug
line (`"   dealt game[%d]: id=%s\n"` in `setup()`) so the test script can
identify exactly which game it drove without guessing — a small, generally
useful addition (any future load-test wrapper that wants to target a
specific dealt game benefits too), not test-only plumbing bolted on
awkwardly.

**What this proves, precisely**: everything the persistence thread had
committed to SQLite before the kill survives it byte-for-byte, including a
game genuinely `PLAYING` (not just `WAITING`/`GAME_OVER`) and a user's
login. **What it deliberately does not claim**: write-behind is async — a
crash landing mid-interval can lose up to `--persist-interval-ms` worth of
the most recent moves (the ones not yet drained into a committed
transaction). This test sidesteps that ambiguity on purpose, rather than
asserting a fuzzy "some moves survived": it drives load, then waits
comfortably longer than the persist interval before killing, so there IS
guaranteed-committed state to recover and a byte-exact assertion is the
right one to make. **The durability guarantee, stated plainly**: a
`kill -9` (or any process crash) loses at most the last
`--persist-interval-ms` of committed-but-not-yet-drained mutations to any
one game or user; everything drained before that survives exactly. An
unclean host power-loss (not a process crash) is outside `synchronous=
NORMAL`'s guarantee — see "Design" above.

### `test.sh` still passes

Run with `--no-db` (pure in-memory, no stray DB file):

```
── ws smoke test: PASS (238 legal moves applied over persistent WS connections)
```

...and with a real `--db` too (the same smoke test also ran, and did run,
against a live SQLite-backed server during this stage's own testing — see
"Crash-recovery test" above for the harder version of the same claim).
`.gitignore` covers `*.db`/`*.db-wal`/`*.db-shm` so a bare `make run`
(DB ON by default) never leaves a stray file tracked by git.

### Persistence overhead: DB-on vs `--no-db`

`foolish_hammer --mode=ws` (WS+legal hammer — the same tool T2a's own
scaling sweep used), 3 trials per (variant, connection count), 12s load
window each, shipped worker-pool defaults, fresh server per trial. Raw
data: `bench_results/durability/persist_overhead.csv`.

| conns | variant | applied/s (avg, range) | mean us | p50 us | p90 us | p99 us | max us (avg) |
|---|---|---|---|---|---|---|---|
| 32  | db_on | 155.0 (129.8-199.5) | 45.1 | 35.2 | 93.0  | 174.9 | 34846 |
| 32  | no_db | 124.1 (118.3-134.6) | 45.3 | 35.6 | 94.4  | 176.1 | 6441  |
| 160 | db_on | 1978.6 (1055.6-2715.8) | 147.5 | 104.6 | 327.9 | 664.7 | 26028 |
| 160 | no_db | 2365.5 (1577.0-3385.8) | 142.8 | 102.7 | 313.5 | 639.7 | 12126 |

**Latency (mean/p50/p90/p99) is within 1-5% between DB-on and `--no-db` at
both scales** — indistinguishable from noise, exactly what the design
predicts: the request path never blocks on the DB, so its latency
shouldn't move when persistence is enabled, and it doesn't.

**Throughput (applied/s) is noisy in both directions** — at 32 conns db_on
averaged higher (155.0 vs 124.1); at 160 conns no_db averaged higher
(2365.5 vs 1978.6) — and neither difference survives a look at the
per-trial ranges, which span **1.4-2.6x at fixed (variant, conns)** on
their own (this is the same "shared, multi-tenant container" box
SERVER_SCALING.md's own sweep flagged for exactly this kind of run-to-run
swing). Reported honestly rather than picking whichever direction looks
better: on this box, persistence overhead on throughput is not
distinguishable from environmental noise at 3 trials.

**One real, if small, signal**: DB-on's average *max* single-sample
latency ran noticeably higher than `--no-db`'s at both scales (34.8ms vs
6.4ms at 32 conns; 26.0ms vs 12.1ms at 160 conns) — plausibly an
occasional tail stall when a `/ws` round trip's `game_mark_dirty` call
briefly contends with the persistence thread's drain (which holds each
game's lock only for a bounded `memcpy`, per the design above, but a
`memcpy` of a ~40-50KB `GameSlot` is not instant, and SQLite's own
`COMMIT` can itself briefly stall on an fsync). This is a tail-latency
observation, not a throughput or median-latency regression — noted here
rather than hidden, not investigated further at 3 trials' worth of
evidence.

### Helgrind

`valgrind --tool=helgrind --history-level=approx` against `./foolish_server
<port> --db=<scratch> --persist-interval-ms=30` (lower than the 75ms
default specifically to run the drain cycle more often per second of
"brief" load), driven by `foolish_hammer --mode=mixed` then `--mode=ws`
back to back, `kill -TERM` on the valgrind PID to end the capture (same
technique T2a's own Helgrind run used). 3 runs. Full digest:
`bench_results/durability/helgrind_summary.txt`.

**Found and fixed a real race**: the `game_persist_snapshot` bug described
under "Design" above — `memcpy`ing `s->lock`/`s->cond`'s own bytes while
holding `s->lock`, racing against ordinary request-handling threads'
lock/unlock traffic on the same mutex. Fixed by copying only the fields
before `lock` in the struct (`offsetof(GameSlot, lock)` bytes). **Zero**
occurrences of that race across all 3 post-fix runs — the dirty-set +
snapshot handoff this check exists to verify is race-free.

**One remaining warning, same single location every run**: a "dubious:
associated lock is not held" report whose stack is entirely inside glibc/
pthread internals reached through `persist_thread`'s own
`pthread_cond_timedwait` call — never through either of this codebase's two
actual signal call sites (`persist_mark_dirty`, `persist_delete`, both of
which hold the mutex for their entire `pthread_cond_signal` call, verified
by inspection). It appears only at/around the `kill -TERM` that ends each
capture, never during ~30-40s of combined steady-state traffic across the 3
runs (hundreds of ordinary 30ms wait cycles with no report), and did not
reproduce in an isolated minimal repro of the same wait/signal/SIGTERM
pattern. This is `pthread_cond_timedwait`'s first-ever use in this codebase
(Stage 1's `bot_thread` uses only plain, untimed `pthread_cond_wait`, and
was fully Helgrind-clean) — assessed as a benign Helgrind/glibc(2.34+ NPTL
condvar rewrite)-interaction artifact tied to the TIMED variant's internal
teardown near a signal-interrupted shutdown, not a real race in this
program's logic. Left visible (not suppressed) rather than hidden behind a
suppression file — see the digest for the full reasoning.

## Seam left for Stage 3 (TLS)

Unchanged by this stage, exactly as Stage 1 left it: `io_read`/`io_write`
(`foolish_server.c`) and `ws.c`'s `ws_read_full`/`ws_write_full`/`ws_fill`
are still the only place plain-socket bytes flow. Stage 2 added no new
socket I/O of its own (the persistence thread never touches a client
connection — it only ever talks to SQLite), so there is nothing new for
Stage 3 to route through TLS beyond what Stage 1 already documented in
`SERVER_SCALING.md`'s own "Seams left" section.
