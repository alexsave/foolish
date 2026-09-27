# The fix: monotonic broadcast versioning + authoritative table + reconnect resync

Addresses the reordering findings (FINDINGS.md / FINDINGS_LATENCY.md) and the
client-reconciliation findings (FINDINGS_CLIENT.md). Root cause of all of them:
live broadcasts carried no monotonic ordering token and the client applied
whatever arrived, merging it into local state.

## Changes

**Server** (`server/impls/supabase/functions/_shared/utils.ts`)
- Stamp every broadcast payload (per-player and spectator) with the committed
  `games.version`.

**Client**
- `AnimationContext`: a **version gate** at message ingest — drop any sequence
  whose version is `<=` the newest already applied (stale / out-of-order /
  duplicate). Seeded and raised from authoritative REST loads; reset per game.
- `ServerContext.mergeTableBattles`: **trust the incoming table** instead of
  re-appending leftover battles. The append used to preserve optimistic attacks
  during out-of-order responses; that's now handled by the version gate (ordering)
  and by the optimistic-conflict resolver, which injects the local player's
  unconfirmed cards into the incoming state upstream. The append was the thing that
  re-introduced a previous bout's cards when an intermediate table-clear was
  skipped.
- `RealtimeAnimationFeed`: on a **re-subscribe** (reconnect), refetch authoritative
  state via the newly-exposed `loadGame`, since broadcasts missed during the gap
  are never redelivered.
- Plumbing: `version` added to the client `PublicGame` type and the load query,
  preserved through `mergeGameData`, and `loadGame` exposed on the context.

## Why all three are needed

The model (`client_sim.ts --fixed`) showed the version gate **alone** is not
enough: it stops *applying* stale sequences, but the ones it does apply still went
through the appending merge, so a skipped clear still re-introduced orphans
(reordered final-mismatch only dropped 47→32 / 60). Trusting the incoming table
(authoritative replace) is what closes it; the reconnect resync covers the
pure-packet-loss case where no later broadcast arrives at all.

## Validation (model)

`npx tsx tests/stress/client_sim.ts --sweep --trials=30 --fixed` — permanently
wrong client table, % of games, across emission-gap × delivery-latency:

```
gap\lat   0  1  2  5 10 25 50 100 250 1000
ALL CELLS .................... 0%
```

Per-regime (`--fixed`, blatency 120, 60 games): phantom 0, cross-bout 0,
final-mismatch 0 for both reordered and disconnect (vs. 60/60/47 and 60/60/1
unfixed). The only residual is "un-cover" counts under heavy reordering — that's
the gate intentionally **skipping** stale intermediate frames and jumping forward
to the freshest authoritative state (correct behaviour, not a backward
rubber-band).

The server-side CAS card-conservation guarantees are untouched; this is purely the
client-facing display/ordering layer.
