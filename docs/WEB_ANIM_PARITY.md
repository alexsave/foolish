# The web's animation, measured against the iMessage one

Written before any code changed, in the shape `docs/ANIM_TIMING_AUDIT.md` established:
what moves is a list and not a guess.

The owner's report: the iMessage board and the C kernel's plan are right, the web is
"kinda glitchy", and three things are named - a bot's opening move sometimes runs into
the next one with no beat between them, covers do not fly to the card they cover, and
the table piles do not transition smoothly.

Every number below was measured in a real browser (Chromium, 1280x900, per
`requestAnimationFrame`) against the replay screen at `/<code>`, which needs no backend
and drives the same pipeline live play does: `pushSequence.ts` -> `useAnimationRun.ts` ->
`AnimationOverlay.tsx`. `anim_plan.h`'s header sets the direction of travel this document
works in: iMessage is the spec, and where the two disagree the web is the client that
re-derives.

Nothing here is fixed yet. This is the inventory.

---

## 1. Covers are aimed by a guess, not by the kernel's answer

`src/state/pushSequence.ts:50-51` already puts the kernel's answer on every pushed event:

```ts
if (e.hasTarget) ev.target_card = e.target;
if (e.battle >= 0)  ev.battle_index = e.battle;
```

`src/components/GameDisplay/AnimationOverlay.tsx` destructures both and then does not use
them - its own comment says so, "kept in the destructure for future multi-card cover
handling; currently unused". `coverTarget()` reads only `target_cards`, which is set by
one caller, the local optimistic path in `AnimationContext.tsx`. So for every cover that
arrives from the server - every bot cover, every opponent cover - the overlay throws the
kernel's answer away and re-derives:

```ts
game.battles.filter((b) => !covered(b))
            .find((b) => canCoverPair(b.attack, c, game.powerSuit))
```

which is the FIRST legally coverable uncovered battle. Whenever more than one battle is
legally coverable - a trump, two battles of equal rank - the guess can name a different
pile than the one the kernel named, and the card flies to the wrong place and snaps on
landing. When nothing matches, the chain falls through to `getFallbackPosition('table')`,
which is the viewport centre.

This is a host restating a decision the kernel already made, which is the single thing
`docs/ARCHITECTURE_AS_A_PATTERN.md` exists to prevent. `findElementByLocation` already
accepts a `battleIndex` and queries `[data-battle-index]`; the cover path never passes one.

## 2. The battle row is never frozen, so the grid re-lays out under the flights

Element centres, stepping the replay one move at a time:

```
t=  591   one battle      3-7@640,424
t= 5693   two battles     3-7@600,424   1-7@680,424
t= 7226   three battles   3-7@560,424   1-7@640,424   2-7@720,424
t=13329   covers land     all three jump y 424->375, two new piles appear at y=474
```

Every landing instantly re-centres and re-rows the whole grid. No transition, no freeze.
A pile that was already down moves 40px sideways the moment the next one lands.

The kernel answers this and the web does not ask. `AnimCounts` (`c/src/anim_plan.h`)
carries `battles` / `n_battles` / `paired` as the pre-stream row, derived by
`anim_pre_stream_table` for additions and `anim_pre_bout_table` for sweeps, and its own
comment describes this exact defect: a card "36pt (half a slot plus its gap) to the side
of where it belonged on the very first painted frame". It is already reachable from
TypeScript - `AnimCountsSnap` in `sdk/ts/wasm/bots.ts` has `nBattles`, `battles[]` and
`paired`, and `AnimPlanSnap.pre` is that struct, so `planFor()` in `src/state/animPlan.ts`
already receives the frozen row and discards it.

`AnimFrame`, the per-frame re-ask, carries no row - only deck, discard, hand and flipped -
so a per-frame answer needs either the plan's `pre` held alongside the frame or a kernel
addition.

What iMessage does, and it is the reference: `ShownLedger` holds the row, seeded from
`AnimPlan.pre.battles` and "advanced one step per landing flight, WITH THE PLAN'S OWN
DURATION ON IT". `MessageTableView+Table.swift` composes the two halves and delegates the
live-versus-sweep choice to the kernel's `anim_shown_table`. `AnimationOverlay.tsx` still
picks `currentAnimation.game_state ?? game`, which `anim_plan.h` calls out by name as the
"live outranks pending" bug.

A frame from the shipped iMessage app (`media/foolish-showcase.mp4` at t=21.0, expanded
board) settles a question this audit had open: a single battle sits at the board's
horizontal centre, x=323 of 646. **iMessage centres its grid exactly as the web does.**
The fix is the freeze and the transition, not a change of layout.

## 3. There is no beat between two moves, and no rest at a bout end

`anim_build_beats` is exported by `sdk/ts/wasm/bots.ts` and is called from nowhere in
`src/`. (`src/components/tutorialBeats.ts` is the tutorial's narration and is a different
thing entirely.) So the web has none of the beats model: consecutive covers by one seat
are not merged, an `out` notice burns a silent 500ms step instead of collapsing into the
beat that moved something, and `ANIM_BEAT_HOLDS` is never read.

Autoplaying the replay and logging every flight, gaps measured from the last landing to
the next start, two regimes appear:

```
~265-282ms   between separate replay steps   (the replay transport's own dial)
~18-36ms     between flights inside one run  (ANIM_GAP_MS = 25, plus a frame)
```

and the bout end is four big events glued together:

```
10442   cover starts          gap 269ms
10958   cover lands
10994   10-card sweep         gap  36ms
11468   sweep lands
11497   refill                gap  29ms
12030   refill                gap  33ms
12549   refill                gap  18ms
```

A bout-ending cover is taken off the table 36ms after it lands. iMessage rests
`boutEndHold` there, which is `flightTime * 3` = 1500ms
(`ios/FoolishKit/Boards/BoardFlight.swift:55`), and that constant's comment records the
owner asking for it twice, the second time by name: "for last defense, still not enough
of a pause in animation when they cover ... Make it like 1.5 second".

`MessageTableView+Sequence.swift` builds `AnimBeats(events)` and iterates beats, not
events, reading `dropsBadge`, `outs`, `placedAny`, `goodMask`, and sleeping `boutEndHold`
when `beat.holds`. The web plays raw events at a flat 500+25 cadence and never rests.

### The mechanism is the beat boundary, not a race

A first reading of `useAnimationRun.enqueue` suggested a race: it appends to the run and
returns early while `originRef` is non-null, and the reset only happens on the next frame
after `done`, so a push landing in that window would be appended to an expired run and
land having never flown. Asked of the kernel directly, that is wrong for the ordinary
case:

```
run of 2 steps: starts [0, 525]   total_ms = 1025
append a step ->  start_ms = 1050, lands 1550
  clock 1025 (the append instant):  step=-1, not started
  clock 1058:                       step=2, 8ms into its 500ms flight
```

The appended step gets its full flight. Being swallowed needs the frame loop to slip more
than a whole step, which is a backgrounded tab, not everyday play.

What is actually wrong needs no race: a push arriving as the previous run ends is appended
to it, so two different players' moves are separated by `ANIM_GAP_MS` - the gap meant for
two steps inside ONE move. There is no beat boundary between moves at all. The repair is
the beats model, not an origin reset.

---

## What the kernel already answers, and the web only has to start asking

- the cover's target and its battle index, already on the event (`pushSequence.ts`);
- the pre-stream battle row (`AnimCounts.battles`, reachable as `AnimPlanSnap.pre`);
- which row the grid paints and whether it is a sweep (`anim_shown_table`);
- beat grouping, the bout-end hold, the out-collapse, the placed set and the
  badge-drops-as-cards-leave rule (`anim_build_beats`, `AnimBeat`).

## What is NOT settled here

- Where the inter-beat rest should live. `anim_build_beats` decides WHETHER a beat holds;
  iOS owns HOW LONG and derives it from the kernel's flight time. Growing
  `anim_build_plan` to lay `start_ms` out beat-aware would be the stronger answer by the
  pattern's own rule, but iOS calls `fio_anim_plan` and `fio_anim_beats` separately and
  paces itself with its own awaits, so changing the plan's layout has blast radius that
  has not been measured. Whoever takes it must check `c/ios/ios_api.c` first.
- A cold-start deal on `/tutorial` was observed behaving differently across three
  identical runs - once the cards flew, once they sat at the deck for 1.95s and vanished
  without moving, once no flight was drawn at all. Not root-caused, not reproduced on the
  replay screen, and recorded here only so it is not lost.

## How to re-measure any of this

```
npm run gen && npm run wasm:bots       # needs libclang-18-dev
npx next dev -p 3000                   # /tutorial and /<replay-code> need no backend
```

The animation gates run as:

```
TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx \
  --experimental-test-module-mocks --test e2e/ui_animation_trace.test.ts
```

The flag is required. Without it the suite dies with "mock.module is not a function" and
reads as a red gate when it is not; it is 23/23 green on a clean tree.
