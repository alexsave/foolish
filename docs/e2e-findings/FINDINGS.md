# Stress test: local-emulated server, animation-sequence integrity

## TL;DR

I stood up a **real Postgres** copy of the Supabase backend running the **real
`commit_game` CAS RPC** and the **real move handlers**, then hammered a single
game with rapid, overlapping calls (human bursts + double-clicks + a concurrent
lease-driven bot loop) through the **real fire-and-forget broadcast path**.

| Property | Result |
| --- | --- |
| Durable card state (deck+hands+table+discard = 36, no dupes) | ✅ **never broke** — 0 violations across ~25k committed moves |
| CAS fence under contention | ✅ held — ~6k conflicts all retried/redone correctly |
| **Live animation-event ordering** | ❌ **breaks** — sequences arrive out of order and the client rubber-bands to a stale state |

The core game is correct. **The display/animation layer is not**, under
realistic Realtime latency — exactly the "glitchy animation events from calling
too fast" hypothesis.

## How it was tested

`tests/stress/` (run with `npm run test:stress`):

- `schema.sql` — gameplay tables + the **verbatim** `commit_game`,
  `try_acquire_bot_lease`, `release_bot_lease`, `renew_bot_lease` functions from
  `supabase/migrations/`. RLS/realtime/auth omitted (the harness connects as the
  service role, like the edge functions). The concurrency primitive under test is
  real plpgsql in real Postgres.
- `db.ts` — faithful re-implementation of `loadCompleteGame` / `commitGame` over
  `pg`. Load is wrapped in a `REPEATABLE READ` txn to match production's
  single-snapshot PostgREST select (without this the harness itself produces torn
  reads — see "False positive caught" below).
- `orchestrator.ts` — faithful replica of `executeWithGameLock` (the CAS retry
  loop) and `broadcastAnimationEvents`. Broadcast is launched **after** commit and
  **not awaited**, exactly like production, with a modelled per-recipient Realtime
  delivery latency so client arrival order is what decides correctness.
- `moves.ts` / `apply.ts` — legal-move enumeration feeding the **unchanged**
  `handleAttack/Cover/Pass/Pickup/Good`.
- `invariants.ts` — card conservation + uniqueness, and per-client broadcast
  ordering analysis.
- `stress.ts` — driver: seeds a game, deals, then fires overlapping bursts (1–3
  concurrent legal moves, ~30% double-submits) while a concurrent bot loop drives
  the bots, auditing the durable state after every step.

## Finding 1 — animation sequences arrive out of order (real)

The number of **version regressions** (a client receives an animation sequence
committed at a version *older* than one it already rendered) scales directly with
broadcast latency:

```
blatency=0ms     regressions: 0
blatency=15ms    regressions: 67
blatency=40ms    regressions: 582
blatency=120ms   regressions: 949
blatency=250ms   regressions: 1620
```

(6 games, 2 humans + 2 bots each.) At 0ms it's clean; at realistic Realtime
latency it's pervasive. Even the opening DEAL (`start`, v1) can land after later
moves.

### Root cause

1. `executeWithGameLock` broadcasts via
   `broadcastAnimationEvents(...).catch(...)` — un-awaited, fire-and-forget, after
   the commit (`supabase/functions/_shared/utils.ts`).
2. Each broadcast opens a **fresh ephemeral channel** `gu-<game>-<player>`,
   `send`s, then `removeChannel`s. Independent sends → Realtime gives no
   cross-broadcast ordering guarantee.
3. Overlapping broadcasts are normal: a human move also kicks `lockedBotLoop`,
   whose every cycle broadcasts; rapid/duplicate human taps add more in flight.
4. The payload carries `sequence_id` (random) + `timestamp` (emit time) but **no
   monotonic game version**.
5. Client `RealtimeAnimationFeed` republishes *every* payload;
   `AnimationContext` dedupes only by `sequence_id` (unique per broadcast, so it
   never matches a reorder) and **appends sequences to its play queue in arrival
   order** — no version gate.

Net effect: under variable latency a later-committed sequence can be played
before an earlier one, or an older sequence arrives after a newer one already
played → the board animates **backward** (cards teleport/rubber-band). Worse on
slow / free-tier networks, and most visible exactly when a user taps quickly or
when bots move while the user is also moving.

Durable state is unaffected because the CAS makes every *committed* state correct;
this is purely a client-visible ordering defect.

### Recommended fix (small, server + client)

- **Server**: include the committed version in the broadcast payload —
  `payload.version = game.version` in `broadcastAnimationEvents` (the value is
  already on `game.version` after `commitGame`).
- **Client**: track `lastAppliedVersion`; at ingest
  (`RealtimeAnimationFeed`/`AnimationContext`) drop any sequence with
  `version <= lastAppliedVersion`. This is lossless: each sequence carries the
  full resulting `game_state`, so a superseded one contains nothing the newer one
  doesn't. The existing optimistic-animation logic is untouched; only a monotonic
  gate is added at the front.

The harness already tags every delivery with its committed version, so a
post-fix run should report **0 regressions** while still delivering every
distinct version once.

## Finding 2 — `rearrange-hand` broadcasts pre-commit (minor, same root cause)

`supabase/functions/rearrange-hand/index.ts` calls `broadcastToGameUser` **inside**
the operation closure, i.e. before (and on every retry of) the CAS commit. A
conflicted attempt can therefore emit a hand-order broadcast that never committed,
or emit twice. Low impact (private hand order, self-heals on the next state) but
the same "broadcast not tied to the committed version" smell; the Finding-1 fix
(version-gated ingest) also neutralizes it.

## False positive caught (worth noting for honesty)

An early run reported a `total=35` (one lost card) at `blatency=0`. It was **not**
a server bug: the harness's `loadCompleteGame` issued 4 separate `SELECT`s, so a
`commit_game` transaction could land between them (a torn read). Production reads
the whole game in **one** PostgREST select = one MVCC snapshot. After wrapping the
harness load in a `REPEATABLE READ` transaction, card violations went to **0** and
stayed there across heavy re-runs. The CAS design itself is sound.
