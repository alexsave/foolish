# PickemupKitTests, every test seen red

Each row is one mutant, applied alone with the ios-sim-verify `mutation_check.sh` (an exact string replacement, the source restored and verified byte for byte afterwards), and the full `PickemupKitTests` scheme run on the iPhone 17e simulator `FC7586CF-78D5-4E61-810E-9449B9AC6C5A` (iOS 27.0).
Every run executed all 17 tests; a row lists the assertions that went red, which were always in the test the mutant was aimed at, plus any other test that reads the same code.
Run 2026-09-27 on the commit that added this file.
The unmutated suite is 17 tests, 0 failures.

The C side's own rows (the layout thresholds, the words, the ranks, the buried cards) are in `pickemup/c/tests/MUTATIONS.md`.

## TableModelTests

| Test | Mutation | Assertion that went red |
|---|---|---|
| testDrawOpensTheDraftAndStagesNothing | `stageIfSendable`'s canSend guard never refuses | "a draw stages nothing" |
| testPlayStagesWithTheKernelsCaption | `play` stages with `collapse: false` | "a play collapses" |
| testPlayStagesWithTheKernelsCaption | `stageIfSendable` captions with `W_HEADLINE` | "the caption is the kernel's staged caption" ("Waiting on Alex" against "Bo played 9 of squares. Alex to play") |
| testAWildWaitsForItsSuit | `play` skips the `Pk.isWild` branch | "no pills under the picker", "the choice stages the wild", the picker position (6 in all) |
| testAWildWaitsForItsSuit | `choose` plays suit 0 whatever was tapped | "the chosen suit is live" (0 against 2), the strip's chosen suit |
| testCancellingThePickerLeavesNothingStaged | `cancelPicker` plays the wild with suit 0 | "the wild is home", "cancel stages nothing" |
| testUndoOfADrawIsRefusedAndSaysSo | `undo` drops `refusedBelowFloor` | "the refusal says drawn cards stay (U23)", the subline |
| testUndoOfAPlayBringsTheCardHome | `Pk.undo` always answers false | "the play comes back", the strip still shows the card |
| testATapOnAFanStagesTheCatchAndASecondTakesItBack | `tapFan` never un-calls | "a second tap un-calls" |
| testTheStagedLinkRoundTripsToTheOtherPhone | `Pk.read` answers 0 without adopting | `t.me` 1 against 0, "Alex's own seven" (6 against 7); also testJoiningADMStartsIt |
| testTheMaskedViewNeverExposesAnotherSeatsCount | (C) `pk_view` fills `reveal[]` while live (`if (1)` for `if (g->over \|\| viewer == PK_VIEW_ALL)`), library rebuilt | "no count while it is played" |
| testJoiningADMStartsIt | `join` always calls `Pk.join` | "the second player's join starts the game" |

## LayoutTests, CardFaceTests, RenderTests

| Test | Mutation | Assertion that went red |
|---|---|---|
| testTheHandOverflowsAsO4Says | `PK_LAY_OVERLAP` maps to `.flat` | "twenty-seven overlap"; also the drawer's fourteen |
| testTheHandOverflowsAsO4Says | `PK_LAY_SCROLL` maps to `.overlap` | "forty-two scroll"; also the drawer's thirty |
| testTheDrawerKeepsOneRow | `maxRows` answers 2 | "the drawer keeps one row", the rows of fourteen |
| testEverySlotIsInsideTheRowAndFacesAreRight | the slot loop stops one short | "a slot per card" (for every n) |
| testEverySlotIsInsideTheRowAndFacesAreRight | `thin` becomes `cardW <= thinBelow` | "an overlapped card keeps its face (U8)" |
| testThePillSlots | `PK_PILL_PASS` maps to `.undo` | "Pass beside Draw"; also testDrawOpensTheDraftAndStagesNothing "and Pass stands beside it (D10)" |
| testTheRingPutsMeAtTheBottom | `PkLayout.seat` passes the seat as `me` | "the seat across is at the top" |
| testFacesComeFromTheKernel | `CardFace` reads the rank for the suit | "circle one" |
| testTheBubbleRendersAt300By195 | `BubbleSnapshot.size` 300 x 300 | "the bubble is 300 x 195" |

## BeatPlayerTests (the motion worker, 2026-09-27)

Run on the OTHER iPhone 17e, `6E0A730D-2655-4E8E-B369-2CF274879D31` (iOS 27.0), with `-only-testing:PickemupKitTests/BeatPlayerTests`: the unmutated suite is 8 tests, 0 failures, and `TableModelTests` (10) stays green beside it (18 tests, 0 failures).
Each mutant was applied alone by a script (exact string replacement, the source restored and checked byte for byte), and the two C mutants rebuilt the xcframework before and after.
The whole scheme could not be run there: `ActionCardCornerTests` and `RenderTests` hang on that simulator (ORCHESTRATION B3).

| Test | Mutation | Assertion that went red |
|---|---|---|
| testADrawIsOneFlightDeckToTheNewSlotThenAFlip | `BeatPlayer.anchorName` maps `PK_ANC_HAND` to "hand" | "the flight lands in the new slot" ("hand" against "hand.7") |
| testADrawIsOneFlightDeckToTheNewSlotThenAFlip | `BeatPlayer.ghosts` draws no flip ghost | "then it turns over there", "face up in its second half" |
| testAnArrivalFliesEachDrawnCardIntoTheSeatsFan | `BeatPlayer.rect` lands a fan flight at the fan's left edge | "at the fan's right end" (153.98 against 174 +/- 2) |
| testAnArrivalFliesEachDrawnCardIntoTheSeatsFan, testASupersedingPlanReplacesThePlayingOneCleanly | `PickemupHost.adopt` plays the newest bubble as if opened cold | "from the bubble on screen", "the kernel's stagger" ([100, 210, 320] against [16, 126, 236]), "mid-flight" |
| testASupersedingPlanReplacesThePlayingOneCleanly, testAStagedPlayHoldsItsSettleUntilSend | `BeatPlayer.play` keeps a playing plan (`guard self.plan == nil`) | "the new plan starts from its own beginning" (181 against 0), "the board is the new plan's start, not a revert" (86 against 89), "the bar moves to Alex" |
| testASupersedingPlanReplacesThePlayingOneCleanly | `BeatPlayer.play` does not restart the clock | "mid-flight", "the new plan starts from its own beginning" |
| testNoAnimationWhenFromEqualsTo | (C) `pk_api_beats` lays out the newest bubble when `from == to`, library rebuilt | "no motion when nothing arrived" (5 against 0), "an empty plan clears" |
| testNoAnimationWhenFromEqualsTo | `PickemupHost.adopt` plays the newest bubble when nothing is newer | "adopting the same bubble again moves nothing" |
| testAStagedPlayHoldsItsSettleUntilSend | `BeatPlayer.ended` drops a plan that holds a staged draft | "the staged draft keeps its frame after the flight", "Bo's turn bar has not moved" |
| testAStagedPlayHoldsItsSettleUntilSend | `TableModel.sent` plays nothing | the Send plan's mode (stage against send), "the bar moves to Alex" |
| testUndoFliesThePlayedCardHome | `TableModel.undo` clears instead of flying the card home | "the card flies home" |
| testThePickerTilesAreTheKernels | (C) `PICKER_REACH_X` 96 becomes 90, library rebuilt | "triangles east", "diamonds west", "the x" |
| testTheJoinThatStartsTheGamePlaysTheDeal | `TableModel.join` never calls `onDealt` | "the table takes over" (lobby against table), "the join that fills the table plays the deal" |

## Not mutated

`ActionCardCornerTests.testAnActionCardExposesItsSuitShape` (O6, added 2026-09-27 by the second simulator worker) is compiled but has NOT run: the simulator would not boot (ORCHESTRATION B2).
Its planned mutant is `CardFace.cornerSuit` answering nil for Skip, which must go red on "a skip on squares carries the square"; until that red run exists this test proves nothing.

Every other test in `PickemupKitTests` has a row above.
What these tests do not reach is the conversation itself (`PickemupMessages/MessagesViewController.swift`: staging through the insert loop, send, cancel, receive); it has no test target, as uttt's has none, and it has NOT yet run inside Messages: on 2026-09-27 the app installed and registered on the simulator, but `simctl launch com.apple.MobileSMS` hung for over five minutes (BLOCKED B2 in `pickemup/docs/ORCHESTRATION.md`).
