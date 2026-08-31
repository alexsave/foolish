# The animation catalogue

Every animation SHAPE the iMessage board can play, what it should look like, and whether anything actually checks it.
Card combinations are not enumerated - "a cover" covers every pair of cards; "a multi-card cover" is a different shape.

Written at 1.0(24).
Status values are `unit` (a named XCTest), `rig` (a FoolishHarness scenario that has actually been run), `both`, `spec` (defined here, not built), or `untested`.
Keep the status column honest: a shape nothing exercises must say so, because the point of this file is to be the list of what we are NOT sure about.

**1.0(28): every shape below was walked with the owner and given a DECIDED line.**
A shape with no `Decided` line was not asked about.
`Decided (keep)` means the owner looked at what the code does today and confirmed it, so the behaviour is now load-bearing and a change to it is a regression rather than a refactor.
`Decided (change)` means today's behaviour is wrong and the shape is now `spec` regardless of what its tests say - a green test against the old shape is a test that needs rewriting.

The five changes from that pass, in one place.
All five are built (1.0(29)); the conflict model is being taken separately.

- **The game-over hold** is one second, not 500ms. BUILT.
- **Undoing a pickup** flies the cards back out of my hand onto the table, where it used to snap. BUILT.
- **Goods cleared by a throw-in** turn back into swords WITH the throw-in card, not after it. BUILT.
- **The out badge** collapses, in parallel with the card motion of the move that ends the player. BUILT (rule and visual; nothing has watched it).
- **Pass / transfer** is a card and a shield leaving together, plus two swords turning in place. BUILT (1.0(29)).

And one correction to this document rather than to the code: receivers DO get the bout-end hold, and Channel D said they did not.

## The three machines

**The action/settlement cut.**
A turn's kernel event stream splits into an ACTION half (`attackPass`, `cover`, `pickup`, `out`) and a SETTLEMENT half (`magicTransition`, `discard`, `cardsToTrash`, `refill`).
The rule is the kernel's (`evw_is_settlement`), not the board's, and is pinned by `MessageStagedDealTests.testTheSettlementBoundaryIsTheKernels`.

**The three channels.**
`stagedAnimation` plays the action half the moment you play a move.
`releasedSettlement` plays the settlement half, and only when Send lands.
`openReplayEvents` plays BOTH halves when a bubble is opened cold or arrives on an open board.

**Why recipients never split.**
A recipient's bubble carries the whole turn and they were never in a position to take it back, so they animate all of it at once.
The hold exists only because the player who staged a move can still undo it, and must not be shown a deal they could still retract.

**The default composition, decided 1.0(28).**
Within one kernel move, everything that moves goes at once; between moves, one after another.
That is what "1 keep / 2 keep / 3 keep / 6 keep / 8 keep" all say, and the two shapes that are not one kernel move - a pass, and a good being cleared - were both decided PARALLEL as well, so there is now exactly one exception in the whole document: the refills after a bout end, which stay serial.

---

## Channel A - my own move, staged, before Send

**Single-card attack.**
The card flies from its own hand slot to its table slot, pre-hidden at the source so it flies rather than pops; the hand re-fans; deck and discard badges do not move; no role changes.
Staging then auto-collapses the drawer to Messages' Send.
Decided (keep): the collapse waits for the flight to LAND.
You always watch your own move finish on the big board; the drawer coming down a beat late is the price and it is the right one.
`BoardAnimator.sequenceDepth` is what holds the collapse off, so any new sequence that forgets to raise it will yank the drawer down mid-flight.
Status: both.

**Multi-card attack (several thrown in at once).**
All cards fly together as one parallel group, each from its own hand slot to its own table slot - never staggered.
Decided (keep).
Status: unit (`MessageParallelCoverTests`, `MessageMulticoverFlightTests.testARealKernelMulticoverAlsoSpreads`).

**Throw-in onto an already-open bout.**
As above, but the new attack lands beside existing battles and the grid re-lays out; existing covers stay tilted while the newcomer arrives upright.
Decided (keep): the grid shift and the flight run AT THE SAME TIME.
The card lands into a slot that is still opening, which is one movement rather than two, and the alternative (open the gap, settle, then fly) shows a hole on the table before anything justifies it.
Status: unit (`CoverTiltTests.testACoverFromAnEarlierBubbleIsTiltedOnTheFirstPaint`, `testAnUncoveredAttackNeverTilts`).

**Pass / transfer (classic rules only).**
Decided (change), and this shape had never been specified at all.
Four things happen and they happen TOGETHER, in one beat:
the card flies to the table;
the shield flies from me to the next defender;
MY badge turns a sword in, because passing makes me an attacker;
the next defender's badge turns their sword out, because they are not one any more.
The shield always FLIES - it went somewhere, so it travels - while both swords are gestures made in place, which is the existing rule in `roleFlights` and needs no new machinery.
Built at 1.0(29), and the surprise was how little of it was new: `FRoleCoin` already turns the passer's sword in behind a departing shield and the receiver's out as one arrives, so all three channels needed was for the hand-off to happen at the right MOMENT.
A staged pass (this channel) was already right - a transfer does not clear the table, so it is an ordinary placement and the `!sequenced` branch of the board's `onChange` syncs the roles in the same tick `flyPlacement` flies the card.
The two channels that replay a kernel stream were not: they handed the shield over at the CLOSING beat, where a bout end's hand-off belongs, so a transfer read as two movements.
`passHandOff` is the rule that moved it, fired beside the group's own flight exactly as `goodsCleared` is.
Its one subtlety is that the kernel snapshots a pass BEFORE the hand-over and emits no defender-change step, so the new defender can only be read off the bubble's final board.
Status: both.
Unit (`Round28ShapeTests` x5, including a real-kernel transfer that pins the stale-snapshot trap; the rules themselves in `PodkidnoyTests.testADefenderIsNeverOfferedATransfer`, the flight generically in `RoleMotionTests.testTheShieldFliesToTheNextDefender`).
Rig: `pass` is now a kind (`HARNESS_ARRIVE_KIND=pass`, cold and live) and a scenario (`HARNESS_SCENARIO=pass` with `HARNESS_AUTOMOVE_KIND=pass`, which is Channel A), and all three were watched frame by frame under `HARNESS_SLOWMO=10`.

**Single cover.**
The card flies from its hand slot to the battle it covers, arriving ALREADY ROTATING into its tilt rather than snapping to it.
The target is the highest attack the card can beat, not the leftmost.
Decided (keep).
Status: unit, heavily (`CoverTargetTests` x12, `MessageMulticoverFlightTests.testACoverFliesInAlreadyRotating`, `CoverTiltTests.testAReplayedCoverIsUprightUntilItLands`).

**Multi-card cover (one card per attack, one move).**
Each card flies to the attack it specifically covers; the pairing is the kernel's, and a mismatched pairing is refused wholesale rather than guessed at.
Decided (keep): parallel, no stagger.
Status: unit (`MessageMulticoverFlightTests` x9, `MessageParallelCoverTests`).

**Cover that closes the bout (the defender's last cards).**
The first shape where the cut bites: the cover lands and STOPS there, table still on screen, discard sweep and refills withheld until Send.
Decided (keep).
Status: unit (`MessageBoutEndHoldTests.testACoverThatClosesTheBoutHolds`, `testAMultiCardCoverHoldsOnceAfterTheWholeGroup`, `testARealClosingCoverHolds`) and rig as an ARRIVAL (`coverend`), but never through a real Send in the rig.

**Pickup.**
The whole table lifts and flies into my hand; the sweep grid holds the cards where they sat so nothing fades out from under them.
The attacker's refill is withheld.
Decided (keep): every card at once, no stagger and no attacks-then-covers ordering.
A pickup is one gesture - you take the table - and splitting it would make the board argue about a distinction nobody is thinking about at that moment.
Status: both (unit `MessageEventsTests.testOpenReplayReconstructsThePreBoutTableForAPickup`; rig stage+send).

**Pickup with throw-ins.**
As above, but every card including late throw-ins flies to my hand - the sweep must carry cards the pre-move view never held.
Decided (keep): identical to a plain pickup, and explicitly NOT staggered just because there are more cards.
Status: unit (`MessageBoutEndHoldTests.testTheSweptTableCarriesTheCoverThePriorViewNeverHeld`, `testTheSweptTableIsOnlySwappedForOneThatKeepsEveryCard`).

**Plain good (does not close the bout).**
Emits NO step at all - the stream is empty.
The only visible change is my sword rotating into a green check, in place, no flight.
Decided (keep): the rotation is the whole animation and needs no pulse or scale beat to sell it.
Status: unit (`RoleMotionTests.testSayingGoodTravelsNowhere`, `MessageStagedDealTests.testAStagedGoodShowsTheGoodMarkAndKeepsTheTable`, `MessageBoutEndHoldTests.testAGoodBubblesDiscardDoesNotHold`).

**Bout-ending good, before Send.**
The sword-to-check rotation ONLY; the discard sweep, both refills and the role hand-off are all withheld.
Owner, 1.0(23): "it correctly only showed me the good checkmark role transition before I sent it."
Status: unit (`MessageStagedDealTests.testAStagedTurnNeverShowsACardOffTheDeck`, `testAStagedGoodShowsTheGoodMarkAndKeepsTheTable`).

**A move that puts a player out.**
`out` is NOT a settlement step, so a cover that empties a hand does not hold - it plays straight through.
The seat's BADGE rotates out: it squishes horizontally to zero width and STAYS there, never expanding back.
Decided (change): the collapse runs IN PARALLEL with the card motion of the move that ends them, not as a beat of its own.
The owner's reasoning is that going out always means a player's last cards are travelling - to the table, or into somebody's hand - so the badge and those cards are one event and should read as one.
Status: the cut rule is unit-tested (`MessageStagedDealTests`: `[cover, out].settlementStart == nil`); the badge behaviour is **spec** - see "The out badge" below.

**Undo.**
The staged card returns to its hand slot, the held settlement is DROPPED rather than released, and re-staging replaces the input bubble.
Undo-to-empty re-seals the base and says on the wire that it carries nothing.
Decided (keep) for a card play: the return flight is the exact reverse of the play, which is what it already does.
Status: unit (`MessageUndoBubbleTests` x5, `MessageStagedDealTests.testUndoDropsTheHold`, `SendWindowTests.testUndoNeverPassesThroughAnEmptyBoard`).

**Undo of a PICKUP.**
Decided (change).
The cards must fly back OUT of my hand and onto the table they came from.
Today `flyUndoReturn` handles only the undo that returns cards TO my hand and every other undo falls through to a snap, so undoing a pickup is the one retraction with no motion at all - the biggest board change in the game happening between two frames.
The reverse of a pickup is a pickup played backwards, exactly as the reverse of a play is `flyUndoReturn`.
Status: unit (`Round28ShapeTests.testEachCardGoesBackToItsOwnBattle`, `testACardThatIsNotOnTheTableIsDropped`), built as `flyUndoRelease`.
The PAIRING is tested; the flight itself has never been watched, in the rig or on a device.

---

## Channel B - the same move, at Send

**Send of a move that did not end the bout (attack, cover, throw-in, plain good).**
Nothing animates.
The Undo pill goes on the TAP itself, not after the rebase, and the board rebases onto the sent bytes with the picture unchanged.
Decided (keep), and the owner's reason is the rule the whole cut exists for: Channel A already animated this move, so a second showing would be the board playing it twice.
Status: both (unit `SendRebaseTests.testTheUndoPillGoesOnTheSendSignalNotOnTheRebase`, `MessageSendStaysOpenTests`; rig stage+send).

**Send of a bout-ending cover.**
The withheld half plays: table sweeps to the discard, each player refills in kernel order, then the role hand-off flight.
Decided (keep) on both counts.
The refills stay SERIAL, one seat at a time in kernel order - the single place in the whole catalogue where things that could go together deliberately do not, because a refill round is the one moment the deck is dealt out and watching it go round the table is the point.
And the settlement starts IMMEDIATELY on the tap, with no beat between "I sent it" and "here is what it did".
Status: unit (`MessageStagedDealTests.testSendReleasesTheHeldSettlement`, `MessageBoutEndHoldTests.testTheHoldIsAReadableBeatAndScalesWithTheFlights`).
**Not exercised through a real Send in the rig** - the auto-player never picks a closing cover.

**Send of a bout-ending good.**
The same released settlement, with the check already showing because staging showed it.
This is the 1.0(23) report.
Decided (keep): identical to the cover above, serial refills and an immediate start.
Status: both (rig: goodend stage+send runs clean).

**Send of the move that ends the game.**
Released settlement, then the result screen: final board, then the rank list.
Decided (change): the final board holds for ONE SECOND before the ranks come up.
`settleResults` waited 500ms, which is half a beat and reads as the board being taken away from you.
The ranks keep their current fade-in; only the wait changed.
Built as `gameOverHold`, and expressed against `flightTime` so a filmed game-over keeps its proportions.
Status: unit (`Round28ShapeTests.testTheGameOverHoldIsASecond`, `testTheGameOverHoldScalesWithTheFlights`).
Status: rig only as an ARRIVAL (`gameover`).
**Never tested as my own send.**

**Send handed the wrong bytes.**
Nothing animates and nothing changes - base, `pending` and `sending` all stand, because a refusal that dropped the staged move would walk the board back by exactly the move just played.
Decided (keep).
This is a defensive case rather than a designed one: it fires when the host hands the extension a payload that is not the chain we sealed, and the only correct picture is the one already on screen.
Status: unit (`SendRebaseTests.testASendCannotRebaseTheBoardOntoAnOlderChain`, `testARefusedSendLeavesTheStagedMoveOnTheBoard`, `SendWindowTests.testASendWhoseBytesCannotBeReadLeavesTheBoardAlone`).

---

## Channel C - reopening my OWN bubble (cold replay)

**Reopen my own attack or cover.**
The board opens on the state BEFORE my move and replays it.
Distinct from Channel A only in that the role marks must be re-seeded from `openReplayPriorState` rather than surviving in `@State`.
Decided (keep).
Status: both (unit `MessageEventsTests.testAChainsLastMoveIsReplayedToEitherSeat`; rig `ARRIVE_COLD` + `ARRIVE_SELF`).

**The speed of a replay.**
Decided (keep): a replay runs at exactly live speed.
Nobody is waiting on input during one, so slowing it down was worth asking about, but two speeds for the same footage is a second timing model to keep honest and the shapes are meant to be recognisable between channels.
Status: implicit in every rig replay.

**Reopen my own bout-ending good.**
Opens with my SWORD STILL UP, rotates it to the check, and only then plays discard, refills and the role hand-off.
The rotation exists only because the prior board is fetched: seeded from the stream's own first frame the check is already on and there is nothing left to animate.
Status: both (unit `RoleOpeningTests.testAGoodOpensFromABoardThatStillHasTheSwordUp` and `testATwoPlayerEndingGoodStillOpensWithTheSwordUp`; rig traced sword -> check -> hand-off).

**Reopen my own bubble immediately after sending it.**
Decided at 1.0(28), and this REVERSED the old rule.
Swiping the drawer down and tapping the bubble you just sent REPLAYS it.
The just-sent marker still exists and is still byte-exact, but it now lives only inside the activation that wrote it (`willBecomeActive` burns it), so it silences the reopen Messages forces on you and never the one you chose.
Status: unit (`StagedBubbleRouting` tests, `MessageTurnControllerTests.testANewActivationDropsTheJustSentMarkerUnspent`).
Still **not rig-tested**, and now carries a known cost: an EXPANDED send still calls `dismiss()`, so its immediate re-activation may replay my own move once (`quiet-drop` in the trail names it).

**Reopen a bubble that carried no move (undo-to-empty).**
Animates nothing, and does not restart the pickup hold.
Decided (keep).
Status: unit (`MessageUndoBubbleTests.testAStagedMoveThatWasUndoneAnimatesNothingForTheReceiver`, `testTheEmptyBubbleDoesNotRestartThePickupHold`).

---

## Channel D - opening SOMEBODY ELSE'S bubble cold

**Their single attack, cover, pickup or good.**
The whole turn plays, action half then settlement half.
Decided (keep), with a CORRECTION to what this section used to say.
It claimed receivers play through "with no hold".
They do not, and they should not: `holdsAfter` is inside `runEventStream`, which every channel shares, so a receiver watching a bout-ending cover gets the same beat between the cover and the sweep that the player who made it gets.
That is right - the beat exists so the closed table is readable, and a receiver has MORE need of that than the person who chose the card.
My hand is masked: their drawn cards are hidden, mine come back with real identities.
Status: unit (`MessageEventsTests.testLastMoveEventsRevealMyOwnRefillAndHideTheOpponents`) and rig for attack, goodend, pickup and gameover.
**Cold cover and cold coverend have never been run.**

**Their turn that folds several actions into one bubble.**
A double cover, or a cover following two pending goods: BOTH covers replay, not just the last.
Decided (keep): consecutive covers from one seat fly TOGETHER as a single parallel group, even though they were two separate decisions.
It is one bubble and it reads as one movement; the move boundary is not on the chain to split by anyway (see `parallelGroups`).
Status: unit (`MessageEventsTests.testAStagedDoubleCoverReplaysBothCovers`, `testTwoCoversStagedTogetherStillReplayBoth`, `testTwoCoversSentSeparatelyReplayOnlyTheSecond`).

**Their game-ending move.**
The final move animates, and THEN the ranks - never straight to the rank board.
Decided (keep), with the same one-second hold as my own send.
Status: rig (`gameover`, cold and live).

**A finished game opened by a spectator.**
As above from viewer -1: no hand, no "(You)" row, final move then ranks.
Decided (keep).
Status: rig (`spectator`), plus a visual check in round 21.

---

## Channel E - a move arriving on a board that is already open

**Any single arrival.**
The chain folds into the LIVE controller - the board keeps its identity, its measured geometry and its animator - and the move plays as a cold open would.
Status: rig, thoroughly (18 cases across all seven kinds x 2/3/4 players, all `BOARD-STUCK: 0`) plus unit (`ArrivalReadoptTests` x7).
1.0(29) corrected the rig itself here: the first arrival now waits for `BoardAnimator.waitForSettle` as well as its fixed 2.5s, because opening the parent chain REPLAYS its last turn and a turn of several actions runs far longer than that.
Every run whose warm-up ended in a big move was really posing the mid-animation case below, on a board whose marks were still those of the bubble before the one on screen.

**The very first move of a game arriving.**
No prior bubble to diff against, so the board must animate the opening attack rather than assume it.
Status: rig (`ARRIVE_WARMUP=0`).

**An arrival landing mid-animation of the previous one.**
Decided (change), and this is the largest decision of the 1.0(28) pass - see "The conflict model" below.
Today the newest sequence simply takes over and the old one stops stepping wherever it happened to be, which leaves the board mid-gesture and cuts to a different one.
It must instead REVERSE what is in flight - animate it back to the parent state, tinted red - and only then play the newest move.
Status: both for the supersede mechanics (`SequenceTeardownTests.testASupersededSequenceHandsItsOpensOnInsteadOfDroppingThem`; rig burst), **spec** for the reversal.

**Several arrivals in quick succession.**
Decided (change), and it follows from the same rule: undo whatever is animating, then play ONLY the last arrival.
Explicitly NOT a queue - the intermediate boards are not replayed one by one, because the newest chain is the thread's truth and watching three supersedes in a row would be slower than the moves themselves.
Status: rig (`ARRIVE_N=3 ARRIVE_GAP=120`) and unit (`SequenceTeardownTests` x4), both against the old take-over-immediately shape.

**A duplicate delivery of the bubble already on screen.**
Animates nothing, and must not raise a veil that no view change will lower.
Status: both (`ArrivalReadoptTests.testADuplicateArrivalCannotStrandTheVeil`, `testAnEqualBoardArrivalCannotArmTheVeil`; rig `ARRIVE_DUP`).

**An older or stale bubble arriving.**
Ignored by Rule P: nothing animates.
Status: unit (`MessageConcurrencyTests`, `StaleBranchTests` x5).

**An arrival while I have a move staged.**
Decided (change).
My staged move is dropped - the arriving chain is the thread's truth - but it must now be VISIBLY retracted first, by the same reversal the conflict model describes, rather than vanishing between two frames.
This is the commonest conflict on a real table and the one where a silent swap is least defensible: the card I chose is on the table one frame and gone the next, with nothing to say it was mine or why it went.
The existing invariant stands unchanged - the board must never render new-base-with-old-staged-moves, not even for one frame - because the retraction plays against the OLD base and the new one is adopted after it.
Status: unit (`SendWindowTests.testAnArrivalClearsStagedMovesBeforeItSuspends`) for the drop; **spec** for the retraction.
**Not rig-tested**, and a real table produces this constantly.

---

## The conflict model (decided 1.0(28))

The three Channel E changes above are one rule, and the owner named the precedent: this is what the web app already does when an optimistic move is superseded.

**The rule.**
When something must be taken off the board that was not taken off by a move - a staged move an arrival overrides, a sequence a newer arrival supersedes - the board REVERSES it before it plays anything else.
The cards travel back the way they came, tinted red, and only when the board is standing at the parent state does the newest chain animate forward from there.
Never a cut, never a snap: two moves in opposite directions, in order.

**Why the red.**
A reversal and a play are the same motion with the sign flipped, so without a colour the player cannot tell "my card went down" from "my card came back".
The web settled on this after months of glitch-fixing and the phone should not invent a second vocabulary: `AnimationOverlay.tsx` draws a reverting card with a red border (`rgb(220,38,38)`), a red drop shadow, a warmed filter and a pink face.
The iOS equivalent is a tint on the flight ghost only - the card in the hand it returns to is a normal card again the moment it lands.

**What already exists, and should not be rewritten.**
The web's decision logic is NOT in TypeScript.
It was lifted into the shared animation core in C, which the phone already links: `c/src/anim_plan.h`, and specifically `anim_resolve_unconfirmed_attack_covers`, which returns the three-way verdict this model needs -
REVERT (fly it back, it was never accepted),
MERGE (keep it, its own confirmation is still coming),
CLEAR (it WAS accepted and this same broadcast swept it off the table, so drop it with no revert animation - reverting these is the "I put a card down, someone picked it up, and it flew back to my hand" flicker).
`src/state/optimisticConflicts.ts` is the thin marshalling wrapper and its header comment is the best prose on the subject in the repo.
Native tests exist (`c/tests/anim_plan_test.c test_optimistic_revert`) and so does an end-to-end one (`e2e/optimistic_revert.test.ts`).

**What is genuinely different on the phone, and must be designed rather than ported.**
The web reverts against a SERVER VERDICT; there is no server here.
An iMessage conflict has exactly two sources - an arrival that supersedes my staged move, and an arrival that supersedes a sequence still in flight - and in both the "verdict" is simply that a newer chain exists.
So the C verdict function is the wrong entry point as it stands; what carries over is the trichotomy, the ordering (reverse fully, then play), and the red.
The CLEAR case in particular has a direct twin here and is the one to get right first: if the arriving chain contains my staged card because the arriving player covered it or picked it up, my card must NOT fly home - their move animates it away, and reverting it first would be the same flicker under a different name.

Status: **spec**, in every part.
Nothing on the phone reverses anything today.

---

## Cross-cutting: the role marks

**Defender change (shield).**
The shield flies from the old defender to the new along an arc, spinning a whole number of turns, and the receiving seat makes way just as the ghost arrives.
The same flight serves a bout end and a mid-bout transfer; what differs is only WHEN it is fired (see "Pass / transfer").
Decided (keep), and the spin is confirmed as EXACTLY ONE turn - which is what `roleFlights` already passes (`spin: 360`).
More than one turn would read as a flourish; a fraction snaps on hand-over, which is round 21's "the shield kinda turns a little bit then turns back".
Status: unit (`RoleMotionTests` x18 - the best-covered area in the app).

**First-attacker change (sword).**
The tinted lead sword flies to the new opener, and the old opener loses it even while still wearing a check.
Status: unit (`RoleMotionTests.testTheSwordThatFliesIsTheOpenersOwn`, `testTheSwordLeavesTheOldOpenerEvenWhileTheyWearACheck`).

**A shield and a sword changing in the same beat.**
Decided (keep): both fly at once.
Status: unit, via `roleFlights` returning both.

**Good declared (sword to check).**
A flip in place, not a flight.
Status: unit (`RoleMotionTests.testSayingGoodTravelsNowhere`, `testAMarkThatBecomesAnotherMarkFlips`).

**Goods cleared by a throw-in (check back to sword).**
Decided (change).
The checks must turn back into swords IN PARALLEL WITH the throw-in card that clears them - the same beat, not the closing one.
Today a good being SET plays at the FRONT of the stream and a good being CLEARED plays at the BACK with the other consequences, and that asymmetry was deliberate (see `goodsOpening`): flipping a cleared good early would snap an attacker's check to a sword before the card that cleared it had left the hand.
Parallel is the answer that was missing from that reasoning.
The card and the marks belong to one another - the throw-in is WHY the goods cleared - so they move together and neither leads.
Status: unit (`Round28ShapeTests.testClearedGoodsTurnAndNothingChangesHands`, `testAGoodThatStillStandsKeepsItsCheck`, `testEachRuleIsDeafToTheOthersCase`), built as `goodsCleared` and fired beside the group's own flight.
The two older tests survived unchanged, which was the surprise: they assert that `goodsOpening` says nil for a clear, and it still does - what moved is where the clear goes INSTEAD, not whether it opens.

**A seat's mark simply ending.**
It turns edge-on rather than fading.
Decided (keep).
Status: unit (`RoleMotionTests.testARoleThatSimplyEndsTurnsEdgeOnRatherThanFading`).

---

## The out badge

Owner, 1.0(24): "if a move puts a player out, their badge should rotate out - so squish to width 0 and not expand back out."

The seat's whole badge - name, count, mark - collapses horizontally to zero width on the move that puts that player out, and stays collapsed.
It is a rotate, not a fade: the badge turns edge-on the way a card does, and there is no spring back.

**The timing, decided 1.0(28): in parallel with the card motion of the move that ends them.**
Going out always means that player's last cards are in the air - onto the table, or into somebody's hand - so the badge collapse and those flights are one event and read as one.
Not a beat of its own, and not deferred to the closing role hand-off.

**A seat that was ALREADY out when the board opened** is drawn collapsed from the first paint, with no animation.
An out player is a fact about the board; only the MOMENT of going out is an event, and a board that animates every previously-out seat on every open would replay other people's exits forever.
The distinction is per-bubble, not per-game: if a seat goes out DURING the bubble I am watching, everyone watching that bubble sees the collapse.

Status: unit for the RULE (`Round28ShapeTests.outsWith` x4 - the lookahead, its stopping condition, several seats at once, and a stream that collapses nobody), built as `FSeatBadge.collapsed` driven by `MessageTableView.outShown`.
The VISUAL has never been watched: nothing rig-tests a collapse, and the seat arc keeping its width while a badge is edge-on is asserted by construction (a `scaleEffect`, which does not change layout) rather than by a snapshot.
Round 16's answer - a dimmed name and no card count - is still underneath it, and is what a collapsed badge would show if the scale were ever removed.

---

## Reports that landed in these gaps

Written the same day this file was, both on 1.0(24), and both in the RELEASED-SETTLEMENT-AFTER-SEND cell that Channel B marks as not rig-tested.
Neither is a design question and no decision above changes either one: they are wrong under every answer in this document.

**A round-ending good after Send played every animation correctly, but the attack card and the cover on top of it stayed on the table.**
Candidate mechanism, unproven: `runEventStream`'s teardown drops the pre-bout sweep grid only when `mySeq == animSequenceToken`.
A superseded sequence hands its OPENED CARDS on (`orphanedOpens`) but not its swept table, so a bout-end sequence that loses the token leaves the grid standing with nobody to take it down.
Every oracle that compares the controller's view against the kernel reads CLEAN while that happens, because `view.battles` really is empty - the phantom cards are the sweep grid, not the view.

**A pickup after Send flew a card from the DRAW PILE onto the table, landing where the picked-up card had been.**
A refill flight resolving its destination to a table rect instead of a hand slot.
Not yet diagnosed.

## What nothing covers

Stage-and-send for a bout-ending COVER, and for the game-ending move; both are the "held settlement releases on Send" path, which is where two of the four 1.0(23)-24 reports landed.

Cold open of a cover, and of a bout-ending cover.

The `out` and game-over visuals as ANIMATIONS - only the rules behind them are tested.

Quiet-open consumption after a send, in the rig - and now the reopen-replays rule that replaced it.

Every part of the conflict model: no reversal, no red, no ordering.
