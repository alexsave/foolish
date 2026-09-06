# e2e — codified checks across client + server

These run the **actual deployed code** — the real `server/impls/supabase/functions/_shared/*`
server modules (`executeWithGameLock`, the action handlers, `commit_game` and the
bot-lease plpgsql, `broadcastAnimationEvents`) and the real client reconciliation
(`src/state/clientReconcile.ts`, the exact functions `ServerContext` /
`AnimationContext` import) — against a real Postgres.

The only substitution is the part of the platform we can't run locally
(PostgREST + Realtime), replaced by one small `pg`-backed adapter
(`adapters/supabase.ts`, ~180 lines) implementing exactly the supabase-js surface
the server uses. The `commit_game` / lease plpgsql is the verbatim migration code,
running in real Postgres. Nothing about gameplay is mocked.

How the real server code loads under Node: `e2e/tsconfig.json` maps the three
Deno/JSR specifiers (`jsr:@supabase/supabase-js`, the edge-runtime type import, and
the `deno.land` http server) to local shims, and `harness.ts` defines the `Deno` /
`EdgeRuntime` globals — so the `_shared` modules import unmodified.

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
| pure | the ~60 that never import `harness.ts` | `availableParallelism()` | CPU |
| db | the ~21 that do | 4 (`E2E_DB_CONCURRENCY`) | Postgres connections |

The db lane's width is connection arithmetic, not a guess.
`max_connections` is 100 on a stock Postgres - local dev and the CI `postgres:16`
service alike - with 3 reserved for superusers.
Two suites drive contention deliberately and keep a pool as wide as the race they
run (24 games for `concurrent_games`, 30 simultaneous acquires for `lease`), since
a narrow pool would make them queue on the POOL instead of on Postgres, which is
the thing they exist to measure; every other file peaks in the low single digits
and gets 8.
Worst case is therefore `24 + 30 + 8 + 8` plus one short-lived admin connection
per file = 74.
Measured peak across a real run: 57.
The sizes live in `adapters/supabase.ts` next to the pool they size.

`E2E_LANES=serial` runs the lanes one after the other instead of overlapped, which
is what to reach for when a red log is unreadable.

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
| `concurrent_games.test.ts` | many real games on one Postgres | no deadlock, no cross-game corruption (answers "is the parallel deadlock a real bug?" — it isn't). The per-file isolation is per FILE, never per game: all 24 games still share this file's one database, which is the whole point of the test |
| `fuzz.test.ts` | the real validation+handler dispatch + CAS | adversarial/illegal/malformed input never duplicates or loses a card, illegal moves are rejected, the server survives hostile payloads (found + fixed a card-duplication exploit) |
| `rearrange.test.ts` | the real `handleRearrangeHand` + CAS | duplicate/garbage index lists are rejected (found + fixed a card-cloning exploit); a real permutation conserves cards |
| `meta.test.ts` | the real consolidated `meta` handlers + CAS | start/add-bot/exit/continue behave correctly through one endpoint |
| `replay_codec.test.ts` | the real engine + replay encode/decode codec | engine-played games round-trip byte-exact through encode → serialize → decode (plus extras names/timing and the replay-screen view builder). Pure codec test — needs no Postgres. |
| `wasm_engine.test.ts` | the C rules kernel (WASM) behind the _shared API | production deck sizes (5+ → 52), card conservation through full kernel games, the retained TS projections (canCover/game_done/next-player/shouldBotAct) never drift from the kernel, hostile inputs reject with production messages. Pure kernel test — needs no Postgres. |

The latency-sweep conclusions are folded into deterministic checks in
`client.test.ts` ("reordering" / "disconnect").
