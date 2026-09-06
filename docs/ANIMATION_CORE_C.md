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
  Since the transport landed this is **marshalling**, not a second rule: it
  builds the same `AnimConflictFacts` the chain builds and buckets the same
  `anim_conflict_verdict` answers.
- `int anim_set_transport(t)` / `int anim_transport(void)` - which client this
  is, said once at initialization.

### The conflict model

When something has to leave a board that no move took off it - a staged move an
arrival overrides, a sequence a newer arrival supersedes - the board REVERSES it
before it plays anything else: the cards travel back the way they came, tinted
red, and only then does the newest chain animate forward.

- `int anim_conflict_facts(moved_ids, open_table, my_hand_ids, out)` - what the
  arriving chain vouches for, reduced to three dense-id bitsets: the cards its
  stream moves, the table of the board it opens on (BOTH sides of a battle
  stand), and my hand there.
- `int anim_conflict_verdict(card_id, dest, facts, hope)` - REVERT / KEEP /
  CLEAR for one card, on either transport.
  CLEAR is decided BEFORE the standing sets, because a card the incoming stream
  moves may also stand on its opening table (a pickup's do by definition) and a
  red flight first is the flicker.
  A masked back and anything that went to a POOL are kept.
  `hope` is the server transport's extra inputs and is required there, NULL on a
  chain - see "The transport" below.
- `int anim_conflict_dest(event_type, seat, my_seat)` - which kind of place an
  event's cards went, so the standing check reads the right side of the board.
- `int anim_conflict_reversal(motions, group_sizes, facts, out)` - the whole
  superseded sequence at once: every motion's verdict, plus which of them fly
  back in which order (reverse group order, a group the verdicts emptied
  dropped).

`anim_resolve_unconfirmed_attack_covers` is the same rule reached through the
server's own vocabulary - see "The transport" below.

### The board's own sets and small rules

The last stage of the lift, which took everything that was still a pure static on
`MessageTableView.swift`.
Card SETS cross as a `u64` bitset over dense ids and ordered hands as arrays of
those same ids, so none of this needs a packed record: a versioned envelope
around two `u64`s would be ceremony, and the brief says to cross as ints instead.

The veil, which is four sets and everything else about it is state (upstream) or
a view (downstream):

- `anim_veil_veiled(hidden, pending_open, has_hand_before, hand_before, has_my_hand, my_hand)`
  - three sources unioned, deliberately not "whichever is up".
  Both halves of the live-play source are required: no pre-move hand is nothing
  to diff against, and no hand at all is a spectator, who has no fan to veil.
- `anim_veil_flying(hidden, pre_hidden)` - `hidden \ pre_hidden`, "in the air
  right now", the one derivation four places rest on.
- `anim_veil_hand_slot_deferred(veiled, flying, holdback)` - which veiled hand
  cards reserve no fan width yet. A held-back card is the one veiled card the fan
  DOES draw, so it keeps its slot.
- `anim_veil_fan(veiled, holdback)` - which veiled cards the fan draws nothing
  for. INVARIANT: the deferral is a subset of this, so the fan never withholds a
  slot from a card it is drawing (asserted over all 512 combinations in
  `tests.c`).
- `anim_veil_grid(sweeping, ...)` - the battle grid's two sets. The branches
  answer off DIFFERENT STATE rather than a different filter over one, because a
  picked-up card is legitimately hidden on the hand's grid and drawn on the
  sweeping one in the same paint.
- `anim_veil_teardown` / `anim_veil_handover` - what a finishing sequence owes the
  board, and what a new play owes the one it replaces.
- `anim_veil_unstarted_replay`, `anim_holdback_is_mine`,
  `anim_selection_after_tap` - the window the veil is up for, the epoch that says
  whose holdback it is, and the rule that the selection may only ever name cards
  in my hand.

The hand's layout, which is a LIST because the fan places cards by index:

- `anim_fan_cards(hand, held, out, cap)` - my hand plus whatever a replay is
  still holding back, each card once.
- `anim_laid_count(hand, held, deferred)` - how many of those the fan lays out.
- `anim_hand_laid_out(cards, deferred, order, out, cap)` - the array the fan
  actually draws, in the order it draws it. `order` is the player's own grow-only
  arrangement and is an INPUT: it legitimately names cards that are not in the
  hand. This is also `FHandFan.displayOrder` with nothing deferred, and the web
  states the same contract as `displayedHand` (`src/state/clientReconcile.ts`).
- `anim_is_placement(type)` / `anim_is_my_placement(type, seat, my_seat)` - the
  holdback's seed. A seatless viewer places nothing, because the kernel spends
  seat -1 on "no particular player" as well as on "no seat".

The table under a sweep, and the end screen:

- `anim_table_card_ids(table, n)` - the cards a 2-bytes-per-battle table holds.
- `anim_table_covers(outer, inner)` - the ONE subset test two table choices rest
  on. A cell holding a card the caller cannot NAME crosses as
  `ANIM_TABLE_UNKNOWN`, not as an empty cell, and is never accounted for: a sweep
  may add a card, never drop one.
- `anim_covered_sweep_accepts(paired, pre, cur)` - whether a bout-ending cover
  sweeps off the kernel's covered table. The `paired` test is not redundant with
  the subset one - a pickup read flat names the same cards in a shape nobody
  vouched for.
- `anim_shown_table(n_live, n_sweep, n_pending, out_sweeping)` - which of the
  three tables the grid paints. It turns on emptiness alone, so the tables never
  cross.
- `anim_finish_rows(elimination, game_over, n_players, my_seat, out, cap)` - rank
  1 is the first player out, the fool takes the last place, and that place is the
  SEAT count rather than the row count. Names are not here; identity lives in the
  roster.
- `anim_shown_ledger_allows(claim, sequencing)` - who may say what the badges are
  showing. Only a bystander ever stands down.

Reached from Swift through `sdk/swift/BoardWire.swift` (the card-set crossing,
the veil, the ledger codes, the finish order), `sdk/swift/HandWire.swift` and the
table half of `sdk/swift/PreTableWire.swift`.

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
The conflict model crosses the same way: `fio_conflict_packed(in, len, out, cap)`
answers a whole superseded sequence in one call (the verdicts AND the reversal's
order), with `fio_conflict_dest` beside it, decoded by
`sdk/swift/ConflictWire.swift` and smoke-tested by `conflict_wire_check`.

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

## Deferred TS deletion (parity soak) - DONE

The soak held, so the three things it was holding open are gone:
`src/state/__ts_reference.ts`, `e2e/anim_core_parity.test.ts`, and the
reference-pointer comments in the three runtime wrappers.

The runtime delegation and the C-twin unit tests stay: the policies are asserted
natively by `c/tests/anim_plan_test.c` (`test_optimistic_animation`,
`test_optimistic_revert`, `test_reconcile`, plus plan building) and end-to-end
through the delegations by `e2e/optimistic_animation.test.ts`,
`e2e/optimistic_revert.test.ts` and `e2e/reconcile.test.ts`.
There is no longer a second TS implementation for a C answer to be compared
against, which was the point.

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
3. ~~Everything still pure on `MessageTableView`.~~ DONE, in the final lift
   stage: the veil's four sets, the hand's layout, the table under a sweep, the
   end screen's order and the ledger's ownership are all `anim_plan.c`. What is
   left on the board is rects, angles, springs, timing against the host and the
   DEBUG traces - see "What stayed in Swift, and why" below.
4. Replace the per-step `deckCountOverride`/`discardCountOverride`/`seatCountOverride`
   advance in `runEventStream` with `plan.steps[i].{deck,discard,hand}` — a count
   never jumps ahead of its flight because the plan already staggers them.
5. Replace the `preHide` set (`myNewIds` / `openReplayTouchedCardIds`) with
   `plan.veil` (dense card ids → identities), and reveal a card when its step
   plays (`plan.steps[i]`'s `startMs`/duration).
6. Keep the flight builders (`openReplayFlights`, `myDrawFlights`, spring
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
  `c/tests/tests.c` `test_the_freeze_is_the_board_before_every_move` and
  `ios/FoolishTests/MessageCountWindingTests`, both of which insist the freeze IS
  the board before the sequence.

## The transport: one rule, two clients, one question

`anim_conflict_*` and `anim_resolve_unconfirmed_attack_covers` used to be two
rules answering "revert / keep / clear" for a card that is on the table without
the newest truth showing it there.
They are one rule now, and the thing that separated them is a single question
asked at the very end of it.

What is IDENTICAL, and is written once:

- the CLEAR test - does the incoming stream itself move this card, checked first
  in both because a card the arrival animates must not fly home red before it;
- the KEEP test - does the card stand where the motion put it, on the board the
  newest truth vouches for.
  The server calls that board the authoritative table and iMessage calls it the
  arriving chain's opening board: different name, same set membership;
- the pool rule and the masked-back rule;
- the reversal's order.

What DIFFERS, and is the whole of the flag:

> Once a card has failed both tests, is "not accounted for" CONCLUSIVE?

- `ANIM_TRANSPORT_CHAIN` (iMessage): yes.
  Every message carries the whole game in a total order, so a card the newest
  chain does not account for is doomed. REVERT.
- `ANIM_TRANSPORT_SERVER` (web, the iOS app's online play, a watch, Steam): no.
  The card's own confirmation is a separate future broadcast.
  So the verdict asks its extra question first - the defender-capacity
  inference, as `AnimServerHope` - and KEEPs the card when the answer is "this
  could still be accepted".

That is one `if` near the end of one function, and a reviewer should be able to
point at it.
Reading a broadcast the chain's way is `e2e/optimistic_revert.test.ts` SCENARIO
A: Hero's sent, legal attack flies home in red and then flies back out when its
own broadcast lands - the player-reported "it jumps to the table, back to my
hand, then to the table again".

**No default.** A verdict that reaches that question with nothing set returns
`ANIM_ETRANSPORT` (`FIO_ETRANSPORT`), and the Swift reader turns that into no
plan at all rather than into somebody else's answer.
The A1 roster cutover is the precedent: unlinked bots silently played `random`,
and nothing noticed until the fuzz was made honest.

**Who declares it.** `MessagesViewController.viewDidLoad` (chain),
`FoolishHarnessApp.init` (chain - the rig poses the extension),
`FoolishApp.init` (server), and `bots()` in `sdk/ts/wasm/bots.ts` (server - every
wasm host is a broadcast host).
`anim_transport()` / `fio_transport()` / `animTransport()` read it back, because
a wrong-mode bug looks exactly like an animation bug.

**One deliberate change of answer.** The old server rule short-circuited the
WHOLE pending set on the first card the broadcast already showed; the shared
rule judges each card, so a broadcast showing some of my run leaves the ones it
shows standing and still asks after the rest.
`AnimationContext` only calls when NONE is accepted, so no caller can produce
that shape; it is pinned in `anim_plan_test.c` rather than left implicit.

## Test mapping

| Original React test | C twin (`anim_plan_test.c`) | Still exercised end-to-end |
|---|---|---|
| `e2e/optimistic_animation.test.ts` | `test_optimistic_animation` | yes — `optimistic_animation.test.ts` now runs through the C bridge |
| `e2e/optimistic_revert.test.ts` | `test_optimistic_revert` | yes — `optimistic_revert.test.ts` (real games) through the C bridge |
| `e2e/reconcile.test.ts` | `test_reconcile` | yes — `reconcile.test.ts` (real broadcasts, reordered) through the C bridge |
| (new - the choreography no TS test covered) | `test_plan_building` + `test_plan_anchors_on_the_first_events_own_board` | `tests.c` `test_the_freeze_is_the_board_before_every_move` |

## What stayed in Swift, and why

The closing inventory of the lift campaign, taken when the last stage landed.

**Rendering, which is where the boundary puts it.** Rects, angles, screen
coordinates and springs: `dragHintPosition`, `collapseFraction`,
`coverLandingRects` / `coverLandingFlights`, `inBoardSpace`, `tableSource`,
`playSourceRects`, `roleFlights`, `undoReleaseTargets`, `approximateTableCenter`,
the `boardSpace` frame plumbing, `FHandFan`'s geometry, `FBattleGrid`'s tilt,
`FlyingCardsLayer`, `CollapseTween`, and every `matchedGeometryEffect` namespace.
`awaitSheetSettled` is timing against the Messages host, not a rule.
The `traceGrid` / `traceCount` / `traceMark` trio is DEBUG diagnostics about what
this board painted, which only this board can answer.

**Liftable and deliberately left.**

- `MessageTableView.autoPick(_ moves:)` - the FoolishHarness rig's move chooser.
  It is `#if DEBUG`, and its actual input is a process environment variable
  (`HARNESS_AUTOMOVE_KIND`); the pure remainder is one `first(where:)` over a
  menu the kernel already narrowed (`fio_play_human_menu`). It is test
  scaffolding, not behaviour a second client re-derives.
- `MessageTurnController.sentBytes(staged:host:sealed:)` - **the PICKER moved,
  2026-09-05, exactly as this entry said it should.** The rule about which
  bubble a send can have carried is `msg_turn_sent_source` (three ints in, one
  answer out) and the verdict that follows from it is
  `msg_turn_send_verdict`; `sentBytes` is now the one line that turns that
  answer into whichever of the two `Data` blobs this device is holding. The
  blobs still never cross, because the kernel has no opinion about a payload it
  did not write. See "Queued: the iMessage chain layer / Item 2" in
  `docs/KERNEL_LIFT_BRIEF.md`.
- `MessageGameStore.handOrder(gameId:)` - the local per-game arrangement the fan
  is laid out against. It is client-local memory in the App Group container, and
  the kernel already consumes it as an input to `anim_hand_laid_out`.

**Neither, and worth a look.** The extension's own on-disk data is still JSON:
`ReplayStore`'s saved-replay index, and `MessageGameStore`'s App Group rows
(seat roster, hand arrangement). Both are client-local STORAGE rather than a
wire, so neither is a task-#17 remainder and neither is a rule a second client
re-derives - but the owner has said they dislike the first, and the two are the
same decision. Recorded here so the campaign closes with it named rather than
quietly out of scope.

## The Steam story

A new platform implements only the **rendering**: decode the plan
(`AnimPlan`/`fio_anim_plan_packed`), tween sprites from each step's `startMs` for
its `durationMs`, obey the `pre` count-freeze until each step lands, and hide the
`veil` cards until their step reveals them. It answers **no** animation-policy
question — no pacing, no count arithmetic, no revert/keep/clear decision, no
version gate — because all of that is `anim_plan.h`, shared and tested.
