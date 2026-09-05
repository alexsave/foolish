# The animation core in C (`anim_plan`)

*Landed July 2026. The third-reimplementation problem, closed: animation policy
was hardened once in React, re-derived a second time in Swift, and a Steam client
would have been a third. It now lives once, in C — `c/src/anim_plan.{c,h}` — and
every host reaches it (the web through wasm, iOS through `ios_api.c`, a future
Steam client through either). This is the same argument `msg_wire.h` and
`evwire.h` make for their layers: the same bytes must mean the same game, and the
same events must mean the same choreography, on every device.*

## The boundary (agreed with the owner)

Only **rendering** is irreducibly per-platform: interpolation/springs, view
updates, screen coordinates, gesture previews. Everything above it is pure data
transformation and moved to C:

| Layer | Was (web) | Was (iOS) | Now (C) |
|---|---|---|---|
| **Plan building** — a viewer's event sequence → an ordered plan of timed steps + count-freeze + veil | `AnimationContext` queue + `ANIMATION_TIME` pacing + `inFlightFromDeck` | `MessageTableView.preCounts` (the freeze) + `preHide` veil | `anim_build_plan`, read through `sdk/swift/AnimPlanWire.swift` |
| **Optimistic policy** — predicted-move synthesis, confirm/revert matching, dedup, version gate | `src/state/*` + `AnimationContext` | (iOS has no optimism yet) | `anim_*` policy fns |
| **Timing policy** — `ANIMATION_TIME` and per-event durations | `constants.ts` | Swift `Task.sleep` | `ANIM_TIME_MS` / `anim_step_duration_ms` |

Evwire **decoding** (packed bytes → events + per-step snapshots) did **not** move:
it already lives in the platform decoders (`sdk/ts/wire/evwire.ts`,
`sdk/swift/EvWire.swift`) that the replay-parity tests pin, and the C core
consumes their *output*. The React `setState`/`setTimeout` queue and the SwiftUI
`matchedGeometry` flights stay per-platform — they are the rendering the boundary
deliberately leaves behind.

## The C API surface (`c/src/anim_plan.h`)

### Timing policy
- `#define ANIM_TIME_MS 500` — `ANIMATION_TIME` (`src/constants/constants.ts`).
- `#define ANIM_GAP_MS 25` — the inter-event gap the web queue waits before
  creating the next event's cards (a matched pair with the overlay clear).
- `int anim_step_duration_ms(int event_type)` — the one place a duration is
  decided (all events pace at `ANIM_TIME_MS` today; the seam for a per-type rule).

### Plan building
- `int anim_build_plan(events, n_events, n_players, final_deck, final_discard, final_hand, out)`
  → `AnimPlan`. Every input event (`AnimPlanEvent`) carries **the board it
  committed**. The count-freeze (`AnimCounts pre`) is derived by anchoring on the
  FIRST event's own board and undoing exactly **one** event; each step's post
  counts are that step's own board. Plus `duration_ms`, `start_ms`,
  `in_flight_from_deck`/`in_flight_to_flipped` (the web `inFlightFromDeck` lag),
  and the **veil** (`veil_ids` — dense card ids of real cards in transit into a
  hand or onto the table, the C twin of `preHide`).
  The walk BACKWARD over every event - what this used to do - is wrong and is
  kept only as the fallback for a stream carrying no boards: undoing a REFILL
  puts its cards back in the deck, but the flipped trump lies under the deck and
  is dealt last without ever being counted, so the deck reads a card high near
  the end of a game. That was round 16's "deck suddenly go to 5 cards, then deal,
  and now I have 6?".

### Optimistic policy
- `uint64_t anim_event_key(type, card, from, to, seat)` — the dedup signature
  (`createCardEventString`); a confirming broadcast collides with the optimistic
  entry iff the five fields match, so a played card is skipped, not re-animated.
- `int anim_should_drop_stale(has_last, last, has_incoming, incoming)` —
  `clientReconcile.shouldDropStaleSequence`, the live-broadcast version gate.
- `int anim_stale_optimistic_on_table(opt_cards, table_cards, named_cards, out_release, cap)`
  — `optimisticAnimation.staleOptimisticKeysOnTable`; releases a lingering
  optimistic card the authoritative table shows but this broadcast does not name.
- `int anim_resolve_unconfirmed_attack_covers(pending, server_table, events, fin, out)`
  → `AnimResolve{revert,merge,clear}` — `optimisticConflicts.resolveUnconfirmedAttackCovers`,
  the revert-vs-keep-vs-clear decision that fixed the "card jumps to the table,
  snaps back, re-appears" flicker.

Style, per `msg_wire.h`/`evwire.h`: fixed-size structs, no allocation, every
input range-checked, errors as negative `ANIM_E*` defines. Card inputs BORROW the
caller's storage (like `EvwEvent`).

## How each host reaches it

**Native / tests.** `c/tests/anim_plan_test.c` — the C twins of the React
animation-quality tests (see the mapping below), wired into `c/Makefile`
(`build/anim_plan_test`, and into `make difftests`).

**Web (wasm).** `c/wasm/wasm_api.c` exports `wasm_anim_should_drop_stale`,
`wasm_anim_stale_optimistic`, `wasm_anim_resolve`, `wasm_anim_build_plan`
(bots-only — the web loads `bots.wasm` at boot). `sdk/ts/wasm/bots.ts` bridges
them (`animShouldDropStale`, `animStaleOptimisticOnTable`, `animResolveUnconfirmed`,
`animBuildPlan`). The `src/state/*` modules now **delegate** to these; the React
layer keeps only rendering + React state mechanics.

**iOS.** `c/ios/ios_api.c` exports `fio_anim_plan_packed(in, len, out, cap)` -
the stream and its per-step boards cross as PACKED BYTES, in, and the plan comes
back packed - plus `fio_anim_should_drop_stale(...)`. `sdk/swift/AnimPlanWire.swift`
is the decoder. There is no JSON plan: `fio_anim_plan_json` is deleted, since two
plans in two formats are two rules waiting to disagree. Exercised natively by
`make ios-smoke` (`plan_wire_check`).

## Which TS modules delegate vs remain

| Module | Status |
|---|---|
| `src/state/clientReconcile.ts` `shouldDropStaleSequence` | **delegates** to `animShouldDropStale` |
| `src/state/clientReconcile.ts` `mergeTableBattles` | **remains** — a JS-null coalesce over `Battle[]`, no decision for C to make (documented in place) |
| `src/state/clientReconcile.ts` `resetToLobby`/`reorderHand`/`isHandPermutation`/hand-memory | **remain** — hand/DOM reconciliation, not animation policy |
| `src/state/optimisticAnimation.ts` `staleOptimisticKeysOnTable` | **delegates** to `animStaleOptimisticOnTable` |
| `src/state/optimisticConflicts.ts` `resolveUnconfirmedAttackCovers` | **delegates** to `animResolveUnconfirmed` |
| `src/state/optimisticOverlay.ts` / `authoritativeVersion.ts` | **remain** — a React-registered provider and a per-game version store; state plumbing, not policy |
| `src/utils/animationUtils.ts` `getCardKey`/`createCardEventString` | **remain** — used by the wrappers to marshal; the dedup key's C twin is `anim_event_key` |

## Deferred TS deletion (parity soak)

Per the owner: the original TS bodies are **not deleted yet**. They are preserved
verbatim in `src/state/__ts_reference.ts` (`*TsReference` exports +
`buildAnimPlanTsReference`) and `e2e/anim_core_parity.test.ts` drives **both** the
C core and the original TS over the same generated (real evwire sequences from
seeded engine games) + hostile/edge inputs, asserting identical outputs. Once
parity has held in CI, these die:

- `src/state/__ts_reference.ts` (the whole file)
- `e2e/anim_core_parity.test.ts` (its job done)
- the reference-pointer comments in the three runtime wrappers

The runtime delegation and the C-twin unit tests stay.

## The TODO seam: `AnimationContext` queue + optimistic synthesis

What did **not** move this pass, and why it is safe to leave:

- **The React queue itself** (`processAnimationQueue`, the `setState`/`setTimeout`
  chain, `animatingCards`, `inFlightFromDeck` state) is rendering — it *drives*
  the plan's timings but is not the policy. It should be re-expressed as a driver
  over `animBuildPlan`'s output; today it still computes its own `ANIMATION_TIME`
  loop. The pacing NUMBERS it uses are now the C constants' twins.
- **Optimistic-event synthesis** — `AnimationContext`'s `attack`/`pass`/`pickup`/
  `cover`/`good` build predicted `ClientAnimationEvent`s, register them in
  `optimisticAnimations`/`optimisticCardPositions`, and revert on server reject.
  This is entangled with React refs and the server-promise lifecycle. The
  *decisions* it makes (which cards to revert, the dedup key, the version gate)
  are already in C; the *synthesis + ref bookkeeping* is the remaining seam. A
  clean move is `anim_synthesize_optimistic(move, game) → predicted events`, with
  React holding only the resulting refs. Not attempted here to avoid a
  half-migration of a 1800-line React file in one pass.

## The Mac session: collapsing `MessageTableView` onto the core

Swift was **not** edited (no Swift compiler in this environment). On the next Mac
session:

1. ~~Add a `foolish_anim_plan(viewer)` call decoded with `Codable`.~~ DONE, and
   not with `Codable`: `sdk/swift/AnimPlanWire.swift` reads the packed
   `fio_anim_plan_packed` blob. The stream crosses as an INPUT rather than being
   looked up from the resident game, because a board animates the stream it was
   handed (often half a bubble) from a SwiftUI body that cannot await the actor.
2. ~~Replace `MessageTableView.preCounts` with `plan.pre`.~~ DONE - the
   count-freeze is derived in C and `preCounts` is deleted.
3. Replace the per-step `deckCountOverride`/`discardCountOverride`/`seatCountOverride`
   advance in `runEventStream` with `plan.steps[i].{deck,discard,hand}` — a count
   never jumps ahead of its flight because the plan already staggers them.
4. Replace the `preHide` set (`myNewIds` / `openReplayTouchedCardIds`) with
   `plan.veil` (dense card ids → identities), and reveal a card when its step
   plays (`plan.steps[i]`'s `startMs`/duration).
5. Keep the flight builders (`openReplayFlights`, `myDrawFlights`, spring
   animation) — that is the rendering the boundary keeps in Swift.

### Web/iOS divergences to reconcile (iMessage is the spec)

**CORRECTED 2026-09-05.** This section used to read "the web is the spec" and
told iOS to bend to the C plan. That is no longer the owner's position: "the
imessage behavior and layout and animation is slightly different from the webs,
and I prefer the imessage version."
So the reconciliation runs the other way - capture the iMessage behaviour, and
the web re-derives.
See `c/src/anim_plan.h`'s corrected opening and `ConflictModel.swift`'s header.

- **Bout-end discard flight ordering. LANDED.** iOS holds the ending cover's
  landing before the discard sweep (`pendingCoverLandingFlights`, note 17, and
  round 16's hold, which rests 1.5s on exactly that beat).
  The C plan emits steps in the kernel's evwire order, which is what the web
  plays.
  This file used to say that if iOS wanted the cover-first beat it should
  "reorder *rendering*, not the plan".
  That instruction is struck: the hold is a rule about how a bout ends, not a
  rendering workaround, and it is in the kernel now - `anim_build_beats` groups a
  stream into BEATS and flags the one that holds (`ANIM_BEAT_HOLDS`), alongside
  the out-collapse, the placed set, the badge direction and the role beat.
  How long the rest lasts is still the platform's (`boutEndHold`); which beat
  rests is not.
- **Open-replay veil vs live veil.** iOS derives the veil two ways
  (`openReplayTouchedCardIds` on open vs `myNewIds` live); the C plan's `veil` is
  one definition (real cards in transit into hand/table). Confirm it subsumes both
  on device; if the open-replay path wants a wider veil (approximate table-center
  sources), that is a rendering-time superset, not a plan change.
- **`flipped` and the deck badge.** A card to the `flipped` (trump) slot does not
  decrement the deck badge (it stays "in the deck system"). The trump lying UNDER
  the deck is also why the freeze cannot undo a refill - see the plan-building
  note above. Held against the kernel's own boards in
  `e2e/anim_core_parity.test.ts` and `ios/FoolishTests/MessageCountWindingTests`,
  both of which insist the freeze IS the board before the sequence.

## Test mapping

| Original React test | C twin (`anim_plan_test.c`) | Still exercised end-to-end |
|---|---|---|
| `e2e/optimistic_animation.test.ts` | `test_optimistic_animation` | yes — `optimistic_animation.test.ts` now runs through the C bridge |
| `e2e/optimistic_revert.test.ts` | `test_optimistic_revert` | yes — `optimistic_revert.test.ts` (real games) through the C bridge |
| `e2e/reconcile.test.ts` | `test_reconcile` | yes — `reconcile.test.ts` (real broadcasts, reordered) through the C bridge |
| (new — the choreography no TS test covered) | `test_plan_building` + `test_plan_anchors_on_the_first_events_own_board` | `e2e/anim_core_parity.test.ts` (C == TS ref, + the freeze IS the board before), `tests.c` `test_the_freeze_is_the_board_before_every_move` |

## The Steam story

A new platform implements only the **rendering**: decode the plan
(`AnimPlan`/`fio_anim_plan_packed`), tween sprites from each step's `startMs` for
its `durationMs`, obey the `pre` count-freeze until each step lands, and hide the
`veil` cards until their step reveals them. It answers **no** animation-policy
question — no pacing, no count arithmetic, no revert/keep/clear decision, no
version gate — because all of that is `anim_plan.h`, shared and tested.
