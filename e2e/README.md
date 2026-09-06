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
Files run serially (`--test-concurrency=1`) because each resets the shared schema;
see `concurrent_games.test.ts` for why that reset — not gameplay — is what
deadlocks under parallelism.

### A run that failed for no reason is usually the LAST run's connections

`e2ePool` opens up to 40 connections, and a suite killed part-way (Ctrl-C, a
crashed process, a timed-out xcodebuild in another terminal) leaves them open.
Those orphans hold the schema, so the next run's `applySchema` half-applies and
every suite after it dies on `relation "games" does not exist` — a failure that
looks like a code regression, degrades further on each re-run, and clears the
moment the database is recreated.

Two things follow. **Recreate the database rather than debugging the failure**:

```bash
psql -h 127.0.0.1 -U stress -d postgres \
  -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity
      WHERE datname='foolish' AND pid <> pg_backend_pid();"
dropdb -h 127.0.0.1 -U stress foolish && createdb -h 127.0.0.1 -U stress foolish
```

`dropdb` failing with *"is being accessed by other users"* is the same symptom —
retry the terminate/drop pair a few times, and never assume a silenced `dropdb`
succeeded. **And never read a partial failure as a baseline**: confirm a suspect
failure against a freshly created database before believing it, on your branch
AND on `main`. CI is immune to all of this (it gets a fresh container per job),
so a local-only failure that CI does not reproduce is this, most of the time.

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
| `concurrent_games.test.ts` | many real games on one Postgres | no deadlock, no cross-game corruption (answers "is the parallel deadlock a real bug?" — it isn't) |
| `fuzz.test.ts` | the real validation+handler dispatch + CAS | adversarial/illegal/malformed input never duplicates or loses a card, illegal moves are rejected, the server survives hostile payloads (found + fixed a card-duplication exploit) |
| `rearrange.test.ts` | the real `handleRearrangeHand` + CAS | duplicate/garbage index lists are rejected (found + fixed a card-cloning exploit); a real permutation conserves cards |
| `meta.test.ts` | the real consolidated `meta` handlers + CAS | start/add-bot/exit/continue behave correctly through one endpoint |
| `replay_codec.test.ts` | the real engine + replay encode/decode codec | engine-played games round-trip byte-exact through encode → serialize → decode (plus extras names/timing and the replay-screen view builder). Pure codec test — needs no Postgres. |
| `wasm_engine.test.ts` | the C rules kernel (WASM) behind the _shared API | production deck sizes (5+ → 52), card conservation through full kernel games, the retained TS projections (canCover/game_done/next-player/shouldBotAct) never drift from the kernel, hostile inputs reject with production messages. Pure kernel test — needs no Postgres. |

The latency-sweep conclusions are folded into deterministic checks in
`client.test.ts` ("reordering" / "disconnect").
