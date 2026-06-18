# Round-2 probes (on top of the version-gate / authoritative-table / resync fix)

Ten questions aimed at regressions the fix could introduce and fresh edge cases.
**8 clean, 1 real new (low-severity, bounded) bug, 1 confirmed pre-existing.**
Q1–Q5 are tested in `probe2.ts`; Q6–Q10 are code analysis with cites.

| # | question | verdict |
| --- | --- | --- |
| Q1 | broadcast `version` ever flat/backward (gate would freeze the client)? | ✅ strictly increases |
| Q2 | two broadcasts ever share a version (gate drops the 2nd)? | ✅ all unique |
| Q3 | does version keep climbing across `continue`/reset (new deal not gated)? | ✅ yes |
| Q4 | "silent good" — non-transitioning good emits no broadcast? | ✅ by design (confirmed intended) |
| Q5 | intra-sequence table forward-only (authoritative replace safe)? | ✅ yes (7,873 seqs) |
| Q6 | gated-out optimistic confirmation leaves tracking uncleared? | 🔧 fixed |
| Q7 | reconnect resync erases an in-flight optimistic attack? | 🔧 fixed (optimistic overlay) |
| Q8 | flapping WS → unbounded resync REST storm? | ⚠️ watch-item (add debounce) |
| Q9 | spectator joining mid-game gets the gate stale-seeded? | ✅ re-seeded by loadGame |
| Q10 | does the gate break the replay player? | ✅ replay carries no version |

## Tested (probe2.ts)

- **Q1/Q2/Q3** — 25 runs of `game → continue → game` on one row, recording every
  broadcast-producing commit's version: strictly increasing, unique, and the
  version keeps climbing across the `continue` reset. So the gate never drops a
  legitimate sequence and a new round's deal is never gated out.
- **Q4 (by design — confirmed intended)** — an attacker pressing *good* when the
  round can't yet transition changes `good_players` server-side but `handleGood`
  returns **0 events** → no broadcast. This is intentional: "goods" are silent and
  don't need to be propagated live. Not a bug.
- **Q5** — across 7,873 real sequences, no card disappears and reappears *within* a
  single sequence. So applying each event's table as an authoritative replace
  animates correctly — validating the `mergeTableBattles` change.

## Q6 — FIXED (was real, new, low-severity): a gated-out confirmation could leave optimistic tracking uncleared

The optimistic entry for a local move is cleared in `handleAnimationMessage`
(AnimationContext.tsx:777-799) when a server **event** matching it is processed. If
the broadcast that confirms my move (version V, whose events include my
`attack_pass`) is itself **gated out** — because the next move's broadcast (V+1)
reordered ahead of it and set `lastAppliedVersion = V+1` — then `handleAnimationMessage`
returns at the gate before that clearing runs, and V+1's events don't reference my
card, so my optimistic entry is never cleared by its own confirmation.

Consequence: for up to the 30 s TTL sweep (AnimationContext.tsx:271-292), the
resolver keeps my card in `optimisticAnimations`; once the card's bout ends and it
leaves the authoritative table, the resolver can **re-inject it as a transient
phantom** until the TTL clears it. Bounded (≤30 s, self-healing) and requires
consecutive-version reorder of my own move, but it's a genuine new interaction
between the gate and the optimistic layer.

**Fix (shipped):** when the gate accepts a sequence, it now also releases
optimistic entries the authoritative state confirms — after `lastAppliedVersionRef`
advances, any of my `optimisticAnimations` whose card is present in the pristine
`message.game.table_battles` is deleted (along with its position tracking). That
clears a confirmation even when its literal broadcast was dropped, so nothing
lingers to be re-injected. Validated in `optimistic_revert.ts` (scenario E).

## Q7 — fixed: reconnect resync no longer vanishes in-flight optimistic cards

The reconnect `loadGame` replaces the table authoritatively (no append), so an
**unconfirmed** optimistic attack/cover made just before the resync would be
dropped and then reappear when its confirming broadcast landed — a "vanish then
reappear" flicker. Fixed with a small `optimisticOverlay` bridge: AnimationContext
exposes the local player's live optimistic cards, and `loadGameInternal` re-applies
them onto the authoritative state before committing it (`applyOptimisticOverlay`).
Idempotent — a normal load with nothing pending is untouched, and the cross-bout
fix is unaffected (only the local player's *tracked* cards are re-applied, never
stale leftovers). Modelled in `optimistic_revert.ts` (scenario D).

## Q8 — watch-item: reconnect resync has no debounce

`RealtimeAnimationFeed` now calls `loadGame` on every re-subscribe. `loadGame`
dedups *concurrent* calls, but a flapping connection (rapid SUBSCRIBED↔ERROR)
would fire one REST refetch per successful resubscribe. Bounded by reconnect rate
and cheap, but on free tier a short debounce ("skip if we resynced < ~2 s ago")
would avoid a refetch burst. Recommended, not urgent.

## Q9 — clean: spectator → player keeps the gate honest

Joining doesn't change `url_game_id`, so the gate isn't reset, but the join flow's
`loadGame` carries the authoritative `version` and the seed effect raises the gate
to it; subsequent live broadcasts are higher and apply normally. A stale replayed
broadcast (lower version) would correctly be dropped.

## Q10 — clean: replay is unaffected

The replay player builds `animation_sequence` messages with no `version` field
(`src/replay/animate.ts` — the `version` in `src/replay/decode.ts` etc. is the
replay *format* version, unrelated). `incomingVersion` is therefore `null` and the
gate is skipped, so seeking/replay behaves exactly as before.
