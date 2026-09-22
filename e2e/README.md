# e2e - codified checks across client + server

These run the **actual deployed code** - the real `server/impls/supabase/functions/_shared/*`
server modules (`executeWithGameLock`, the action handlers, `commit_game` and the
bot-lease plpgsql, `broadcastAnimationEvents`) and the real client reconciliation
(`src/state/clientReconcile.ts`, the exact functions `ServerContext` /
`AnimationContext` import) - against a real Postgres.

The only substitution is the part of the platform we can't run locally
(PostgREST + Realtime), replaced by one small `pg`-backed adapter
(`adapters/supabase.ts`, ~180 lines) implementing exactly the supabase-js surface
the server uses. The `commit_table` / lease plpgsql is seed.sql's own code,
running in real Postgres. Nothing about gameplay is mocked.

How the real server code loads under Node: `e2e/tsconfig.json` maps the three
Deno/JSR specifiers (`jsr:@supabase/supabase-js`, the edge-runtime type import, and
the `deno.land` http server) to local shims, and `harness.ts` defines the `Deno` /
`EdgeRuntime` globals - so the `_shared` modules import unmodified.

## Run

```bash
# one-time: Postgres + the role/db the adapter expects
apt-get install -y postgresql && service postgresql start
sudo -u postgres psql -c "CREATE ROLE stress LOGIN SUPERUSER PASSWORD 'stress';"
sudo -u postgres psql -c "CREATE DATABASE foolish OWNER stress;"

npm install            # pg is a devDependency
npm run test:e2e
```

Connection is configurable via `E2E_PGHOST/PGPORT/PGUSER/PGPASSWORD/PGDATABASE`.

**This is not the supabase container.**
The adapter talks to `127.0.0.1:5432` as `stress`/`stress` on database `foolish`.
`supabase start` is not enough, and the way it fails is misleading: you get
`relation "games" does not exist`, which means "no e2e Postgres", not "missing
migrations" - the supabase DB on `:54322` has a perfectly good `games` table,
which is exactly what makes the error read the wrong way.
CI stands the real one up as a `postgres:16` service; locally:

```bash
docker run -d --name foolish-e2e-pg \
  -e POSTGRES_USER=stress -e POSTGRES_PASSWORD=stress -e POSTGRES_DB=foolish \
  -p 5432:5432 postgres:16
```

Nothing to pass for speed: the harness turns the durability off itself, on the
server, the first time it connects - see "The database is fast on purpose".

### The container has to be the one that answers

Two Postgres servers can hold `:5432` at once, and for months two of them did.
A Mac running `brew services start postgresql@15` alongside the container above
has both listening, and `lsof -nP -iTCP:5432 -sTCP:LISTEN` shows exactly how:

```
postgres    1699 alex  IPv6  TCP [::1]:5432 (LISTEN)          # homebrew postgresql@15
postgres    1699 alex  IPv4  TCP 127.0.0.1:5432 (LISTEN)      # homebrew postgresql@15
com.docke  86434 alex  IPv6  TCP *:5432 (LISTEN)              # foolish-e2e-pg (postgres:16)
```

A specific bind beats a wildcard one, so `127.0.0.1:5432` reached **PostgreSQL
15 (Homebrew)** while this file and all three CI workflows said `postgres:16`.
Nothing failed; the suite passed on the wrong major version, and every local
timing in the repo had been measured on a server nobody meant to measure.

So the major version is now an assertion.
The harness checks it once per process, before it creates the first database,
and a mismatch is a failed run with the `lsof` line and both fixes in the
message rather than a silent 20% difference in everyone's numbers.
The two fixes are: stop the other server (`brew services stop postgresql@15`),
or keep it and give the container a port of its own -

```bash
docker run -d --name foolish-e2e-pg ... -p 55432:5432 postgres:16
E2E_PGPORT=55432 npm run test:e2e
```

The expected major lives in `harness.ts` as `EXPECT_PG_MAJOR`, one constant, and
`E2E_PG_MAJOR` overrides it for a deliberate run on another major.
When CI's `postgres:16` service moves, CI itself goes red until the constant
moves with it, which is the point - the workflows and the check cannot drift
apart quietly.

**Never run two suites at once** - `resetDb()` TRUNCATEs shared tables and will
corrupt the other run.
Don't rebuild `bots.wasm.gz` while a suite is running either; the suite loads the
`.gz` off disk, and the pre-hooks (`npm run wasm`) rewrite it in place.

## One database per file, and the two lanes that follow

Every Postgres-backed file opens by applying `e2e/schema.sql` + the production
`seed.sql`, and `schema.sql` starts by DROPping the `public` / `auth` / `realtime`
schemas.
While the whole suite shared one database that reset was a wrecking ball swung
through whatever else was mid-transaction, which is what forced
`--test-concurrency=1`: 81 files, one at a time, 27 minutes on the critical path
of every PR.
The gameplay was never the problem - `concurrent_games.test.ts` exists to show
that 24 real games on ONE Postgres neither deadlock nor corrupt each other.

So the shared thing is gone rather than serialised around.
`E2E_PGDATABASE` (default `foolish`) is now only the maintenance database that
`CREATE DATABASE` / `DROP DATABASE` are issued from; it holds no app tables.
Each test file owns `e2e_<file>`, created by `applySchema()` and dropped when the
file ends.
A per-file DATABASE rather than a per-file schema, because `seed.sql` names
`auth.users` and `realtime.messages` the way production does, and rewriting those
names to fit an isolation scheme is exactly the drift the harness refuses to
introduce.

`scripts/run_e2e.mjs` then runs two lanes, and membership is derived from the
import rather than from a list anyone has to maintain (`harness.ts` is the only
door to the pool):

| lane | files | width | bounded by |
| --- | --- | --- | --- |
| pure | the 73 that never import `harness.ts` | `availableParallelism()` | CPU |
| db | the 37 that do | 5 (`E2E_DB_CONCURRENCY`) | Postgres connections |

The db lane's width is connection arithmetic, not a guess.
`max_connections` is 100 on a stock Postgres - local dev and the CI `postgres:16`
service alike - with 3 reserved for superusers, which leaves 97 slots.
Two suites drive contention deliberately and keep a pool as wide as the race they
run (24 games for `concurrent_games`, 30 simultaneous acquires for `lease`), since
a narrow pool would make them queue on the POOL instead of on Postgres, which is
the thing they exist to measure; every other file peaks in the low single digits
and gets 8.
Worst case is therefore `24 + 30 + 8 + 8 + 8` plus one short-lived admin
connection per file = 83, inside the 97.
Measured peak across a real run at the old width of 4: 57, and the realised peak
sits well under the worst case because the expensive-first order below starts
`concurrent_games` long before `lease`, so the two wide pools rarely overlap.
The sizes live in `adapters/supabase.ts` next to the pool they size.

`E2E_LANES=serial` runs the lanes one after the other instead of overlapped, which
is what to reach for when a red log is unreadable.

### The expensive files start first: `e2e/FILE_ORDER`

Both lanes hand node:test a list, and node starts files in that order as slots
free.
The list used to be alphabetical, which is a scheduling decision nobody made on
purpose: the longest file in each lane sorted under "s" and started when the lane
was two thirds done, with nothing left to overlap it.
So the runner reads `e2e/FILE_ORDER`, a plain-text list of every test file most
expensive first, and starts each lane's files in that order.

The file is generated by `node scripts/run_e2e.mjs --write-order` and is never
hand-edited.
That flag preloads `scripts/e2e_order_probe.mjs` into every per-file child, which
asserts nothing and prints nothing; at exit it records the file's elapsed and CPU
milliseconds, and the runner writes the merged list when the run ends.
It is ranked on CPU time rather than elapsed.
Elapsed re-measures the contention of whichever run wrote the file - on a busy
machine it contains every second the file spent queued behind seven others for a
core - while CPU is the work the file actually did, reproducible across runs and
across machines.
Elapsed is kept beside it in the file so a reader can see which files are waiting
rather than working.

It is a hint and nothing else.
A file missing from it simply runs first (an unmeasured file may be the next long
one), a listed file that no longer exists is ignored, and a stale order costs
wall-clock and can never cost correctness.
The runner's header line prints how many files are unranked, which is how a
reader finds out the list has drifted behind the tree.

### The database is fast on purpose

Every pooled session runs with `synchronous_commit=off`, carried in the pool's
connection `options` (`SESSION_OPTIONS` in `adapters/supabase.ts`) beside
`TimeZone=UTC`.
On the dev Postgres, 200 one-row commits take 269 ms with it on and 2.4 ms with
it off: 1.35 ms of fsync per COMMIT against 0.012 ms.
The suite commits tens of thousands of times, since every move is a `commit_game`
CAS.

It is worth less than it looks, and the honest number belongs here rather than in
a commit message: the db lane run on its own went from 59.7 s to 56.3 s.
(That pair was measured before the section above, so it was measured on the
Homebrew 15 that used to win the port - the ratio is the point, not the seconds.)
The lane's time is round trips, not fsync - 191,198 Postgres queries per run,
86 s of aggregate in-query time - and 3.4 s is what removing the fsync from each
of them is worth.
It is kept because it is free and it is correct, not because it was the answer.

This is not a weaker database.
`synchronous_commit` changes no visibility, no isolation, no locking and no
constraint - only whether COMMIT waits for the WAL to reach the platter - so
every CAS race, deadlock and card-conservation assertion is the same experiment
it was.
What it gives up is surviving a power cut, of a database the file creates when it
starts and drops when it ends.
Being a session option it also holds for a connection opened before the server
tuning below has been applied, and for a server that refuses to be tuned at all.

What a session option does not reach is `CREATE DATABASE`'s own checkpoint, the
WAL writer, and the full-page image Postgres writes for every page first touched
after a checkpoint.
Those are server settings, so the harness sets them on the server: the first
Postgres-backed file of a run issues

```sql
ALTER SYSTEM SET fsync = off;
ALTER SYSTEM SET full_page_writes = off;
ALTER SYSTEM SET synchronous_commit = off;
SELECT pg_reload_conf();
```

and every later file finds them already off and issues nothing.
`ALTER SYSTEM` + reload rather than `docker run -c` because all three settings
are SIGHUP-settable and that is the one mechanism that works in both places a
suite runs: a GitHub Actions `services:` block takes no command arguments, so
`-c fsync=off` is a thing only a local `docker run` could ever pass.
Doing it from the harness means a fresh clone and CI get the same server with
nothing to remember, and `npm run test:e2e` is still the whole setup.

It is skipped unless `E2E_PGHOST` is a loopback address.
A server on this machine is one this checkout stood up; a server somewhere else
is exactly the case where writing `fsync=off` into its `postgresql.auto.conf`
would be the wrong thing to do, and these settings must never go near a real
database.
A refused `ALTER SYSTEM` (a non-superuser role, a managed server) prints one
line and the suite runs on: the tuning costs seconds, never correctness.

The measurement, on the `postgres:16` container, db lane alone
(`E2E_LANES=serial`), on top of the per-session `synchronous_commit=off` that was
already there:

| | db lane |
| --- | --- |
| the server as the image ships it | 50.5s |
| `fsync`, `full_page_writes`, `synchronous_commit` off server-wide | 46.6s / 46.1s |
| + `wal_level=minimal`, `max_wal_senders=0`, `shared_buffers=512MB`, `max_wal_size=2GB` | 45.7s |
| + the data directory on a tmpfs ramdisk | 46.1s |

The three in row two are the whole win, and the last two rows are why nothing
else is configured here.
The postmaster settings are worth about half a second - inside the noise - and
cannot be set on a CI service container at all, since they need a restart the
`services:` block cannot perform.
The ramdisk measured at nothing, which is the expected answer once the fsyncs are
gone: what is left is round trips, and a round trip does not touch the disk.
Whole suite, overlapped lanes, 542 tests, six runs in one adjacent block: 87.7s
mean before and 83.2s after.
Only runs from the same block compare on this machine - the same tuned suite an
hour later, with nothing else on the cores, was 77.8s - which is why the db-lane
table above is the measurement and the whole-suite pair is the corroboration.

### A killed run is now inert, not poisonous

It used to be the repo's standing trap: `e2ePool` held up to 40 connections, a
suite killed part-way (Ctrl-C, a crashed process, a timed-out `xcodebuild` in
another terminal) left them open holding the shared schema, the next run's
`applySchema` half-applied, and every suite after it died on
`relation "games" does not exist` - a failure that looked like a code regression
and got worse on each re-run.

That cannot happen now, because cleanup is on ACQUIRE, not only on release.
`applySchema()` opens with

```sql
DROP DATABASE IF EXISTS e2e_<file> WITH (FORCE);
CREATE DATABASE e2e_<file>;
```

and `WITH (FORCE)` terminates whatever backends the killed run left behind
instead of failing on them.
A leaked database belongs to exactly one file, no other file reads it, and that
file destroys it before it uses it.
This is also why the name is derived from the FILE and not from a random or
clock-derived id: the determinism gate forbids entropy under `e2e/`, and a
deterministic name is what makes the leak self-healing rather than unbounded.

If you want to sweep them anyway:

```bash
psql -h 127.0.0.1 -U stress -d postgres -At \
  -c "SELECT 'DROP DATABASE '||datname||' WITH (FORCE);' FROM pg_database WHERE datname LIKE 'e2e\_%'" \
  | psql -h 127.0.0.1 -U stress -d postgres
```

`E2E_DB_PREFIX` renames the whole set, for running two checkouts against one
Postgres at once.

### Never `await import()` inside a loop

The suite's 27 minutes were not mostly gameplay, or Postgres, or the V8 coverage
instrumentation (that is ~20s of it).
They were module resolution.

Under the e2e runner's TypeScript loader a dynamic `import()` re-runs the whole
resolve hook chain on every call - about 1.9ms - even though the module is
already in the registry and nothing is re-evaluated.
A hoisted reference to the same module object is free.
`commitGame` alone lazily imported eight modules per move, `loadCompleteGame`
two, and `packed_review_gaps` five inside its per-move loop, so the fuzzers were
paying tens of milliseconds a move to look up paths they had already looked up
thousands of times.
`pass_parity` went from **357s to 14s** on that alone.

The server's laziness itself is deliberate and stays - a create/lobby cold start
must not pull the rules-wasm embed.
What changed is that each lazy import is memoised (`lazy()` in
`_shared/adapter/utils.ts`), so it defers exactly as before and resolves once.

Two dynamic imports under `e2e/` are deliberate and must NOT be hoisted:
`belief_logs_wiring.test.ts` and `lobby_add_bot.test.ts` import *after*
installing a `mock.module`, and a static import would bind the real module
before the mock exists.
Both say so at the call site.

### What is left, and where it actually goes

After the loader fix the suite is no longer dominated by any one mistake.
Measured back to back on an 8-core Mac, 542 tests passing at every row:

| | wall clock |
| --- | --- |
| before, alphabetical order, db lane at 4 | 124.1s |
| expensive-first, db lane at 5, `synchronous_commit=off` | 95.6s |
| + `security_hidden_info` split into one file per seat count | 79.2s |
| + `belief_logs` and `state_codec` ported to `c/tests` | 72.5s |

Every row above was measured on the Homebrew PostgreSQL 15 that used to win the
port, so they compare with each other and with nothing else.
On the `postgres:16` container the suite actually targets, same tree, same 542
tests, one adjacent block of six runs before and after the server tuning: 87.7s
and 83.2s, and 77.8s for the tuned suite again on a quiet machine.

The 15s between those two servers is not the major version, and it is worth
knowing what it is, because it is the largest single number left in the suite.
Measured with 20,000 back-to-back `SELECT 1` on one warm connection:

| server | per round trip |
| --- | --- |
| Homebrew PostgreSQL 15, native, loopback | 40.5 us |
| `foolish-e2e-pg`, `postgres:16`, `-p 5432:5432` | 165 us |
| the local Supabase database, `postgres:17`, `-p 54322:5432` | 320 us |

Every container on Docker Desktop for Mac is behind a userspace port forwarder
and a VM, and that is the whole 124 us.
At 191,198 queries a run it is ~24s of pure latency in the db lane, most of it
hidden behind the pure lane and about 15s of it not.
The same container on a Linux CI runner publishes its port into the host's own
network stack with no VM in the way, so this is a local tax and not a CI one.

A developer who wants the 15s back should run a NATIVE PostgreSQL 16
(`brew install postgresql@16`) rather than the container, and stop the container
so there is only one server on the port.
The check is on the major version, not on the container, so a native 16 satisfies
it exactly as the image does - and a native 15 now fails loudly instead of
quietly answering for it.

The shape behind those numbers is worth keeping, because it says which further
ideas are worth having.
The pure lane alone was 67.1s and the db lane alone 56.3s, so the two lanes
together cost more than either but less than the 123s of running them back to
back, and no width setting fixed the starvation between them.
The db lane is not write-bound and never was: it issues 191,198 Postgres queries
per run and spends 86s in them, against 73s of node CPU, so it is round trips.
The pure lane is not waiting on anything at all - it is the C kernel, compiled to
wasm, playing real games.

Which is why the only two things that ever moved the number were making the
longest file cheaper and turning the longest file into several files.
Ordering moves the tail; it cannot move the floor.
`security_hidden_info` was 80s of three independent seat-count scenarios in one
process and is now three files, 30.7s together with every assertion unchanged.
`belief_logs` and `state_codec` drove the C kernel and asserted properties of the
C code, so the bulk of each now lives in `c/tests/tests.c` with a small wasm case
left behind in the TypeScript file; together they were 146s and are now under 5s.
That work did not evaporate - `c/build/cnitro_tests` went from 3.8s to 27.7s, and
it has now been profiled rather than assumed.
Timing every one of the 204 tests individually: `belief_logs` is 23.5s of the
27.3s and everything else in the file, `state_codec`'s 3,497 round-trips
included, is 12.8ms.
Inside those 23.5s there is no per-decision setup, no rebuild and no allocation
to find - 3,430 octogen decisions at 6.9ms each, half of them the run with the
memory taken away that IS the experiment, and `/usr/bin/sample` puts all of it
in `sim_solve_rec` / `sim_gen_moves` / `cover_assign`, which is octogen thinking.
The one thing the profile did find was not in the test at all: 15% of the whole
binary was inside `___tlv_get_addr`, because Darwin has no local-exec TLS and the
default native build kept the `_Thread_local` qualifier that only `make OMP=1`
needs.
Neutralising it there (`c/Makefile`, the same `-D_Thread_local=` the wasm module
has always used) took the suite to 23.8s with all 5,761 checks passing and every
counter in its output identical.
What is left is genuine Monte Carlo: the only remaining lever is fewer decisions,
and that is sample size, not waste.

## Seeds

No suite draws from `Math.random` - `scripts/check_determinism.mjs` fails CI over
`e2e/`, `sdk/` and `server/` if one does, because the product's rule is one crypto
draw per game, at the deal, with everything after it derived from that seed.
A suite that shuffles unseeded breaks the same rule from the outside: every run is
a different experiment, and a red one hands the reader no repro.

Seeded suites draw from `helpers/rng.ts` (`suiteRng`), which resolves its seed as
`E2E_SEED_<SUITE>`, else `E2E_SEED`, else the fixed default, and prints the one it
picked.
The seed is named in the failure messages too, since that is the line that gets
pasted.

```bash
E2E_SEED_REPLAY_CODEC=12345 npm run test:e2e     # one suite, one experiment
E2E_SEED=12345 npm run test:e2e                  # every suite
for s in $(seq 1 50); do E2E_SEED=$s ... ; done  # widen the search deliberately
```

Seeding narrows nothing: the trial counts are what they were.
The five Postgres-backed suites still deal from the live crypto path, exactly as
production does, so their seed reproduces the move choices but not the deal.

## What's checked

| file | uses real… | asserts |
| --- | --- | --- |
| `server.test.ts` | executeWithGameLock + handlers + commit_game + broadcast | card conservation (sequential & under contention); every broadcast carries a strictly-increasing version |
| `cover.test.ts` | the cover handler | same-rank double-tap rejects gracefully (no `SEVERE` 500); the other same-rank attack still coverable |
| `lease.test.ts` | the bot-lease plpgsql | exactly-one acquire; TTL recovery; stale-token fencing |
| `reconcile.test.ts` | real broadcasts → real client gate + table merge | client converges to the authoritative table under heavy reordering |
| `client.test.ts` | `clientReconcile` (the deployed client logic) | no hand swaps/dupes/table-cards; trust-incoming table; version gate; optimistic-overlay resync |
| `concurrent_games.test.ts` | many real games on one Postgres | no deadlock, no cross-game corruption (answers "is the parallel deadlock a real bug?" - it isn't). The per-file isolation is per FILE, never per game: all 24 games still share this file's one database, which is the whole point of the test |
| `pool_teardown.test.ts` | the harness pool and its teardown | `pool.end()` resolves only after every pooled connection has closed, so the teardown's `DROP DATABASE ... WITH (FORCE)` never terminates a live backend (that FATAL used to surface as an uncaughtException that turned a green file red) |
| `fuzz.test.ts` | the real validation+handler dispatch + CAS | adversarial/illegal/malformed input never duplicates or loses a card, illegal moves are rejected, the server survives hostile payloads (found + fixed a card-duplication exploit) |
| `rearrange.test.ts` | the real `handleRearrangeHand` + CAS | duplicate/garbage index lists are rejected (found + fixed a card-cloning exploit); a real permutation conserves cards |
| `meta.test.ts` | the real consolidated `meta` handlers + CAS | start/add-bot/exit/continue behave correctly through one endpoint |
| `replay_codec.test.ts` | the real engine + replay encode/decode codec | engine-played games round-trip byte-exact through encode → serialize → decode (plus extras names/timing and the replay-screen view builder). Pure codec test - needs no Postgres. |
| `wasm_engine.test.ts` | the C rules kernel (WASM) behind the _shared API | production deck sizes (5+ → 52), card conservation through full kernel games, the retained TS projections (canCover/game_done/next-player/shouldBotAct) never drift from the kernel, hostile inputs reject with production messages. Pure kernel test - needs no Postgres. |

The latency-sweep conclusions are folded into deterministic checks in
`client.test.ts` ("reordering" / "disconnect").
