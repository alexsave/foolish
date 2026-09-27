# Client reconciliation under packet loss / reordering

Follow-up to `FINDINGS.md`. Question 1: does the client reconcile after a brief
WS disconnect, or do cards from different bouts share the table? Question 2: can
out-of-order delivery + optimistic animations/reverts glitch?

Both reproduced **empirically** with a client-model simulation
(`client_sim.ts`) that replays **real** broadcast streams (real handlers → real
per-event `game_state` snapshots from a multi-bout game) through the client's
**real merge logic** — `mergeGameData`/`mergeTableBattles`/`mergeHandOrder`
ported verbatim from `src/contexts/ServerContext.tsx` into `client_merge.ts`.

```
npx tsx tests/stress/client_sim.ts --trials=80 --blatency=120
```

## Results (80 real games, all reached a 2nd bout)

| regime | phantom card on table | cross-bout table | covered card un-covers | table permanently wrong |
| --- | --- | --- | --- | --- |
| **in-order** (baseline) | 0/80 | 0/80 | 0/80 | 0/80 |
| **disconnect** (1 lost packet) | **79/80** | **79/80** | 3/80 | 1/80 |
| **reordered** (latency) | **80/80** | **80/80** | **80/80** | **68/80** |

In-order is clean, which validates the model: the client reconciles perfectly
when delivery is perfect. The breaks are entirely delivery-induced.

Reordered is dose-dependent on latency (like Finding 1):

```
blatency  20ms  60ms  120ms  250ms     (permanently-wrong table, /40)
            27    32     36     36
```

## Q1 — WS disconnect: YES, cards from different bouts share the table

Two facts combine:

1. **The client never refetches authoritative state on WS reconnect.**
   `src/state/RealtimeAnimationFeed.tsx:85-116` — on `CHANNEL_ERROR`/`TIMED_OUT`/
   `CLOSED` it only re-subscribes; there is no catch-up fetch, and
   `ServerContext` has no polling / visibility / online refetch. Broadcasts
   missed during the gap are gone forever.
2. **State is merged, not replaced**, and the table merge re-appends stale
   battles. `ServerContext.tsx:362-386` `mergeTableBattles`: the only table-clear
   path is `if (incomingBattles.length === 0) return []`. A normal round
   transition clears because its snapshot has `table_battles: []`. **If that one
   round-transition broadcast is the packet lost during the gap**, the client
   never sees an empty table; the next bout's attack arrives non-empty, and the
   merge keeps incoming **plus appends every previous-bout battle whose attack key
   isn't in incoming** (`:377-383`) → the old bout's cards sit on the table next
   to the new bout's.

The sim shows this in **79/80** games. It is **transient** (final mismatch 1/80):
it self-heals at the *next* round end, when an `incomingBattles.length === 0`
snapshot finally flushes the orphans. So the player sees a wrong, mixed table for
~one full bout after a blip — including phantom "uncovered attacks" that wrongly
imply the defender still owes a cover.

### Fix
On WS (re)subscribe success (`status === 'SUBSCRIBED'` in RealtimeAnimationFeed),
trigger an authoritative `loadGame(game_id)` refetch and **replace** (not merge)
the table from it. A reconnect must resync, not just reattach. (The version stamp
from Finding 1 also lets the client tell whether it missed anything.)

## Q2 — out-of-order delivery: covered cards un-cover, table ends up wrong

Under realistic latency the un-awaited per-broadcast sends (Finding 1) arrive out
of version order, and the client applies them in **arrival** order with no version
gate (`AnimationContext.tsx:744-760` only dedupes by the *random* `sequence_id` +
exact event content — neither catches a reorder). Then `mergeTableBattles` trusts
**incoming's** defense state by attack key (`:374`), so a stale snapshot where
`7♦` is still uncovered **overwrites** a covered `{7♦ → 8♦}` battle: the covering
card visibly disappears (un-cover **80/80**), and the table is left permanently
wrong in **68/80** games (the client's last-applied snapshot is older than the
newest it received).

### Optimistic animations + reverts make it worse (code analysis)

The optimistic layer is keyed on `optimisticAnimations` (a Map by card string)
and is also version-blind, so reordering amplifies rather than absorbs the glitch:

- **Spurious revert → flicker.** A stale snapshot that predates the server's
  acceptance of the user's card doesn't contain it, so `resolveOptimisticConflicts`
  (`AnimationContext.tsx:285-336`, `:570-730`) treats it as rejected and queues a
  `revert` animation flying the card table→hand — then the newer snapshot re-adds
  it hand→table. The just-played card bounces back and forth.
- **Re-animating a confirmed card.** Once a card's optimistic entry is cleared on
  confirmation (`:787-795`), a later-arriving stale sequence still containing that
  card no longer matches as optimistic (`:777-799`), so it's re-queued as a fresh
  animation — the card animates into place a second time.
- **Optimistic pass position re-applied.** A stale `message.game` whose
  `defender`/`first_attacker` don't match the optimistic pass state gets
  **overwritten** with the optimistic values (`:841-858`), so the defender
  indicator jumps to a position the server has already moved past.

All three share Finding 1's root cause (no monotonic version on broadcasts) and
are fixed the same way: stamp each broadcast with the committed `games.version`
and drop/ignore any sequence at or below the last applied version, before both the
merge and the optimistic reconciliation.

## Honesty notes

- The Q1/Q2 table numbers are **empirical** (real merge over real snapshots);
  the three optimistic sub-points are **code analysis** with exact line cites, not
  separately simulated — modelling the full `resolveOptimisticConflicts` state
  machine faithfully was out of scope, and the empirical "un-cover 80/80" already
  exercises the same stale-merge path the optimistic layer sits on.
- Early metric drafts over-counted (same card legitimately re-attacked in a later
  bout looked "cross-bout"); the committed metrics reset per-bout and compare the
  client table against the server's authoritative table at the newest applied
  version, which is why the in-order baseline is a clean zero.
