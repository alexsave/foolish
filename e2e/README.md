# e2e — codified checks across client + server

These run the **actual deployed code** — the real `supabase/functions/_shared/*`
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

## What's checked

| file | uses real… | asserts |
| --- | --- | --- |
| `server.test.ts` | executeWithGameLock + handlers + commit_game + broadcast | card conservation (sequential & under contention); every broadcast carries a strictly-increasing version |
| `cover.test.ts` | the cover handler | same-rank double-tap rejects gracefully (no `SEVERE` 500); the other same-rank attack still coverable |
| `lease.test.ts` | the bot-lease plpgsql | exactly-one acquire; TTL recovery; stale-token fencing |
| `reconcile.test.ts` | real broadcasts → real client gate + table merge | client converges to the authoritative table under heavy reordering |
| `client.test.ts` | `clientReconcile` (the deployed client logic) | no hand swaps/dupes/table-cards; trust-incoming table; version gate; optimistic-overlay resync |
| `concurrent_games.test.ts` | many real games on one Postgres | no deadlock, no cross-game corruption (answers "is the parallel deadlock a real bug?" — it isn't) |
| `fuzz.test.ts` | the real validation+handler dispatch + CAS | adversarial/illegal/malformed input never duplicates or loses a card, illegal moves are rejected, the server survives hostile payloads (found + fixed a card-duplication exploit — see `findings/FINDINGS_FUZZ.md`) |
| `rearrange.test.ts` | the real `handleRearrangeHand` + CAS | duplicate/garbage index lists are rejected (found + fixed a card-cloning exploit); a real permutation conserves cards |
| `meta.test.ts` | the real consolidated `meta` handlers + CAS | start/add-bot/exit/continue behave correctly through one endpoint |

The narrative write-ups that produced these assertions live in `findings/`
(`FINDINGS_*.md`). They're historical investigation notes; the codified, pass/fail
version is this suite. The latency-sweep conclusions are folded into deterministic
checks in `client.test.ts` ("reordering" / "disconnect").
