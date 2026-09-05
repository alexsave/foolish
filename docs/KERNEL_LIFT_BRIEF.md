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

## Not in scope

Do not bump the version and do not archive or upload.
The owner runs those builds by hand for this campaign.

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

## Queued: the transport mode, and the conflict rule that reads it

Not part of this campaign.
Owner's design call, recorded so it is not lost.

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

### Item 1: the gates.  Small, obviously correct

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

### A real bug: the prior board is nil more often than it should be

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

## Queued: device findings from the 1.0(43) pass

Owner-reported, on device, after the lift stages landed.
Not lift work.
Recorded with the evidence so whoever picks them up does not start from scratch.

### 1. Replaying my own attack sneaks the card back into the hand first

Owner: "when you replay one of your own attack bubbles, I can briefly see a small
transition as the card I played kinda 'moves back in' to my hand.
Then it animates correctly to fly to the table ...
we shouldn't start with 5 cards, fade the one I threw back in and rearrange
animation, then throw it out.
The visual should START with the 6 cards, and just fly the one."
Subtle - "like I'm catching the tail end of it" - and easy to reproduce.

The FlightRecorder log has it (this-session block, a 2p chat, one attack to
replay):

    0.08s  adopt      turn 5, 1 to animate
    0.10s  fan-rows   laid=6 hand=5 held=1  veiled=1 preHidden=1 settled=true seq=0
    0.60s  style      expanded
    0.65s  anim-open  n=1 from=4 seats=0 kinds=atta
    0.77s  fan-rows   laid=5 hand=5 held=0  veiled=1 preHidden=0 settled=true seq=1

`hand=5` is the committed truth; `held=1` is the holdback putting the played card
back so the fan lays 6.
That is the RIGHT resting state to open on.
The defect is the ORDER in which it is reached: the board paints the settled
5-card hand first and the 6-card layout arrives after, so the card fades in and
the fan re-centres before anything flies.

Likely shape of the fix, unverified: the holdback has to be established before the
first paint, not applied from a change handler afterwards.
That is the same class as `freezeCounts`, whose comment already says it must run
BEFORE `apply` and not from the `onChange` the view change triggers, "because
onChange fires after body, so a freeze there is already one paint late".
Look at `handHoldback` / `holdbackIsMine` / `releaseHoldback` and when
`replayLastMoveOnOpen` arms them relative to the first body pass.

Note the half-second between `adopt` (0.08s) and `anim-open` (0.65s): the sheet is
still coming up, which is exactly the window the owner is catching the tail of.

### 2. Table-to-discard flies with expanded coordinates on a collapsed board

Owner: "table to discard animation still seems to have a geometry mismatch like
it's using expanded coords on a collapsed screen."

MY HYPOTHESIS, unverified - but it is a known class in this codebase.
Round 43 fixed exactly this on the HAND side: `handCardFrames` is a PUBLISHED
preference and a preference lags a layout pass, so collapsing the drawer moved the
board while the per-card frames still described the expanded one - the rig caught
it as `SLOTCHECK MISMATCH n=11 worst=391.0pt`, 391pt being the distance the hand
travels between expanded and compact.
The cure was to COMPUTE the slot rather than read it (`handSlotsNow`).

That cure was never applied to the TABLE side.
`tableCardSource` still reads `lastBattleCardFrames` and `discardSource` still
reads `lastBattleFrames`, both published preferences, so the sweep to the discard
pile should show the same lag under the same conditions.

### 3. SUSPICION ONLY: does the auto collapse leave the geometry stale?

Owner: "geometry seems to be broken by the auto collapse? then fixed by swiping to
expand/collapse."
Note the owner's own question mark, and their follow-up: "not entirely confident
on the autocollapse not updating geometry properly, it's just a hypothesis."

Keep the two apart, because only one of them is evidence.

**Observed**, and reproducible: the geometry is wrong at some point, and swiping to
expand or collapse repairs it.

**Guessed**, by me, and NOT verified: that the automatic collapse takes a path
which skips a frame republish the interactive path performs.
That is one explanation of the observation.
It is not the only one.
At least three others fit the same symptom equally well:

- the frames are republished on both paths, but the auto collapse republishes them
  at a moment when the layout has not settled, so the values are fresh and wrong
  rather than stale and right;
- nothing is wrong with the collapse at all, and the swipe merely forces an extra
  layout pass that would have corrected ANY stale rect, whatever staled it;
- the auto collapse and the animation that exposes the bug simply co-occur,
  because both follow a send.

**How to tell them apart**, before changing anything: log the published table rects
against the presentation style on both paths and compare.
If the auto path never publishes, the first explanation holds.
If it publishes the expanded values while compact, the second does.
If the rects are identical on both paths, the fault is elsewhere and finding 2
stands on its own.

This matters for the fix, not just for tidiness: if finding 2 is repaired by
COMPUTING the rects rather than reading published ones - the round 43 cure - then
the collapse path stops mattering for the sweep whichever explanation is true, and
chasing it first would be wasted work.

### How to work these

The rig is the right tool and it already exists: `FoolishHarness` plus the unified
log (`subsystem == "cards.foolish.anim"`), with the oracles `staleAtRest`,
`backwardsPaints`, `vanishedAtRest`, `strandedAtRest`, `sweepVisibleNow`,
`veilStandingNow`.
Two of those oracles have previously carried baselines that hid defects, so check
what they assert before trusting a green run.
