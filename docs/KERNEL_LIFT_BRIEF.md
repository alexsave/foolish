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

## The determinism pass - DONE

Landed 2026-09-05, in two halves.
The first half (the seven e2e suites, `e2e/helpers/rng.ts`, the first
`scripts/check_determinism.mjs`) went in with the lift.
The second half is this one, and it is what closes the section.

The invariant, in the owner's words: **the only true nondeterministic randomness
should be when we seed a live game.**

That is now true, and enforced.
`sdk/ts/wasm/engine.ts`'s `crypto.getRandomValues` in `injectDealSeed` is the one
draw on the game path - 32 bytes, once, saved to `games.game_seed`, with mid-game
engine randomness and bot decisions both reseeded from it.
The bot seed folds in the never-client-visible deal seed on purpose, so a Monte
Carlo bot's rollout stream is reproducible only to the server that holds it.

### What the second half changed

**Three ids stopped being drawn.**
A session log row got `crypto.randomUUID()` when it was appended
(`appendLogs`, `sdk/ts/wasm/engine.ts`), another when the packed wire was decoded
back into rows (`decodeLogs`, `sdk/ts/wire/logwire.ts`), and a third in
`addLog` (`server/api/common/common_utils.ts`).
They are `derivedUuid(namespace, seq)` now - `sdk/ts/wire/detid.ts`, a
UUID-shaped string that is a pure function of its inputs.
Nothing ever read those ids for a decision, which is why they could be derived;
what the entropy cost was the ability to compare one run of a game against
another, and `decodeLogs` not being a pure function of the bytes it decodes.

**The engine's clock became pinnable.**
`__setEngineClock` sits beside `__setDealSeedOverride` and covers the three
`Date.now()` reads that reach game state (a log row's `created_at`, and the two
`good_timestamp` stamps).
The clock still runs live in production - those are real timestamps the replay
extras read per-move timing off - but a test can now say which one.

**The e2e harness stopped drawing player and game ids.**
`e2e/harness.ts`'s `uuid()` was `crypto.randomUUID()`, so every suite that seeds
a game ran a different experiment each run, and a red
"game m4a3f2, player 9c1e... rejected" named nothing anyone could re-run.
It derives from the suite seed and the test file now.
The file is in the namespace because `node --test` gives each file its own
process: without it, two files hand the shared Postgres the same ids.
This also un-broke `fuzz.test.ts`, which advertises a `FUZZ_SEED` and was
quietly mixing entropic player ids into the stream that seed was meant to pin.

**Four `qsort` comparators became total orders.**
`cmp_by_elo` (`c/src/main_elo.c`), `cmp_desc` (`c/src/main_showcase.c`) and
`cmp_bsz` (`c/tools/leafbook/build_book.c`) returned 0 on a tie, and `qsort` is
neither stable nor consistent between libcs.
The `build_book.c` one is the one that matters: that order is the order the CHD
displacement search walks its buckets, so it decides the exact bytes of the
committed `c/src/leafbook_data.h` - `make leafbook` on a Mac and on the Linux CI
box produced two different books from identical inputs.

**One test's verdict stopped depending on the wall clock.**
`ios/FoolishTests/PickupHoldTests.swift` asserted `env.sentAt != 0`, and
`clockNow()` is unix seconds `& 0xffff`, so 0 is a legal stamp for one second
out of every 65536 - a red run on correct code roughly every 18.2 hours.

**Four Swift order-from-a-Dictionary sites became ordered.**
`MessageGameStore`'s three `persist*` writers serialized a `[String: Row]` in
per-launch hash order, so the same store state produced different bytes on every
launch; its `latestChain` eviction picked which live game to delete by hash order
whenever two chains shared an `updatedAt`; `MessagesViewController`'s transition
waiters resumed in hash order despite being keyed by a sequence number; and
`FStrings.t` substituted placeholders in hash order.

### The gate

`scripts/check_determinism.mjs` runs in `validate.yml` before anything that needs
a database.
It is four rules now, each with its own scope:

- `Math.random`, `randomUUID`, `getRandomValues`/`randomBytes` - every file in
  `e2e/`, `sdk/` and `server/`.
- a clock read (`Date.now`, `new Date()`, `performance.now`) - e2e **test files**
  only. A server must read a clock, and none of that decides a card; what must
  not happen is a test whose verdict depends on the machine's clock. Benches and
  harnesses under `e2e/` are out for the same reason.

It matches calls rather than the string, so the comments in `engine.ts` and
`bots.ts` that record the removed per-move reseeds do not trip it, and the
`bot_parity` pattern (assigning `Math.random` a seeded LCG) is an assignment, not
a draw.
`src/` stays out (cosmetic textures, React keys, error ids) and `offlinefun/`
stays out (research arenas, where randomness is the point).

Allowlisting is per (rule, file) with an expected count and a reason.
Adding a draw to an allowed file still fails, and an entry whose calls have gone
away also fails, so the list cannot rot into a blanket permission.
`e2e/determinism_gate.test.ts` runs the scanner over fixtures that break each
rule and over the shapes it must not flag, so the gate has been watched going
red.

### What is deliberately still entropic

- **The deal seed** (`sdk/ts/wasm/engine.ts`). The one draw. Everything protects it.
- **`createId()`** (`server/api/common/common_utils.ts`) - the game id is also the
  code a player shares to join, so it must be unguessable. It is
  `randomUUID().slice(0, 6)`, i.e. 24 bits, which collides at a few thousand live
  games: **widen it, do not derive it.**
- **`gen_random_uuid()` for `bot_lease_token`** (`seed.sql`) - a lease holder
  token.
- **JWT expiry against the real clock** (`auth.ts`) - that is what expiry means.
- **The iMessage create/rematch seed and game id**
  (`ios/FoolishKit/Messages/MessagesRootView.swift`) - the file's own comment
  records why: a guessable or rerollable seed let the creator reroll by tapping
  New game until the deal was good.
- **Two correlation tokens on the live server** (`utils.ts`) - a broadcast
  envelope sequence id and a request-id log prefix, neither compared nor ordered
  by.
- **`meta_actions.ts`'s lobby bot pick** - TEMPORARY, and allowlisted by name.
  See "Queued: the iOS lobby needs the bot picker".
- **`UUID()` for per-test UserDefaults suite names** (11 iOS test files) - the
  UUID never enters an assertion; a fixed name would make those tests
  order-dependent, which is strictly worse.

### Two things found and NOT fixed

`server/impls/native/foolish_server.c` seeds its deal with
`rand()` after `srand(time(NULL) ^ getpid())` - about 32 bits of predictable
entropy against the TS path's 256 crypto bits.
It is the local dev/reference server, so it is not a live hole today; if it ever
faces users it is a card-prediction one.

`ios/FoolishApp/AppCoordinator.swift`'s `makeSeed()` splats a millisecond
timestamp across 32 bytes for the OFFLINE vs-bot deal.
Offline play has no adversary, so it is fine where it is - but it looks exactly
like the iMessage seed and is not one.

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

## Queued: the iMessage chain layer - BOTH ITEMS DONE

Third in the queue, after the determinism pass and the transport mode.
Owner's instinct was that all of `ios/FoolishKit/Messages` (2,388 lines) should be
C.
Measured, it splits three ways, and only two of them are worth doing.

Both landed 2026-09-05: the gates (Item 1) and the turn controller (Item 2).
What is left in that directory is the third way - the Messages framework, the
App Group storage and the captions - which the next section says should not move
and still should not.

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

### Item 2: the turn controller as a transition function - DONE

Landed 2026-09-05, in the shape specified below.
`msg_wire.c` grew a `msg_turn_*` section - ten rules over an eight-bit chain
state - and `MessageTurnController` now asks them instead of stating them.
Nothing about the awaits moved, because nothing about the awaits could.

What crossed, and what each one is deliberately not the obvious thing:

- **What may be staged.**
  `msg_turn_can_send` / `can_act` / `can_stage`.
  `can_act` counts the HUMAN menu (`play_human_menu`), which is the one real
  behaviour change in the lift: `iCanAct` counted the raw menu, and the raw menu
  always offers `good` because a bot needs to say good over an uncovered attack
  to leave the eligible set.
  A seat whose only offer is a good the board will not let it make was reading
  as a seat with a move, and the board drew an action bar with no live button in
  it.
  The controller now publishes `humanLegal` beside `legal`, narrowed off the
  same bytes in the same assignment, so the two cannot describe different menus.
- **The admission door.**
  `msg_turn_admit`, one verdict for the three refusals `apply` makes.
  The ORDER between them is the rule: the retraction is asked first because it
  is the silent one.
- **What an arrival does to a staged move.**
  `msg_turn_arrival`, four outcomes, each of them a bug this extension has had:
  a duplicate delivery red-retracting a move nobody superseded, a burst's second
  arrival adopting underneath the flight meant to precede it, a retraction
  offering to take back a bubble the thread already has.
  Plus `msg_turn_adopt_duplicate`, the deliberately narrower guard the adopt
  path keeps for its direct callers.
- **What a send means.**
  `msg_turn_sent_source` (the picker - three facts in, one answer out) and
  `msg_turn_send_verdict` (what follows from it).
  The verdict is asked once, and again after the decode when the first answer is
  `DECODE`, which keeps the host's one await where it belongs.
  `docs/ANIMATION_CORE_C.md` had left `sentBytes` as "liftable and deliberately
  left" with the note that the thing to lift was the PICKER and not the bytes;
  that is what happened, and the two `Data` blobs still never cross.
- **What is withheld, and what a read publishes.**
  `msg_turn_hold_state` (the step whose board a held settlement shows, and why a
  `good`'s cut of 0 answers 0 rather than an error) and `msg_turn_publish` (the
  held view, the EMPTY menu, the animation boundary, the veil).

That last one closes half of a finding recorded further down this file.
"An untested load-bearing rule: the empty menu under a held settlement" says
stage 2 mutated away both halves of the rule and the whole suite stayed green.
Both halves are now `msg_turn_publish` outs, and mutating either one to a
constant fails `msg_wire_test` - `publish never publishes the empty menu` and
`publish never shows the held view`, one failure each, against a baseline of
zero.
What is still missing is the FIXTURE the finding actually asks for - a
defender's cover that empties their own hand, driven through a real controller -
so the finding stays open; what has changed is that the rule is no longer
un-guarded anywhere.

Everything crosses as ints.
The chain state is eight booleans and every answer is a small enum, so a packed
record around either would be the ceremony the "No JSON" section warns about,
and `ios_api.c` carries `_Static_assert`s that the bridge's names and the
kernel's values have not drifted.
`TurnWire.swift` is the crossing, beside the other wire decoders; no app-layer
file imports `CFoolish`.

Mutation-checked: fifteen mutations to `msg_wire.c`, each applied on its own
against a zero-failure baseline and listed with its failure count in
`c/tests/msg_wire_test.c` above `test_turn_controller`, and six more to the
Swift crossing listed in `ios/FoolishTests/TurnWireTests.swift`.

The original assessment follows.

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

### An untested load-bearing rule: the empty menu under a held settlement - TESTED

Fixed 2026-09-06, and the report below was right on every count.
Mutating `legalPacked = heldSettlement.isEmpty ? read.legalPacked : emptyMenu`
down to `legalPacked = read.legalPacked` left the whole `MessageStagedDealTests`
class green, its own `XCTAssertTrue(c.legal.isEmpty)` included.

The missing fixture is written:
`MessageStagedDealTests.testAHeldSettlementEmptiesAMenuTheKernelWouldHaveOffered`.
It hunts for the position instead of asserting into the dark - two greedy
policies (attackers throw, the defender covers, and a chain long enough to empty
a hand in one turn), 3 to 5 players, 120 deals each - and asserts only once the
KERNEL's own menu for that seat is NON-empty, which is the thing that makes the
assertion mean anything.
A search that finds nothing fails.

Both halves are mutation-checked, and neither catches the other's mutation:
- the menu half fails with "a held settlement published 1 legal moves while the
  kernel offered 1";
- the view half (`view = heldView ?? v`) fails with "1 dealt card(s) reached the
  board".
It finds its position in about a second, so it costs the suite nothing.

Why the shape is as rare as the report says: after a pickup or a good the next
first attacker is never the seat that moved, so the kernel's menu for it is empty
and the assertion agrees with the rule about nothing.
Only `handle_cover`'s `hand_count == 0` branch sets
`first_attacker = defender` - it discards, refills that defender a fresh hand,
and hands them the opening attack of the next bout.
Across 3 to 6 players and 120 deals a play policy that is not aiming for it
reaches that position roughly twice.

The original report follows.

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

Half of it closed with Item 2 (2026-09-05).
Both halves are `msg_turn_publish` outs now, and mutating either to a constant
costs 2 failures each in `msg_wire_test` - so the RULE is guarded where it is
stated.
The FIXTURE was the remaining work, and it is written now (see the top of this
section): it drives a defender's own hand empty through a real controller and
watches what the board is allowed to offer afterwards.
The two guards are not redundant.
`msg_wire_test` pins the kernel's answer where the rule is stated; the fixture
pins that a board built on that answer never offers the move, which is the half
that stayed green through every mutation before it existed.

### A CI flake: edge-serve 502s under the real memory budget - DIAGNOSED

The `memory` workflow's `edge-serve` job failed on main with
`semtex,octogen returned 502` at commit `fb9294e`, then passed on re-run with a
BYTE-IDENTICAL tree (`798b003b` both times), so it is a flake and not a
regression.
The original report read it as two Monte Carlo bots timing out under the edge
runtime's real memory budget.
**It is not that, and it cannot be.**

Measured against a real local stack on edge-runtime v1.74.3, which is the version
the failing job ran:

- The edge runtime's own main service answers **546 `WORKER_LIMIT`** when a
  worker is killed for a resource limit (reproduced by asking `memtest` for 20
  full games in one request, which blows the 2000ms CPU hard limit), **503** for
  a worker that failed to boot and **500** for one that threw.
  Its source enumerates exactly those codes.
  It cannot emit a 502.
- A **502** with the body `An invalid response was received from the upstream
  server` is Kong saying it had no upstream at all.
  `docker stop` on the edge-runtime container reproduces it byte for byte.

So the failing run was the edge runtime not answering, not a bot under pressure.
The `memtest` requests take 111-166ms in CI against a 2000ms CPU hard limit and a
256MB worker; there is roughly a 12x margin and no measured pressure.

What makes the container stop answering: `functions serve` OWNS its lifetime, and
when that CLI process is terminated it gracefully removes the container.
SIGTERM to `functions serve` -> container removed -> the very next request through
Kong is exactly the observed 502.
The workflow used to start `functions serve` in one step and make the requests in
the NEXT one, so the assertions ran against a container owned by a background
process left over from a step that had already finished.
The failing run's timeline fits that to the millisecond: the readiness probe
answered at the step boundary, the main worker logged `serving the request` 19ms
into the next step, and then nothing more was ever heard from it.

Fixed by serving and asserting in ONE step, so the process is a live child of the
shell doing the asserting.
Not proven is what terminated the process on that one run; what the workflow
threw away was the evidence, because it catted `serve.log` about 10ms after curl
returned and the CLI's log stream lags the HTTP response.
So a failure now settles first, then dumps `serve.log`, `docker ps -a`, the
container's own `docker logs`, and whether the serve process is still alive, and
it names the fault by what the status code MEANS rather than printing a bare
number.
Both new failure branches are mutation-checked against a real local stack.

Deliberately NOT added: a retry.

Related, and fixed in the same pass: both workflows asked `supabase/setup-cli` for
`version: latest`, which makes an unauthenticated GitHub API call on every run.
That call rate-limited on main (`Failed to resolve latest Supabase CLI release:
rate limit exceeded`) and it silently changes the tooling underneath us.
Both are pinned to `2.116.0`, which is what `latest` resolved to at the time, so
the pin is behaviourally a no-op; a fixed version skips the API call entirely and
downloads straight from the release URL.

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

## Delete the dead C build flags - DONE

Owner: "All of it. It's in git history if we need to tune it again."

Done.
Fourteen flags and the code they guarded are gone, along with the two research
harnesses that existed only to drive them.
`c/src/cordite_sim.c` lost 564 lines - it was the largest C file in the repo, and
about a quarter of it was unreachable.

Every dead flag was in BOT RESEARCH code.
Nothing in the game core, the wire, or the animation layer was guarded by a flag
no build sets.

### Deleted: read by the code, set by no build target

| flag | file |
|---|---|
| `CD_TT_TRACE` | `cordite_sim.c` |
| `CD_TT_RANKSYM` | `cordite_sim.c` |
| `CD_TT_STATS` | `cordite_sim.c`, `cordite_sim.h`, `main_eval.c` |
| `CD_TT_BOUNDS` | `cordite_sim.c` |
| `CD_TT_SUITSYM` | `cordite_sim.c` |
| `CD_TT_BOUNDS_USE` | `cordite_sim.c` |
| `CD_TT_TAILCACHE` | `cordite_sim.c` |
| `GRPO_RNG_DEBUG` | `game.c` |
| `CD_TT_ADAPT` | `cordite_sim.c` |
| `OG_HIDE_UNCOVERABLE` | `octogen_strategy.c` |
| `CD_TT_DEPTH_PREF`, `CD_TT_ORDER2`, `CD_TT_ORDER3` | `cordite_sim.c` |
| `CD_SIM_SOLVE_MAX_DEPTH` | `tools/endgame_retro/alphabeta_probe.c` |

The flag list was re-derived from the tree rather than taken from this table's
earlier draft, and each one was checked against every build path that exists -
the global `CFLAGS`, `IOS_CAPS`, `l1_measure`, `leafbook`, `og_explain`,
`WASM_FLAGS`, `WASM_RULES_FLAGS`, `WASM_GUARDS_FLAGS`, `WASM_BOT_CFLAGS`,
`WASM_ORACLE_CFLAGS`, `WASM_ORACLE_MT_CFLAGS`, `server/impls/native/Makefile`,
and the five CI workflows.
Nothing defined any of them.
The census was re-run after the Mode B oracle and the native-server work landed,
since both add build paths: the new `wasm-oracle-mt` target and the new
`FOOLISH_ORACLE_MT`, `FOOLISH_QUIC` and `FOOLISH_NO_OPENSSL` flags set none of
the deleted ones, and each of the three is set by exactly one target.
The only `-D` uses left anywhere were in prose: research docs and two tool
scripts, neither of which is a build target.

`CD_TT_TAIL_K`, `CD_TT_TAIL_N` and `CD_TT_BOUND_MINCARDS` went with their
parents.
They are `#ifndef X / #define X <default>` idioms that guard zero lines, so they
would have been wrong to delete on their own - but they lived INSIDE the
`CD_TT_TAILCACHE` and `CD_TT_BOUNDS` blocks and had no meaning without them.
`GUNPOWDER_MODE`, `LEAFBOOK_K`, `REPLAY_BN_CAP` and `CNITRO_WASM_SIZE_T` are the
same idiom and were left alone.

### The research harnesses went with the flags

`c/tools/hide_tax/` was deleted outright: `hide_eval.c` links against
`og_hide_fire_count`, a symbol that only existed under `OG_HIDE_UNCOVERABLE`, so
it could not compile after the deletion.

`c/tools/tt_divergence_viz/generate.sh` lost its `measure` half, which built a
`-DCD_TT_STATS` evaluator and read `main_eval`'s `CD_GW` emitter.
The `rebuild` half stays and still renders `docs/tt-divergence.html` from the
banked per-game working sets, which are checked in.
Leaving the measure path in place would have left a script that compiles
cleanly, silently measures nothing, and produces an empty sweep.

`docs/OCTOGEN_HIDE_UNCOVERABLE.md`, `docs/SOLVER_TT_WORKING_SET_PLAN.md` and
`docs/C5_BOUNDS_HANDOFF.md` now say at the top that the code they hand off is
gone and name git as the recovery path.
The measurements in them stand; the recipes do not.

### The other direction: are any flags always on, so the guard is pointless?

Re-checked against the current tree, and the answer is still **no**.

`CD_LEAFBOOK` sits in the global `CFLAGS` (`Makefile:31`), so every ordinary
native target has it and the guard looks redundant.
It is not.
The `leafbook` target builds with its own `LEAFBOOK_CFLAGS`, which deliberately
omits it - the Makefile says so in as many words: "The build_book pass compiles
WITHOUT -DCD_LEAFBOOK (it builds the book by direct solves)".
`l1_measure` and the rules/guards wasm modules also build without it.
The guard IS the bootstrap.
Remove it and the book becomes unbuildable, because building the book would
require the book.

Everything else - `CD_WASM_OVERLAY`, `CD_RULES_OVERLAY`, `CD_TT_2WAY`,
`CD_TT_PACK8`, `DEAL_RNG_DISABLED`, `FOOLISH_ORACLE_BUILD`,
`FOOLISH_SEEDED_BOTS_ONLY`, `GUARDS_VALIDATE_ONLY`, `REPLAY_STATS`,
`LEGAL_STATS`, `OG_EXPLAIN_BUILD` - is set by one or two named targets and is
genuinely conditional.
The near-miss worth writing down is `GUARDS_VALIDATE_ONLY`: only `guards.wasm`
sets it, and it looks file-local, but the code it guards is in `game.c`, which
every target compiles.

`ACCELERATE_NEW_LAPACK` is in the global `CFLAGS` and read by nothing in this
repo: it is Apple's own Accelerate macro, consumed by a system header.
Not ours, left alone.

### How the deletion was verified

Not by assertion.
`unifdef` did the removals, so no `#ifdef` body was edited by hand, and then the
binaries were diffed.

`rules.wasm` and `guards.wasm` are **byte-identical** to a baseline built from
`origin/main`, and so is `ios/Fixtures/goldens.json`.

`bots.wasm`, `bots-explain.wasm`, `oracle.wasm` and `oracle-mt.wasm` are 374-381
bytes smaller.
A bisect pinned the whole delta on two constructs that only the deleted flags
ever made non-constant: `SimSolver.order` with its move-ordering branch (only
`CD_TT_ADAPT` / `CD_TT_ORDER2` / `CD_TT_ORDER3` ever set it to anything but 0)
and the `store` / `tbl` / `tmask` locals (only `CD_TT_DEPTH_PREF` and
`CD_TT_TAILCACHE` ever varied them).
With those two put back and nothing else, the module is byte-identical - so
every actual flag deletion is provably codegen-neutral, and what changed is the
runtime-dead branch the non-LTO wasm build could not prove away.

All of this was measured again on each of the five bases main moved through while
the branch was open: the native-server merge that moved `cordite_sim.c`'s mask
tables to `_Thread_local`, the Mode B oracle, the determinism pass, the chain
layer's turn controller, and the post-game analyser.
Each time the earlier numbers had been taken against a tree that no longer
existed, and a measurement that IS the evidence for a change has to be true of the
tree that actually merges - re-running it is the work, not a formality.
The signatures come back identical value for value every time, so each of those
changes is itself play-neutral and the deletion stays play-neutral on top of all
of them.

Play identity was then measured directly.
`GAME_SIG` signatures over 380 games at 2-8 players, native and again under the
wasm bot module's exact flag set (`CD_TT_BITS=12 -DCD_TT_2WAY -DCD_TT_PACK8
-DCD_LEAFBOOK`), are identical bot for bot: octogen, cordite, semtex, robusta,
blackpowder, firecracker.

`make tests` 6056 passed / 0 failed - the same count the pristine baseline
reports, measured rather than assumed, and up from 6017 because the post-game
analyser added 39 cases while this branch was open - `make difftests` green with
`solver_difftest` at 0 mismatches for 2, 3 and 4 players, `make leafbook-verify`
1,000,000 samples 0 mismatches, `make leafbook-gate`, `make l1-measure`,
`make og_explain`, `make ios-lib` / `ios-smoke` / `ios-goldens`,
`ios/scripts/mac_tests.sh` (623 + 25 XCTest cases, 0 failures),
`ios/scripts/lint_architecture.sh` and `npm run check:determinism` all pass.
The native server is Linux-only since it grew epoll, so on this Mac the check is
its kernel-linking target, `make sem_fuzz`, which builds.

`game.c`'s `<stdio.h>` and `<stdlib.h>` includes went too - `GRPO_RNG_DEBUG` was
the only thing in that file that ever called into either, which is exactly what
the wasm libc shims say in their own header comments.
`rules.wasm` and `guards.wasm` stayed byte-identical across that removal, so it
is provably a no-op.

### Two breaks the parallel merges left on main

Neither is this change's doing and both are fixed here, in their own commits, so
they are easy to lift out.

`make wasm-oracle-mt` did not compile. The native-server merge made
`engine_snap_hook` `_Thread_local` in `game.h`; the Mode B merge added
`wasm/wasm_oracle_mt.c`, which re-declared it without the qualifier. No CI job
builds wasm, so the collision landed green.

`npm run check:determinism` failed. The determinism pass taught the gate to catch
clock reads that decide a test verdict; the Mode B suite has two, and it landed
first. The gate is right: that loop's tolerance is derived from the counts the run
reached, so a slow runner would gather less evidence and get a WIDER tolerance -
the assertion weakening exactly when the machine is least able to earn it. The cap
never fired anyway (measured: the heavy caller reaches its full target in ~5s
against a 25s cap), so it is deleted rather than allowlisted.

That one was fixed twice. A separate PR reached main first and settled it by
allowlisting the two reads as a budget rather than a verdict, which silences the
gate without closing the widening-tolerance hole; this branch replaces that entry
with the deletion. Removing the reads then forces removing the entry, because the
gate checks in both directions and fails on an allowlisted call site that no
longer exists. An allowlist that cannot go stale is worth more than one that only
ever grows.

Three times in one night, two PRs merged cleanly in text and broke in meaning -
this branch included, which is why its evidence was re-taken rather than carried
forward. The pattern is worth naming: nothing in CI built wasm, and nothing
re-runs a merged PR's gate against a tree the other PR changed.
The first half now has a CI job. The second half is still open.

A third, smaller instance of the same shape, recorded because it will bite again:
`ios/scripts/mac_tests.sh` regenerates the Xcode project only when
`ios/project.yml` is newer than the generated `project.pbxproj`. The project
globs `../sdk/swift` as a DIRECTORY, so a PR that adds a Swift file there without
touching `project.yml` leaves every existing checkout building a project that
does not contain it - a `cannot find type` error in a file nobody edited. The
turn controller did exactly that. `--regen` clears it; a staleness check that
looked at the globbed directories rather than only at the spec would not need to
be remembered.

One pre-existing warning went with the sweep: `sim_other_in` in `cordite_sim.c`
had no callers before this change either.
Two more (`robusta_choose_multi`, `robusta_choose_7p_real`) are untouched - they
are somebody's live research, not this campaign's business.

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

## Fixed: a replay URL failed by naming the wrong fault

Fixed 2026-09-06, in the shape the report suggested.
`urlToCode` now takes the code out of the link forms people actually paste:
scheme or no scheme, `www.` or not, a trailing slash, a query, a fragment, the
printed `WWW.FOOLISH.CARDS/` form, a bare code, and any of those wrapped in
whitespace.
Some other host falls back to the last path segment.
When what is left is not base32, `urlToGame` refuses it as
**"not a replay code"** instead of handing `base32Decode` - which ignores stray
characters - a string that decodes to a DIFFERENT game and dies later under a
codec error that was never the fault.

Both halves are covered, and both were mutation-checked against the unfixed
codec: `e2e/replay_codec_edges.test.ts` walks eleven pasted forms and six
non-codes (2 of 8 failed on the old code, naming the exact defect), and the
property test in `e2e/replay_codec.test.ts` re-reads every game it encodes
through three pasted forms of that game's own link, so no replay code is frozen
as a fixture anywhere.

Note what is deliberately NOT refused: a well-formed base32 string that is not a
real code (`helloworld`) still reaches the decoder, because nothing about it says
otherwise and a length heuristic would be a guess.
The decoder's error is then honestly about the bytes it was given.

The original report follows.

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
