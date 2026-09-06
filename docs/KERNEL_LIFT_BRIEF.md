# The Swift-to-C lift: the brief every stage works from

This is the working brief for the campaign that moves logic out of
`ios/FoolishKit` and into the C kernel under `c/`.
It exists so each stage can be handed to a fresh pair of hands without
re-deriving the ground rules.

## Why

The iMessage extension grew a rich animation and interaction model in Swift while
the C animation core (`c/src/anim_plan.h`) stood still.
More clients are coming - the iOS app proper, a watch, whatever follows - and
every rule left in Swift is a rule each of them re-derives and gets subtly wrong.

The owner's standing instruction: **when in doubt, move code to C.**
Where a thing genuinely cannot leave Swift, it should at least leave the view
and land in `FoolishKit`, so the iOS app can reuse it.

## The direction of travel

**iMessage is the spec.**
Not the web.
The extension's behaviour has been refined by hand over dozens of device rounds
and the owner prefers it.
When an iMessage rule lands in C it **replaces** whatever rule it meets there;
the web becomes the client that re-derives.
Do not reconcile an incoming rule with the web's version - that is backwards.

## The boundary

Only **rendering** is irreducibly per-platform: interpolation and springs, view
updates, screen coordinates, gesture previews, anything typed in `CGRect` /
`CGPoint` / `Angle`.

Everything above that line is pure data transformation and belongs in C:
ordering, grouping, which counts freeze until when, which cards are veiled,
which seats change role and when, what a gesture resolves to, what a conflict
means.

A useful test: if the function would give the same answer on a watch with a
different screen, it belongs in C.

## No JSON

**Every new kernel entry this campaign adds crosses as PACKED BYTES.**
Not JSON.
This is the owner's standing position and it is not a preference about taste -
task #17 spent a whole round taking production Swift off JSON decode, and a new
JSON entry hands that ground straight back.

So:

- A new `fio_*` entry gets a fixed-layout byte blob and a Swift decoder beside
  the other wire decoders in `sdk/swift/`, in the shape `fio_state_packed` /
  `fio_legal_packed` / `fio_bot_drive_packed` already use.
- Where a stage meets an existing `*_json` entry that it is replacing, the JSON
  one goes.
  `fio_anim_plan_json` is the case in point: its only caller in the entire repo
  is the C smoke test, so Stage 4 replaces it with a packed twin and deletes it
  rather than leaving two plans that can disagree.
- `c/src/json_out.c` is the ONE exception and stays.
  It is how non-Swift hosts (the web, through wasm) read the kernel's formats,
  and it is a reader of packed bytes rather than a second format.
  Refactoring it is fine; growing it for a Swift caller is not.
- `docs/ANIMATION_CORE_C.md`'s "Mac session" checklist opens by telling you to
  call `fio_anim_plan_json` and decode it with `Codable`.
  That step is stale and this rule overrides it.

If a packed layout feels like too much ceremony for what you are moving, that is
usually a sign the thing should cross as a handful of ints rather than as a blob
at all.

## Comments

The Swift these rules are moving out of carries very long archaeological comment
blocks - the history of a bug, the owner's words, which round it was found in.

**Do not copy those blocks into C.**
Compress each to a short statement of what the rule *is* and why it is not the
obvious thing.
A sentence or two.
The history stays in git and in the Swift file's own history; the C header is a
specification, not a scrapbook.

Where a Swift site is deleted outright, its comment goes with it.
Where a Swift site becomes a call into C, leave one line saying which C function
now answers.

## House rules that apply to every stage

- **Mutation-check every test.**
  A test that passes against the broken version of the thing it guards is not a
  test.
  Break the rule deliberately, watch the test fail, put it back.
  Say in the test file which mutation was run.
- **Prefer delete.**
  Code that is dead in production and kept alive only by its tests goes, and the
  tests go with it.
- **No em dashes** anywhere - in code, comments, commit messages or prose.
  Plain `-`.
- **Never freeze replay codes as fixtures.**
- One sentence per line in long Markdown.
- Commit messages: lower-case conventional prefix, a sentence that says what is
  now true rather than what was done.

## Verifying

    cd c && make tests && ./build/cnitro_tests      # 3200+ cases, must be 0 failed
    cd c && make ios-lib                            # rebuilds the xcframework
    ios/scripts/mac_tests.sh                        # FoolishTests + HarnessTests + shipping build
    npm run test:swift-parity                       # the Swift/TS codec gate

Editing anything under `c/` makes `ios/vendor/Foolish.xcframework` stale and a
FoolishKit build phase will refuse to link until `make ios-lib` has run.
That guard is doing its job; run the rebuild, do not disable it.

`ios/scripts/mac_tests.sh` handles the `xcodegen`-blanks-the-entitlements
landmine on its own.
Do not run `xcodegen generate` by hand without restoring
`ios/FoolishApp/Foolish.entitlements` with `cp -p` afterwards.

## Where things are

| what | where |
|---|---|
| the animation core | `c/src/anim_plan.{c,h}` |
| the event wire (writer, and now the reader) | `c/src/evwire.{c,h}` |
| legality and the move menu | `c/src/legal.{c,h}` |
| JSON emission for hosts | `c/src/json_out.{c,h}` |
| the iOS bridge | `c/ios/ios_api.c`, `c/ios/include/ios_api.h` |
| C tests | `c/tests/tests.c` |
| the Swift board | `ios/FoolishKit/Boards/MessageTableView.swift` |
| the staging/send controller | `ios/FoolishKit/Messages/MessageTurnController.swift` |
| the Swift kernel bindings | `sdk/swift/` |

## Shipping

Superseded 2026-09-05: the owner asked for the lift itself to be bumped and
uploaded ("bump and upload this version, then go forth with the things we
queued"), so the campaign's original "the owner runs these builds by hand" no
longer holds for this branch.
See reference notes for the archive/export steps; the scheme is
FoolishMessagesApp, and `destination: upload` needs the owner signed into Xcode.

## The campaign is finished

Seven stages, ending 2026-09-05.
What moved: the wire's reader and the settlement cut, what a gesture on a board
means, the shape of a sequence, the count freeze, the pre-bout table, the
conflict verdict, and finally the board's own sets - the veil, the hand's layout,
the table under a sweep, the end screen's order and who may write the shown
counts.

`docs/ANIMATION_CORE_C.md` carries the closing inventory under **"What stayed in
Swift, and why"**: the rendering that correctly stays, the two liftable things
that were deliberately left (the harness's `autoPick`, which is a `#if DEBUG` rig
reading an environment variable, and `MessageTurnController.sentBytes`, which is
a rule about the iMessage host's payload handoff with no kernel concept behind
it), and the one thing that is neither and should be looked at (the extension's
own on-disk data is still JSON).

Read that section before starting an eighth stage.
It exists so the campaign closes as finished rather than abandoned.

## Queued for after the lift: the determinism pass

Not part of this campaign.
Recorded here so it is not lost, and so nobody starts it early.

The invariant, in the owner's words: **the only true nondeterministic randomness
should be when we seed a live game.**
That is already the design - one crypto draw per game at the deal
(`injectDealSeed`, `sdk/ts/wasm/engine.ts`), with mid-game engine randomness and
bot decisions both reseeded deterministically from the deal seed, so a whole game
replays from it.
The bot seed folds in the never-client-visible deal seed on purpose, so a
Monte Carlo bot's rollout stream is reproducible only to the server that holds it.

What is not yet true is that anything enforces it.
The comments in `engine.ts` and `bots.ts` record that per-move `Math.random`
reseeding was there once and was removed, so this has been broken before.

The work, when it is picked up:

1. Seven e2e suites still shuffle with `Math.random`, so they run a different
   experiment every run and a failure hands the reader no repro:
   `concurrent_games`, `meta`, `attack_cover_parity`, `server`, `reconcile`,
   `resilience`, and whatever remains of `replay_codec`.
   `bot_parity.test.ts` already does it correctly - it patches `Math.random` with
   an LCG and restores it afterwards.
   That is the house pattern; hoist its `mkLcg` into a shared `e2e/` helper
   rather than inventing a second one.
2. Seed from an env var with a fixed default, print the seed, and name it in
   failure messages.
   Seeding must not shrink what a suite explores - same number of trials, just
   reproducible.
3. A CI gate over `e2e/`, `sdk/` and `server/`.
   `sdk/` has zero real calls today (every hit there is a comment about the ones
   that were removed, so the check must match calls and not the string).
   `server/` has exactly one, `meta_actions.ts`'s random lobby bot, which should
   stay and should be allowlisted by name with its reason beside it - lobby
   composition is not gameplay, and the chosen bot is recorded in the game.
   Do NOT extend the gate to `src/` (cosmetic textures, React keys, error ids) or
   `offlinefun/` (research arenas, where randomness is the point).

## Queued: the transport mode, and the conflict rule that reads it - DONE

Landed 2026-09-05, as specified below.
`anim_set_transport` / `anim_transport`, one `if` at the end of
`anim_conflict_verdict`, and `anim_resolve_unconfirmed_attack_covers` reduced to
marshalling over the same verdict.
The two modes are proven to disagree on the stage-6 input in one process (C) and
again through the browser's own module, and to agree on everything before the
question.
See `docs/ANIMATION_CORE_C.md` "The transport: one rule, two clients, one
question" for the shipped shape.

One answer changed deliberately: the server rule short-circuited the whole
pending set on the first card the broadcast already showed, and each card is now
judged on its own.
`AnimationContext` only calls when NONE is accepted, so no caller can produce
that shape.

The original spec follows.

Stage 6 (`9357178`) found that the iMessage conflict rule and the web one answer
different questions, and stopped rather than making the browser worse.
The difference is not the verdict - it is **how a client knows whether its own
optimistic card survived**.

- iMessage: every message carries the whole game, totally ordered.
  When a newer chain arrives it is the complete truth, so doom is knowable
  locally and immediately.
- Everything else (web, iOS, watch, Steam) goes through the server.
  A card's confirmation is its own later broadcast, so "the newest news does not
  mention my card" means "the receipt is still in the post", not "it was
  rejected".

Reading the second as the first is what would have put the card-out /
card-home-in-red / card-out-again stutter back into the browser.

**The design: a transport mode set once at initialization, not a flag threaded
through every call.**
iMessage is the odd one out and every other client shares the server's shape, so
the mode is a property of the app rather than of the question being asked.

There is a precedent to copy: `fio_set_passing` is exactly this - a
session-scoped term of the table, set after adoption and before use, reported
back by `fio_passing_allowed`.
Follow that shape rather than inventing another.

Three guardrails, each earned by something this repo has already been bitten by:

1. **No silent default.**
   A call that depends on the mode before it has been set returns an error
   rather than guessing.
   The A1 roster cutover is the precedent: unlinked bots silently played
   `random`, and nothing noticed until the fuzz was made honest.
2. **A test that proves the two modes DISAGREE** on the same input.
   Without it the flag is untested plumbing, and the pending-attack case from
   Stage 6 is the obvious one to pin: server says keep, iMessage says revert.
   Both must also be exercised in the same process, because the FMSG e2e
   concurrency suite already drives chain behaviour from a server-shaped host.
3. **The mode is visible in diagnostics.**
   A wrong-mode bug looks exactly like an animation bug, and the only cheap way
   to tell them apart is to be able to read the mode back.

### Shape of the work: share everything that can be shared

The mode is not a fork in the road.
It selects ONE extra step, and everything either side of that step is the same
code running on the same inputs.

Owner's instruction: the doom determination shares as much code as possible.
So do not stop at "the trichotomy is shared" - most of the doom test is shared
too, and only one question inside it is transport-specific.

What is IDENTICAL in both transports, and must be written once:

- the CLEAR test - does the incoming stream itself move this card.
  Checked first in both, for the same reason: a card the arrival animates must
  not fly home red before it, and a pickup's cards stand on the table it sweeps.
- the KEEP test - does the card stand where the motion put it, on the board the
  newest truth vouches for.
  The server calls that board the authoritative table; iMessage calls it the
  arriving chain's opening board.
  Different name, same question, same set membership.
- the pool rule - a card that went into a pile or a badge is never reverted,
  because conjuring a ghost back out of a pile is its own class of bug.
- the masked-back rule - an opponent's face-down draw has no identity to
  conflict on and no view to fly back from.
- the reversal: which motions fly, in reverse group order, empty groups dropped.

What is GENUINELY transport-specific, and is the whole of the flag:

> Once a card has failed both tests above, is "not accounted for" CONCLUSIVE?

- iMessage: yes.
  A chain is complete and totally ordered, so a card the newest chain does not
  account for is doomed. REVERT.
- Server: no.
  The card's own confirmation is a separate future broadcast that has not
  arrived.
  So before concluding, the server asks its extra question - the
  defender-capacity inference in `anim_resolve_unconfirmed_attack_covers` - and
  keeps the card when the answer is "this could still be accepted".

Concretely that is one branch near the end of one function, not two functions:
the shared path computes CLEAR / KEEP / not-accounted-for, and only the
not-accounted-for case consults the mode.
A reviewer should be able to point at the single `if` that the flag controls.
If the diff ever grows a second one, the split has been drawn in the wrong place.

`anim_resolve_unconfirmed_attack_covers` and `anim_conflict_verdict` then stop
being two rules and become one rule with one transport-dependent question.

## Queued: the iMessage chain layer

Third in the queue, after the determinism pass and the transport mode.
Owner's instinct was that all of `ios/FoolishKit/Messages` (2,388 lines) should be
C.
Measured, it splits three ways, and only two of them are worth doing.

### Does not move, and should not

About 1,400 lines are the Messages framework and platform storage, not rules
wearing a UI hat:

- `MessagesRootView` (1,142 lines, by far the most platform-coupled file in the
  directory) - `MSMessagesAppViewController` lifecycle, conversation objects,
  presentation-style transitions, the lobby and nickname screens.
- `MessageGameStore` (145) - App Group `UserDefaults` shared between the app and
  its extension.
- `MessageSummary` (129) - the bubble caption.
  Captions stay in the sender's language by owner's decision, so the strings
  cannot move; only the choice of which summary fits which event could, and that
  is a few dozen lines.
- `CollapseTween`, `MessageDevBoard`, `ChatKey` - presentation timing, the dev
  board, a key type.

### Item 1: the gates.  Small, obviously correct - DONE

Landed 2026-09-05.
`StaleBranchGate.isAhead`, `NicknameGate` (both caps and the taken-name scan)
and all four of `SeatIdentity`'s decisions are `msg_wire.c` rules now, reached
through packed rosters and length-counted names.
`StagedBubbleRouting` was deliberately LEFT: it is URL work plus one byte
comparison, and a second client cannot get `a == b || a == c` wrong.

The `SeatIdentity` wrinkle was resolved rather than dodged: `game.h`'s "seat
identity is deliberately not in the state blob" is not reversed, because no seat
goes into any state and every signal the rules read is still handed in by the
host.

The original assessment follows.

`StaleBranchGate` (59), `NicknameGate` (19), `StagedBubbleRouting` (22), and
possibly `SeatIdentity` (48).
Zero platform coupling, pure decisions, the same shape stages 2 and 7 moved.
About 150 lines.

One wrinkle on `SeatIdentity`: `game.h` states that seat identity "is
deliberately not in the state blob; it lives with the caller."
Moving it reverses a documented decision, so do it consciously or leave it.

### Item 2: the turn controller as a transition function.  The one with leverage

`MessageTurnController` (474) is not a pure function - it is a state machine over
time.
Its historical bugs were never wrong rules; they were concurrency
(`never seal OR READ across an await`, the resident game being one slot,
`markSent` rebasing backwards).
C cannot own those, because the suspension points are Swift's.

But it can own the decisions ACROSS them, and this repo has already proved the
pattern: `bot_drive` returns the actions AND the pacing, and the host decides
only how to wait.

Same shape here - `(chain state, event) -> (new state, effects)` in C, with Swift
pumping it and performing the effects.
The awaits stay Swift; what may be staged, what is withheld, and what a send
means stop being Swift's.
Roughly 250 lines of decision logic, not 2,388.

**Trigger: a second chain-based client.**
Today nothing else speaks FMSG, so the move buys correctness-by-construction and
no reuse.
When a watch, an Android client or anything else adopts the chain, this is the
piece that stops it re-deriving the staging protocol - and staging is where the
extension's worst bugs have lived.

Note that these 2,388 lines already satisfy the campaign's fallback rule: they
are in FoolishKit, not in a view, so nothing here is stranded in the extension.
The question is reuse, not rescue.

## Queued: findings the campaign turned up but did not fix

These are not lifts.
They are defects and gaps found while moving other code, deliberately not fixed
in passing, and recorded here because a commit message is not where anyone looks
for work.

### A real bug: the prior board is nil more often than it should be - FIXED

Fixed 2026-09-05.
The self-check counted EVENTS where a step is a FRAME, and the board came off
the extra frame's first event where it should come off its TRAILER (the state
that step committed, which is defined even for a step that emitted none).
Measured on the real-game sweep in `c/tests/tests.c`, whose prior-board model
now reads the same trailer: 26 of 1291 sweeps fell back to the flat table with
16 reshaping the grid, and it is 5 and 5.
Those 5 are one 20-card pickup the test build's `MAX_LOG_PAIRS=16` under-names,
not a missing board, and the suite now asserts that is the only way a flat
reading can happen.

The original report follows.

`MessageEnvelope.lastMoveEventsWithPrior` returns nil whenever the previous step
emitted anything other than EXACTLY ONE event - and a bare `good` emits none.

Measured in stage 5 over 5,810 pickups: **157 of them** fall back to the flat
one-battle-per-card table because of this, and the flat shape differs from the
real table in over half of all cases.
On screen that is the grid re-arranging itself just before the cards fly.

Not fixed with stage 5 because the same prior ALSO seeds the role marks, where
"the first event of the previous step" is deliberate.
So this is a change with two consumers and wants its own look.

### An untested load-bearing rule: the empty menu under a held settlement

While a bout settlement is withheld, the board publishes an EMPTY legal menu so
the player cannot act on a deal they have not been shown.
Stage 2 mutated away both halves of that rule and **the whole suite stayed
green**.

The reason no test catches it: every held bout end the fixtures reach is a pickup
or a good, after which the acting seat is not the next actor and the raw menu is
empty anyway - so the assertion passes against no rule at all.
The one shape that would catch it is a defender's cover that empties their own
hand, which no fixture reaches.

Stage 2 removed the duplication the rule could hide behind rather than faking a
test.
Writing the missing fixture is the actual fix.

### A CI flake: edge-serve 502s under the real memory budget

The `memory` workflow's `edge-serve` job failed on main with
`semtex,octogen returned 502` at commit `fb9294e`, then passed on re-run with a
BYTE-IDENTICAL tree (`798b003b` both times), so it is a flake and not a
regression.
Two Monte Carlo bots timing out under the edge runtime's real memory budget is a
resource-pressure symptom worth understanding rather than re-running.

### Housekeeping

- Three stale git worktrees: one marked `prunable` from an old session,
  `/private/tmp/foolish-drag-wt`, and the flake-fix worktree now that #114 is
  merged.
- A background-shell trap worth knowing about, since it cost seven stuck
  processes: `until ! pgrep -f foo.py; do sleep 5; done` never exits, because the
  waiting shell's own command line contains `foo.py` and `pgrep -f` matches
  itself.
  Use `pgrep -f "[f]oo.py"` or wait on a PID.

## Device findings from the 1.0(43) pass - ALL THREE FIXED

Owner-reported, on device, after the lift stages landed.
Not lift work.
Fixed on this branch after 1.0(44) was uploaded, so the fixes ride the next build.

Each was reproduced on the rig BEFORE it was touched, and each fix was verified
by the same measurement afterwards.
The two hypotheses recorded here beforehand are kept below, because one of them
was WRONG and the shape of the error is worth having: the guess named the right
symptom class and the wrong layer, and it was the owner's own "just a hypothesis"
that turned out to be the cause.

### 1. Replaying my own attack sneaks the card back into the hand first - FIXED

Owner: "we shouldn't start with 5 cards, fade the one I threw back in and
rearrange animation, then throw it out.
The visual should START with the 6 cards, and just fly the one."

**Reproduced**: `HARNESS_SCENARIO=arrival HARNESS_ARRIVE_COLD=1
HARNESS_ARRIVE_SELF=1 HARNESS_ARRIVE_KIND=attack HARNESS_PLAYERS=2`, which is a
cold open of a bubble whose last move is mine.
The unified log carried the owner's own trail:

    fan-rows 1 rows laid=6 hand=5 held=1 ... settled=true seq=0
    stream#1 step attackPass@1 n=1 flights=1
    fan-rows 1 rows laid=5 hand=5 held=0 ... settled=true seq=1

The tell is that the FIRST line exists at all.
It comes from `.onChange(of: laidHandCount)`, and an onChange only fires on a
CHANGE - so the count was 5 for every paint before it and became 6 afterwards.
The board painted the settled five-card hand, then re-laid the fan at six and
faded the played card in, then flew it.

**Cause**, `MessageTableView.swift`: `handHoldback` is armed inside
`replayLastMoveOnOpen`, which the file described as landing before the first
paint - and structurally cannot, because it runs from `.onChange(of:
controller.view)`.
`settled`'s own doc had already said so about `freezeCounts`; the holdback was
the one veil that had never been given the same cure.

**Fix**: `fanHoldback`, a computed property beside `pendingOpen`.
While `unstartedReplay` is open the answer comes from the controller
(`HandLayout.myPlacedCards` over the same stream the arming uses), and after it
shuts the armed `handHoldback` answers, as before.
Every render site now asks it; nothing that draws reads `handHoldback`.
A union rather than a swap, so a sequence still flying its own cards does not
have them pulled out of the fan when a second bubble raises a fresh veil.

**Verified**: the `seq=0` line is gone.
The only `fan-rows` left is the drop to five, and it now fires at flight start -
the fan closing as the card leaves, which is what round 42 wanted.

### 2. Table-to-discard flies with expanded coordinates on a collapsed board - FIXED

Owner: "table to discard animation still seems to have a geometry mismatch like
it's using expanded coords on a collapsed screen."

MY EARLIER HYPOTHESIS, AND IT WAS WRONG: that this was round 43's hand-side bug
on the table side - `tableCardSource` reading the published `lastBattleCardFrames`
instead of computing, the way `handSlotsNow` computes.
Measured, the table's per-card frames track a collapse faithfully: a probe on
`BattleCardFramesKey` shows them going from y=345 to y=142 in the same instant
the drawer snaps.
Nothing on the table side needed computing.

**Reproduced**: play the bout-ending move myself, let the board auto-collapse,
then Send - which is when the withheld settlement is released and the sweep
actually runs.
`HARNESS_SCENARIO=arrival HARNESS_ARRIVE_KIND=cover HARNESS_AUTOMOVE=1
HARNESS_AUTOMOVE_KIND=good HARNESS_AUTOSEND=1 HARNESS_AUTOSEND_DELAY_MS=3500`.
The rig could not pose this before: `HARNESS_AUTOSEND` pressed Send after a fixed
600ms, which outran the post-stage auto-collapse (`HarnessModel.stage` waits for
the move's own sequence to settle first, and bails the moment the bubble is
delivered), so every AUTOSEND run had sent from an EXPANDED board.
The delay is a knob now.

With the collapse first, the sweep flew:

    OFF-HAND flight opendiscard-0-6 from=(151,345) to=(328,45) hand=(187,623)

on a 261pt drawer.
y=345 is the expanded table centre and 623 the expanded hand: the cards left from
200pt below the bottom of the visible drawer.
The same run with a MANUAL collapse instead flew `from=(151,142) ...
hand=(187,217)` - correct - which is the control that made finding 3 the answer.

**Cause**: finding 3, below. Same fix.

### 3. The auto collapse leaves every published frame measured on the expanded board - FIXED

Owner: "geometry seems to be broken by the auto collapse? then fixed by swiping
to expand/collapse", and their own caveat, "not entirely confident ... it's just
a hypothesis."

The hypothesis was right, and it is finding 2's cause rather than a separate
report.
Three seconds after an ARMED collapse, with the drawer compact and still:

- the board's own GeometryReader reads 243 - the layout is correct;
- `handFrame` still publishes midY 623 and the battle cards y 345 - every
  PUBLISHED frame still describes the expanded box;
- a MANUAL collapse takes `CollapseTween.step`'s `.follow`, never touches the box
  height, and publishes correctly throughout.

That difference between the two paths is exactly what the owner noticed, and it
is not staleness in any one reader: it is every landmark a flight aims at.

**Cause**, `MessagesRootView.follow`: the armed path holds the box at the
expanded height and eases it down (round 10d, filmed, and correct).
`boxHeight` runs 0 -> expanded -> (animated) target, and the target IS the height
the box rests at - so handing the override back (`boxHeight = 0`) is numerically
no change at all.
SwiftUI delivered the subtree's preferences once, from the expanded pass that
opened the animation, and never had a later height CHANGE to deliver from.

**Fix**: the tween releases through `CollapseTween.handBack`, half a point off
the rest height and back one frame later.
Two real layout changes, sub-pixel, and the second republishes every landmark at
the size actually on screen.
Skipped entirely when `follow` has already released the box, so a manual drag
mid-tween cannot be handed a compact height.

**Verified**: the same run now flies `from=(151,142) to=(328,45) hand=(187,217)`,
identical to the manual-collapse control, and `SLOTCHECK MISMATCH n=... worst=406.0pt`
- which had been firing on every auto-collapse - is gone.

### One rig scenario is inert, and was not fixed

`HARNESS_SCENARIO=myplay` was built in round 42 as "a COLD OPEN of my own move …
the only one that arms `handHoldback`", and it no longer poses that: it opens
with `openReplay events=0`.
`dealDriven` delivers its bubble through `deliverSealed`, which stages and then
presses Send - and a chain this device just sent is opened QUIETLY on purpose
(`MessageTurnController.suppressOpenReplay`), which is correct behaviour and
kills the scenario.
`arrival` with `HARNESS_ARRIVE_COLD=1 HARNESS_ARRIVE_SELF=1` poses the same shape
and is what finding 1 was reproduced and verified on.
Left as it is; noted so the next person does not read a green `myplay` run as
evidence of anything.

### How to work these

The rig is the right tool and it already exists: `FoolishHarness` plus the unified
log (`subsystem == "cards.foolish.anim"`), with the oracles `staleAtRest`,
`backwardsPaints`, `vanishedAtRest`, `strandedAtRest`, `sweepVisibleNow`,
`veilStandingNow`.
Two of those oracles have previously carried baselines that hid defects, so check
what they assert before trusting a green run.

None of the six saw any of the three findings above, and that is worth knowing
rather than holding against them: all six are about WHAT the board is showing and
none is about WHERE.
A geometry oracle - "no flight may take off from a point outside the board" -
would have caught findings 2 and 3 on any of the collapse runs the rig has been
making for rounds.

## Queued: the JSON that is left - THREE OF FOUR DONE

Landed 2026-09-05.

- **3 (the kernel crossing) DONE.** `fio_msg_encode`, `fio_msg_carry`,
  `fio_msg_start_rematch` and `fio_msg_penalty_fool_seat` take the packed roster
  - `n_joins(1)` then `n_joins x {seat(1) name_len(1) name[]}`, byte for byte
  the tail `fio_msg_decode_packed` hands back.
  `sdk/swift/RosterWire.swift` is the one codec.
  Nothing on the wire moved.
- **1 and 2 (the on-disk stores) DONE**, after the owner extended the scope
  ("turn the on-disk stores away from JSON too") and waived the migration
  ("just break in progress games if you need to").
  `MessageGameStore`'s three maps and `ReplayStore`'s index are
  `PackedWriter`/`PackedReader` bytes behind a format byte; the keys and the
  filename were bumped, so the old JSON is never read rather than migrated, and
  the old `replays.json` is left on disk rather than deleted.
- **4 (the client-server envelope) NOT DONE, and deliberately.**
  `ios/FoolishNet/PackedGame.swift`'s roster island is the last JSON, and it is
  the SERVER's envelope (`GAME_RESP_FORMAT`): changing it means changing the
  server's encoder and deploying both in lockstep, which the owner's "try not to
  change wire format if you can - only the ondisk container" rules out.
  The file now says so at the site instead of claiming the whole payload is
  packed.
  When it IS picked up, the work is the server's encoder emitting the same
  `n_joins/seat/name_len/name` shape plus `is_ai` and the game's id/name/status,
  and `PackedGame.decode` reading it with `PackedReader`; it needs a coordinated
  deploy or a version byte in the envelope, not a client change.

The original spec follows.

Owner: "I REALLY don't like JSON."
Four items, queued together.
Stage 7 ADDED none of this - its diff contains no new encoder, decoder or
`Codable` - it reported what was already there.

### The finding that ties 3 and 4 together: the roster is the last JSON

Everything else in this system went packed.
The roster - seats, names, is_ai - did not, because it is strings, and it is now
the only JSON left on any path that matters.

**The packed client-server payload carries a JSON island inside it.**
`ios/FoolishNet/PackedGame.swift` decodes `magic | flags | seat | version |
rosterLen | ROSTER | viewLen | packed state`, and that roster segment is
`JSONDecoder().decode(Roster.self, ...)` - sitting a dozen lines above a comment
that reads "no kernel JSON round-trip (owner: wipe the JSON; client-server is
packed kernel wire)".
The comment is true of the state and false of the roster.

**The same roster crosses into the kernel as a JSON string.**
`fio_msg_seal`, `fio_msg_carry`, `fio_msg_start_rematch` and
`fio_msg_penalty_fool_seat` all take `const char *joins_json`, and
`sdk/swift/MessageEnvelope.swift` builds it at four sites.

**And a packed layout for exactly this data already exists and is already
shipping.**
`fio_msg_decode_packed`'s blob ends with `n_joins(1)` then
`n_joins x {seat(1) name_len(1) name[]}`.
So the kernel currently PARSES JSON in order to produce a format it already knows
how to write and read.
That is the whole of items 3 and 4: use the layout that exists.

Watch the name budget when doing it - names are <=64 UTF-8 bytes and there is a
12-byte display cap from the App Store review pass; a length-prefixed byte string
handles both without the escaping rules JSON drags in, and
`e2e/imessage_replay_names.test.ts` already gates a Swift/TypeScript name codec
byte-for-byte, so there is a pattern and a test shape to copy.

### 1. `MessageGameStore` - App Group storage, 7 sites

Seat rows, latest-chain rows, hand order, all `JSONEncoder`/`JSONDecoder` into
`UserDefaults` in the shared container.
Client-local: no other client reads these bytes and no rule is derived from them.

Lower value than 3 and 4 - it crosses no boundary - but it is JSON on disk, the
owner does not want it, and the rows are small fixed shapes that pack trivially.
Do it AFTER 3 and 4, and reuse whatever encoder those produce rather than
inventing a third.
Storage needs a migration story that a wire does not: the container already holds
v2 rows, so either read-old-write-new or a version byte.

### 2. `ReplayStore` - the saved-replay index on disk, 5 sites

`[ReplayRecord]` encoded to a file.
Already known and already deferred once (see the owner's note that they dislike it
but that it is not a task-#17 wire remainder).
Same category as item 1 and the same migration caveat; do them together.

### What is NOT ours, and should stay

`ios/FoolishNet/GameFeed.swift` decodes Supabase realtime rows
(`struct ViewRow { view: String; version: Int? }`).
That is the Supabase client's own protocol, not a format this repo chooses, and
the payload inside `view` is already a packed blob.
Replacing it means replacing the transport.
Leave it.

## Queued: the iOS lobby needs the bot picker

NOTE ONLY - do not build this as part of the determinism pass.

Two clients disagree about how a bot is chosen, and the `Math.random` in
`server/impls/supabase/functions/_shared/adapter/meta_actions.ts` is a symptom
rather than the problem.

- **Web**: `src/components/Lobby.tsx` has a picker and passes the chosen id
  (`addBot(game_id, bot?.id)`).
  The optional chaining is deliberate - the code beside it handles the
  no-id case explicitly ("the random fallback has no id yet, so it keeps a temp
  id") - so the random path is still reachable there too, just not what anyone
  taps.
- **iOS app**: `ios/FoolishApp/LobbyView.swift` is a plain "Add Bot" button
  calling `game.addBot()` with no id, so `OnlineService.addBot(gameId:)` sends
  `bot_id: nil` and the server takes the random branch.
  The iOS lobby never got the picker.

**The work, when it is picked up:**

1. Give the iOS lobby the same picker the web has - tap through the bot options,
   tap the one you want.
2. Then delete the server's random branch, since nothing will reach it.
3. Then remove the allowlist entry from the `Math.random` CI gate, which stops
   needing an exception at all.

Until step 1 lands, the server's random pick must STAY - deleting it would leave
the iOS "Add Bot" button unable to add anything - and the gate's allowlist stays
with it.
The allowlist entry should say it is temporary and name this note, so nobody
reads it as a permanent blessing.

The point is not the randomness.
It is that "which bot gets added" is answered two different ways depending on
which client asks, which is the class of divergence this whole campaign has been
closing.

## Queued: delete the dead C build flags

Owner: "All of it. It's in git history if we need to tune it again."

Every dead flag is in BOT RESEARCH code.
Nothing in the game core, the wire, or the animation layer is guarded by a flag
no build sets.

### Delete: read by the code, set by no build target

| flag | guarded lines | file |
|---|---:|---|
| `CD_TT_TRACE` | ~95 | `cordite_sim.c` |
| `CD_TT_RANKSYM` | ~79 | `cordite_sim.c` |
| `CD_TT_STATS` | ~67 | `cordite_sim.c` |
| `CD_TT_BOUNDS` | ~49 | `cordite_sim.c` |
| `CD_TT_SUITSYM` | ~48 | `cordite_sim.c` |
| `CD_TT_BOUNDS_USE` | ~32 | `cordite_sim.c` |
| `CD_TT_TAILCACHE` | ~17 | `cordite_sim.c` |
| `GRPO_RNG_DEBUG` | ~14 | `game.c` |
| `CD_TT_ADAPT` | ~11 | `cordite_sim.c` |
| `OG_HIDE_UNCOVERABLE` | ~11 | `octogen_strategy.c` |
| `CD_TT_DEPTH_PREF`, `CD_TT_ORDER2`, `CD_TT_ORDER3` | ~9 | `cordite_sim.c` |
| `CD_SIM_SOLVE_MAX_DEPTH` | ~3 | `alphabeta_probe.c` |

About 435 lines over ~50 sites.
`cordite_sim.c` is the largest C file in the repo at 1,727 lines and roughly a
quarter of it is unreachable.

The `CD_TT_*` family is transposition-table tuning scaffolding from the cordite
research - trace, stats, suit and rank symmetry folding, bound tightening, move
ordering.
Cordite is the ELO #1 bot, so this is the record of how it got there.
The owner's call is to delete it and rely on git.

Delete the guarded code WITH the guard.
An `#ifdef` whose body is removed but whose flag survives in a comment or a
Makefile note is the same vestige in a smaller size.

### Do NOT touch: tunables that only look like dead conditionals

`GUNPOWDER_MODE`, `CD_TT_TAIL_K`, `CD_TT_TAIL_N`, `CD_TT_BOUND_MINCARDS`,
`LEAFBOOK_K`, `REPLAY_BN_CAP` are `#ifndef X / #define X <default>` idioms.
They guard ZERO lines.
Deleting the guard deletes the default and breaks the build.
`CNITRO_WASM_SIZE_T` is a typedef guard in the wasm libc shims.

### The other direction: are any flags always on, so the guard is pointless?

Checked, and the answer is **no** - with one near-miss worth writing down,
because it is the one a careless sweep would remove.

`CD_LEAFBOOK` sits in the GLOBAL `CFLAGS` (`Makefile:31`), so every native target
has it and the guard looks redundant.
It is not.
The `leafbook` target builds with its own `LEAFBOOK_CFLAGS`, which deliberately
omits it - the Makefile says so in as many words: "The build_book pass compiles
WITHOUT -DCD_LEAFBOOK (it builds the book by direct solves)".
The guard IS the bootstrap.
Remove it and the book becomes unbuildable, because building the book would
require the book.

Everything else - `CD_WASM_OVERLAY`, `CD_RULES_OVERLAY`, `CD_TT_2WAY`,
`CD_TT_PACK8`, `DEAL_RNG_DISABLED`, `FOOLISH_ORACLE_BUILD`,
`FOOLISH_SEEDED_BOTS_ONLY`, `GUARDS_VALIDATE_ONLY`, `REPLAY_STATS`,
`LEGAL_STATS`, `OG_EXPLAIN_BUILD` - is set by one or two named targets and is
genuinely conditional.

`ACCELERATE_NEW_LAPACK` is in the global `CFLAGS` and read by nothing in this
repo: it is Apple's own Accelerate macro, consumed by a system header.
Not ours, leave it.
(`_T` in an earlier scan was an artifact of a regex chopping
`-D_Thread_local=`; there is no such flag.)

### How to verify the deletion

Deleting dead code cannot change behaviour, so prove that rather than assert it:
build every target that touches these files and diff the binaries, or failing
that run the bot benchmarks and hold the ELO table.
`make tests`, `make difftests` and the bot parity suites must be unchanged.

## Settled: the Infinite Oracle's endgame scores, measured

The queued entry here said the Oracle's Monte Carlo scores INVERT in the last
few plies, and offered a HYPOTHESIS that the fault was in the rollout rather
than the world sampling.
Both halves have now been measured against ground truth.
The inversion is not real; the pessimism about the numbers is.

Reproduced with the committed harness, not a new one:
`make og_explain`, then `OG_EXPLAIN=<file> ./build/og_explain <seed> moves deal`
driving the recorded game (`tests/og_explain.c`, `driven_replay`).

**The ranking at the cover of the last eight is CORRECT.**
An exhaustive minimax over the real kernel (`calculate_legal_moves` + the
`handle_*` entries, with the exact solver taking the deck-empty tails) says:
cover with the JACK is a proven LOSS in every world; cover with the KING is a
proven WIN in the world where the opponent holds no trump king, and cannot be
worse than the jack anywhere, since the jack loses everywhere.
(Enumerated over all six placements of the last deck card among the unseen
cards, a superset of the five the belief admits.)
Keeping the jack is what wins there: with the trump jack drawn off the flip, a
jack attack lets her throw the trump jack on her own rank and go out.
So the brief's premise, "the two moves differ only in which club she keeps, and
played out both lose", was wrong, and the Oracle preferred the right move.

**The magnitude is wrong, and the cause is the rollout OPPONENT, not the axis.**
Both numbers are the same axis: a mean finish position over the sampled worlds
(528 and 864 here).
Nothing mixes a proof into a frequency here.
The king cover scores 1.013 because the rollout's opponent almost never plays
the refutation, the trump-king throw-in onto the king she just laid down.
`sim_trump_attack_prob` (`c/src/cordite_sim.c`) returns **0.02** whenever
`deck_n > 0 || has_flipped`, so with one card and the flip still out it declines
the throw 98% of the time.
That is a faithful mirror of `trump_attack_probability` in
`handwritten_strategy.c`, so it is the modelled opponent behaving as specified,
not a fidelity bug.
Truth is about 1.8 for the king cover against the Oracle's 1.013: it wins in
the one world of five where the opponent cannot answer.
The five worlds where the throw-in is available are 14-card endgames the exact
solver does not resolve inside its budget, so they are read as losses from the
line rather than asserted as proofs.
Turning the exact leaf endgames off (`OG_BBLEAF=0`) scores every candidate at
1.0, which is the same optimism with the exact tail removed.

**A second, separate reason the Oracle can be wrong one ply earlier.**
At the three uncovered eights the position has 27 legal moves.
`og_pick_candidates` keeps at most 10 covers, ranked by the product of card
scores, and the only FULL cover is a three-card move whose product ranks it out.
The Oracle scores what octogen considers, so a move that is never a candidate
cannot appear in the readout however long it converges.

Ruled out by measurement, not by argument:

- **The leafbook.** A `-UCD_LEAFBOOK` build produces byte-identical wrong
  numbers at all three plies.
- **The belief pool.** The dumped pool is exactly the five cards the position
  admits, with the queen of spades correctly pinned rather than pooled.
- **The harness.** Native `og_explain` and the browser `oracle.wasm.gz` agree.

**What WAS a real bug, and is fixed:** see the next section.
It lives in the exact solver the Oracle leans on, it is the sign confusion the
symptom looked like, and it is simply somewhere else.

## Fixed: a solved endgame did not carry the seat it was solved for

`cd_sim_solve` returns the value of a position from ONE seat's point of view:
`+990` means "the seat I asked about escapes".
The transposition table keyed that value on the position alone
(`sim_fingerprint`, `c/src/cordite_sim.c`), and nothing resets the table between
calls, so the two seats of one endgame shared every entry.
Ask seat A, then ask seat B: B read A's proof unflipped and was told the loser
wins.

Measured on 300 handwritten games: **1747 of 1747** resolved deck-empty
positions came back with the wrong sign for the second seat.
The same at `CD_TT_PACK8 + CD_TT_2WAY`, at `CD_TT_BOUNDS`, at `CD_TT_TAILCACHE`,
at `CD_TT_RANKSYM` and at `CD_TT_SUITSYM`.
It reaches the Oracle: analysing this one 30-decision game, 864 of 869 TT hits
read an entry stored under the other seat.

`solver_difftest` could not see it, and that is structural rather than bad luck:
it calls `cd_sim_solve_reset()` before every solve, so it never asks two seats
of one position off one table.

The fix stores the CANONICAL value, always the lower-indexed IN player's side,
and flips it back on the way out.
That keeps the cross-seat reuse instead of keying it away, and negating a
fail-soft bound swaps LOWER and UPPER, which the bounds path now does.
Guarded by `test_solver_tt_value_carries_its_seat` in `c/tests/tests.c`.

Strength: paired octogen-vs-espresso runs are outcome-IDENTICAL before and
after, at pc2 over 600 games (mean finish 1.160, 84.0% win, histogram 504/96)
and at pc4 over 300 (1.960, 43.0%, 129/83/59/29).
Not "within noise": the same histogram, so the fix changed no octogen decision
in either sample.
It is in shared solver code, so cordite, blackpowder and semtex reach it too;
nothing beyond octogen was measured.

The committed `public/oracle.wasm.gz` and `c/build/bots.wasm.gz` still carry the
old solver: this reaches production only when they are rebuilt.

## Queued: a replay URL fails by naming the wrong fault

A papercut, not a crash, on the most natural input a person can give.

`urlToGame` in `server/api/common/replay/codec.ts` recognises exactly one
prefix, `WWW.FOOLISH.CARDS/`.
Paste the obvious thing, a real link of the form `https://foolish.cards/<code>`,
and the prefix does not match, so the whole string is treated as the code.
The scheme's own letters survive the `[^A-Za-z2-7]` filter and corrupt it.
It then fails with **"unsupported replay format version 11"**, which sends the
reader hunting for a codec problem that does not exist.

This repo has form on exactly this class of defect.
`REPLAY_EHEADER`'s comment argues for naming what an error actually means, and
the whole of `REPLAY_ETOOLONG` (commit `ebcc7a1`, "a game too long to code says
so, instead of malformed input") exists because a refusal was reported as a
corruption.

**SUGGESTED SHAPE, not a decided design**: accept the full URL forms people
actually paste, and if the code still will not decode, say that the input did
not look like a replay code rather than naming a format version that was never
in it.
