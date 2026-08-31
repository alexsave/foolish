# The animation catalogue

Every animation SHAPE the iMessage board can play, what it should look like, and whether anything actually checks it.
Card combinations are not enumerated - "a cover" covers every pair of cards; "a multi-card cover" is a different shape.

Written at 1.0(24).
Status values are `unit` (a named XCTest), `rig` (a FoolishHarness scenario that has actually been run), `both`, `spec` (defined here, not built), or `untested`.
Keep the status column honest: a shape nothing exercises must say so, because the point of this file is to be the list of what we are NOT sure about.

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

---

## Channel A - my own move, staged, before Send

**Single-card attack.**
The card flies from its own hand slot to its table slot, pre-hidden at the source so it flies rather than pops; the hand re-fans; deck and discard badges do not move; no role changes.
Staging then auto-collapses the drawer to Messages' Send.
Status: both.

**Multi-card attack (several thrown in at once).**
All cards fly together as one parallel group, each from its own hand slot to its own table slot - never staggered.
Status: unit (`MessageParallelCoverTests`, `MessageMulticoverFlightTests.testARealKernelMulticoverAlsoSpreads`).

**Throw-in onto an already-open bout.**
As above, but the new attack lands beside existing battles and the grid re-lays out; existing covers stay tilted while the newcomer arrives upright.
Status: unit (`CoverTiltTests.testACoverFromAnEarlierBubbleIsTiltedOnTheFirstPaint`, `testAnUncoveredAttackNeverTilts`).

**Pass / transfer (classic rules only).**
The card flies to the table as an attack does, AND the shield flies from me to the next defender - a flight, not a flip, because I stop being the defender.
Status: **untested as a shape.** The rules are unit-tested (`PodkidnoyTests.testADefenderIsNeverOfferedATransfer`) and the shield flight is unit-tested generically (`RoleMotionTests.testTheShieldFliesToTheNextDefender`), but `pass` is not a rig kind and this shape has never been posed end to end in any channel.

**Single cover.**
The card flies from its hand slot to the battle it covers, arriving ALREADY ROTATING into its tilt rather than snapping to it.
The target is the highest attack the card can beat, not the leftmost.
Status: unit, heavily (`CoverTargetTests` x12, `MessageMulticoverFlightTests.testACoverFliesInAlreadyRotating`, `CoverTiltTests.testAReplayedCoverIsUprightUntilItLands`).

**Multi-card cover (one card per attack, one move).**
Each card flies to the attack it specifically covers; the pairing is the kernel's, and a mismatched pairing is refused wholesale rather than guessed at.
Status: unit (`MessageMulticoverFlightTests` x9, `MessageParallelCoverTests`).

**Cover that closes the bout (the defender's last cards).**
The first shape where the cut bites: the cover lands and STOPS there, table still on screen, discard sweep and refills withheld until Send.
Status: unit (`MessageBoutEndHoldTests.testACoverThatClosesTheBoutHolds`, `testAMultiCardCoverHoldsOnceAfterTheWholeGroup`, `testARealClosingCoverHolds`) and rig as an ARRIVAL (`coverend`), but never through a real Send in the rig.

**Pickup.**
The whole table lifts and flies into my hand; the sweep grid holds the cards where they sat so nothing fades out from under them.
The attacker's refill is withheld.
Status: both (unit `MessageEventsTests.testOpenReplayReconstructsThePreBoutTableForAPickup`; rig stage+send).

**Pickup with throw-ins.**
As above, but every card including late throw-ins flies to my hand - the sweep must carry cards the pre-move view never held.
Status: unit (`MessageBoutEndHoldTests.testTheSweptTableCarriesTheCoverThePriorViewNeverHeld`, `testTheSweptTableIsOnlySwappedForOneThatKeepsEveryCard`).

**Plain good (does not close the bout).**
Emits NO step at all - the stream is empty.
The only visible change is my sword rotating into a green check, in place, no flight.
Status: unit (`RoleMotionTests.testSayingGoodTravelsNowhere`, `MessageStagedDealTests.testAStagedGoodShowsTheGoodMarkAndKeepsTheTable`, `MessageBoutEndHoldTests.testAGoodBubblesDiscardDoesNotHold`).

**Bout-ending good, before Send.**
The sword-to-check rotation ONLY; the discard sweep, both refills and the role hand-off are all withheld.
Owner, 1.0(23): "it correctly only showed me the good checkmark role transition before I sent it."
Status: unit (`MessageStagedDealTests.testAStagedTurnNeverShowsACardOffTheDeck`, `testAStagedGoodShowsTheGoodMarkAndKeepsTheTable`).

**A move that puts a player out.**
`out` is NOT a settlement step, so a cover that empties a hand does not hold - it plays straight through.
The seat's BADGE rotates out: it squishes horizontally to zero width and STAYS there, never expanding back.
Status: the cut rule is unit-tested (`MessageStagedDealTests`: `[cover, out].settlementStart == nil`); the badge behaviour is **spec** - see "The out badge" below.

**Undo.**
The staged card returns to its hand slot, the held settlement is DROPPED rather than released, and re-staging replaces the input bubble.
Undo-to-empty re-seals the base and says on the wire that it carries nothing.
Status: unit (`MessageUndoBubbleTests` x5, `MessageStagedDealTests.testUndoDropsTheHold`, `SendWindowTests.testUndoNeverPassesThroughAnEmptyBoard`).

---

## Channel B - the same move, at Send

**Send of a move that did not end the bout (attack, cover, throw-in, plain good).**
Nothing animates.
The Undo pill goes on the TAP itself, not after the rebase, and the board rebases onto the sent bytes with the picture unchanged.
Status: both (unit `SendRebaseTests.testTheUndoPillGoesOnTheSendSignalNotOnTheRebase`, `MessageSendStaysOpenTests`; rig stage+send).

**Send of a bout-ending cover.**
The withheld half plays: table sweeps to the discard, each player refills in kernel order, then the role hand-off flight.
Status: unit (`MessageStagedDealTests.testSendReleasesTheHeldSettlement`, `MessageBoutEndHoldTests.testTheHoldIsAReadableBeatAndScalesWithTheFlights`).
**Not exercised through a real Send in the rig** - the auto-player never picks a closing cover.

**Send of a bout-ending good.**
The same released settlement, with the check already showing because staging showed it.
This is the 1.0(23) report.
Status: both (rig: goodend stage+send runs clean).

**Send of the move that ends the game.**
Released settlement, then the result screen: final board, then the rank list.
Status: rig only as an ARRIVAL (`gameover`).
**Never tested as my own send.**

**Send handed the wrong bytes.**
Nothing animates and nothing changes - base, `pending` and `sending` all stand, because a refusal that dropped the staged move would walk the board back by exactly the move just played.
Status: unit (`SendRebaseTests.testASendCannotRebaseTheBoardOntoAnOlderChain`, `testARefusedSendLeavesTheStagedMoveOnTheBoard`, `SendWindowTests.testASendWhoseBytesCannotBeReadLeavesTheBoardAlone`).

---

## Channel C - reopening my OWN bubble (cold replay)

**Reopen my own attack or cover.**
The board opens on the state BEFORE my move and replays it.
Distinct from Channel A only in that the role marks must be re-seeded from `openReplayPriorState` rather than surviving in `@State`.
Status: both (unit `MessageEventsTests.testAChainsLastMoveIsReplayedToEitherSeat`; rig `ARRIVE_COLD` + `ARRIVE_SELF`).

**Reopen my own bout-ending good.**
Opens with my SWORD STILL UP, rotates it to the check, and only then plays discard, refills and the role hand-off.
The rotation exists only because the prior board is fetched: seeded from the stream's own first frame the check is already on and there is nothing left to animate.
Status: both (unit `RoleOpeningTests.testAGoodOpensFromABoardThatStillHasTheSwordUp` and `testATwoPlayerEndingGoodStillOpensWithTheSwordUp`; rig traced sword -> check -> hand-off).

**Reopen my own bubble immediately after sending it.**
Animates NOTHING - the just-sent marker makes it a quiet open, because I watched the move live.
Status: unit only (`StagedBubbleRouting` tests).
The one-shot consumption is **not rig-tested**, and is the likeliest explanation for a rotation the owner expected and did not see.

**Reopen a bubble that carried no move (undo-to-empty).**
Animates nothing, and does not restart the pickup hold.
Status: unit (`MessageUndoBubbleTests.testAStagedMoveThatWasUndoneAnimatesNothingForTheReceiver`, `testTheEmptyBubbleDoesNotRestartThePickupHold`).

---

## Channel D - opening SOMEBODY ELSE'S bubble cold

**Their single attack, cover, pickup or good.**
The whole turn plays, action half then settlement half, with no hold.
My hand is masked: their drawn cards are hidden, mine come back with real identities.
Status: unit (`MessageEventsTests.testLastMoveEventsRevealMyOwnRefillAndHideTheOpponents`) and rig for attack, goodend, pickup and gameover.
**Cold cover and cold coverend have never been run.**

**Their turn that folds several actions into one bubble.**
A double cover, or a cover following two pending goods: BOTH covers replay, not just the last.
Status: unit (`MessageEventsTests.testAStagedDoubleCoverReplaysBothCovers`, `testTwoCoversStagedTogetherStillReplayBoth`, `testTwoCoversSentSeparatelyReplayOnlyTheSecond`).

**Their game-ending move.**
The final move animates, and THEN the ranks - never straight to the rank board.
Status: rig (`gameover`, cold and live).

**A finished game opened by a spectator.**
As above from viewer -1: no hand, no "(You)" row, final move then ranks.
Status: rig (`spectator`), plus a visual check in round 21.

---

## Channel E - a move arriving on a board that is already open

**Any single arrival.**
The chain folds into the LIVE controller - the board keeps its identity, its measured geometry and its animator - and the move plays as a cold open would.
Status: rig, thoroughly (18 cases across all six kinds x 2/3/4 players, all `BOARD-STUCK: 0`) plus unit (`ArrivalReadoptTests` x7).

**The very first move of a game arriving.**
No prior bubble to diff against, so the board must animate the opening attack rather than assume it.
Status: rig (`ARRIVE_WARMUP=0`).

**Several arrivals in quick succession.**
Each folds in turn, and a later arrival CANCELS a superseded adopt rather than letting it sail on.
Status: rig (`ARRIVE_N=3 ARRIVE_GAP=120`) and unit (`SequenceTeardownTests` x4).

**A duplicate delivery of the bubble already on screen.**
Animates nothing, and must not raise a veil that no view change will lower.
Status: both (`ArrivalReadoptTests.testADuplicateArrivalCannotStrandTheVeil`, `testAnEqualBoardArrivalCannotArmTheVeil`; rig `ARRIVE_DUP`).

**An older or stale bubble arriving.**
Ignored by Rule P: nothing animates.
Status: unit (`MessageConcurrencyTests`, `StaleBranchTests` x5).

**An arrival while I have a move staged.**
My staged move is dropped - the arriving chain is the thread's truth - and the board must never render new-base-with-old-staged-moves, not even for one frame.
Status: unit (`SendWindowTests.testAnArrivalClearsStagedMovesBeforeItSuspends`).
**Not rig-tested**, and a real table produces this constantly.

**An arrival landing mid-animation of the previous one.**
The in-flight sequence hands its opened cards on rather than dropping them.
Status: both (`SequenceTeardownTests.testASupersededSequenceHandsItsOpensOnInsteadOfDroppingThem`; rig burst).

---

## Cross-cutting: the role marks

**Defender change (shield).**
The shield flies from the old defender to the new along an arc, spinning a whole number of turns, and the receiving seat makes way just as the ghost arrives.
Status: unit (`RoleMotionTests` x18 - the best-covered area in the app).

**First-attacker change (sword).**
The tinted lead sword flies to the new opener, and the old opener loses it even while still wearing a check.
Status: unit (`RoleMotionTests.testTheSwordThatFliesIsTheOpenersOwn`, `testTheSwordLeavesTheOldOpenerEvenWhileTheyWearACheck`).

**Good declared (sword to check).**
A flip in place, not a flight.
Status: unit (`RoleMotionTests.testSayingGoodTravelsNowhere`, `testAMarkThatBecomesAnotherMarkFlips`).

**Goods cleared by a throw-in (check back to sword).**
Status: unit (`RoleMotionTests.testTheCheckTurnsBackIntoASwordWhenAThrowInClearsTheGoods`, `RoleOpeningTests.testAGoodThisMoveClearedDoesNotOpenTheStream`).

**A seat's mark simply ending.**
It turns edge-on rather than fading.
Status: unit (`RoleMotionTests.testARoleThatSimplyEndsTurnsEdgeOnRatherThanFading`).

---

## The out badge

Owner, 1.0(24): "if a move puts a player out, their badge should rotate out - so squish to width 0 and not expand back out."

The seat's whole badge - name, count, mark - collapses horizontally to zero width on the move that puts that player out, and stays collapsed.
It is a rotate, not a fade: the badge turns edge-on the way a card does, and there is no spring back.
A seat that was ALREADY out when the board opened is drawn collapsed from the first paint, with no animation - an out player is a fact about the board, and only the moment of going out is an event.

Status: **spec.**
Today the badge stays on screen with its name in a dimmed ink and no card count, which is round 16's answer to the same question and is not this.

---

## Reports that landed in these gaps

Written the same day this file was, both on 1.0(24), and both in the RELEASED-SETTLEMENT-AFTER-SEND cell that Channel B marks as not rig-tested.

**A round-ending good after Send played every animation correctly, but the attack card and the cover on top of it stayed on the table.**
Candidate mechanism, unproven: `runEventStream`'s teardown drops the pre-bout sweep grid only when `mySeq == animSequenceToken`.
A superseded sequence hands its OPENED CARDS on (`orphanedOpens`) but not its swept table, so a bout-end sequence that loses the token leaves the grid standing with nobody to take it down.
Every oracle that compares the controller's view against the kernel reads CLEAN while that happens, because `view.battles` really is empty - the phantom cards are the sweep grid, not the view.

**A pickup after Send flew a card from the DRAW PILE onto the table, landing where the picked-up card had been.**
A refill flight resolving its destination to a table rect instead of a hand slot.
Not yet diagnosed.

## What nothing covers

`pass`/transfer, in any channel - not a rig kind, never posed end to end.

Stage-and-send for a bout-ending COVER, and for the game-ending move; both are the "held settlement releases on Send" path, which is where two of the four 1.0(23)-24 reports landed.

Cold open of a cover, and of a bout-ending cover.

The `out` and game-over visuals as ANIMATIONS - only the rules behind them are tested.

Quiet-open consumption after a send, in the rig.
