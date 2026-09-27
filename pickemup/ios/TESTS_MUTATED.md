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

## The review, O6 and O9 tests (the B2 simulator worker, 2026-09-27)

Run on a fresh iPhone 17e, `pk-b2` (iOS 27.0), created for this pass and deleted after it.
The unmutated scheme is 37 tests, 0 failures, before and after every row below, and 38 once `AnchorTests` joined it.
Each Swift mutant was applied alone by `mutation_check.sh` with `-only-testing:` on the class it aims at (the source restored and checked byte for byte); the C mutant rebuilt the xcframework before and after.
Until this pass none of these had run: they were compiled only (ORCHESTRATION B2, B3).

The first run found two of them red, and neither was a product regression.
`ActionCardCornerTests` and `NoCountLeakTests` read labels off a hosted SwiftUI view, and SwiftUI builds its accessibility elements only once an assistive client (VoiceOver, an XCUITest runner) has switched accessibility automation on; a plain unit-test process never has, so both walks found an EMPTY tree ("both other seats are on the tree: []", "XCTUnwrap failed").
The views were right all along: every seat label is the kernel's `PK_API_W_SPOKEN_FAN` word and the fan carries no count.
The fix is in the tests, `PickemupKitTests/AXTree.swift`: it switches automation on once per process, as a client would, and is the one tree walker both tests use (IOS_DECISIONS I42).
The first row below is that cause, proven: with the switch turned off again, both tests fail exactly as they first did.
`NoCountLeakTests` was also narrowed to what it guards: it now walks each other seat's badge and fan elements, label and value, and asserts both are on the tree, where it used to read every label that merely contained a name (a future caption such as "Bo played 9 of squares" would have tripped it without being a count).

Three planned mutants did not go red as first written, and each exposed a test that could not fail:
- `KernelWordsTests.testTheRulesAreAsManyAsTheKernelHas` SURVIVED `out.count < 4`: its probe buffer was 64 bytes, a rule line is longer, and the kernel answers -1 for a buffer too small as well as for past the last, so the assertion passed whatever `Pk.rules` returned; the buffer is now 1024, as `Pk.rules`' own.
- `FanTapTests` and `TableModelTests.testATapOnAFanStagesTheCatchAndASecondTakesItBack` SURVIVED `.moved` and `.uncalled` mapped to `.refused` (the planned `case` edits do not compile under warnings-as-errors, so the mutant moved into `Pk.tapFan`'s mapping): since I30 every branch of `TableModel.tapFan` refreshes, so the call on screen is the kernel's either way; what the branch still owns is the staging and the motion, and the tests now assert those.

| Test | Mutation | Assertion that went red |
|---|---|---|
| ActionCardCornerTests.testAnActionCardExposesItsSuitShape, NoCountLeakTests.testNoOtherSeatsLabelCarriesADigit | `AXTree.enable` passes 0 to `_AXSSetAutomationEnabled` (the cause, reproduced) | "XCTUnwrap failed: expected non-nil value"; "Bo's badge is on the tree: []", "Bo's fan is on the tree: []", the same for Cy |
| ActionCardCornerTests.testAnActionCardExposesItsSuitShape | `CardFace.cornerSuit` drops Skip (`rank == PK_R_REVERSE \|\| ...`) | "a skip on squares carries the square" ("" against "square") |
| NoCountLeakTests.testNoOtherSeatsLabelCarriesADigit | SeatBadge's fan label appends its backs (`Pk.words(PK_API_W_SPOKEN_FAN, seat) + " 3"`) | "no digit in another seat's label: Bo's cards. Tap to catch them on one 3", the same for Cy |
| FanTapTests.testATapOnAnotherFanMovesTheCallAndARefusedTapKeepsIt | `Pk.tapFan` maps `PK_API_FAN_MOVED` to `.refused` | "the moved call stages again" (1 against 2) |
| TableModelTests.testATapOnAFanStagesTheCatchAndASecondTakesItBack | `Pk.tapFan` maps `PK_API_FAN_UNCALLED` to `.refused` | "the un-call plays its own motion (the ring fades off)" (plan 1 kept) |
| HostGateTests.testAMismatchedPairShowsUnreadableAndReadsNothing | `PickemupHost.adopt` drops its `guard readable` | the answer (0 against `PK_EFORMAT` -3), "nothing was adopted" |
| ZoneTests.testTheZonesAreTheKernels | `PkLayout.Zone` maps `.drawBand` to `PK_ZONE_PILE_DROP` | "the draw band (U24)" (x, y and width) |
| KernelWordsTests.testTheCornerIndexIsTheKernels | `CardFace.label` returns the kernel's word even when it is "" | "a skip prints its glyph, not an index", "a plain wild prints no index" |
| KernelWordsTests.testTheRulesAreAsManyAsTheKernelHas | `Pk.rules` stops at four lines (`out.count < 4`) | "every rule the kernel has" (143 against -1), after the buffer fix above |
| DrawnStayTests.testAnOlderTimerNeverHidesANewerDrawnStay | `showDrawnStay`'s timer clears `drawnStay` without comparing the generation | "a newer showing stays up" |
| BeatPlayerTests (arrival, superseding, no animation) | `PickemupHost.adopt` plays `Pk.beats(from: 0, to: 1_000, open: true)` instead of `Pk.beatsNow()` (the newest bubble as if opened cold) | "from the bubble on screen", "the kernel's stagger" ([100, 210, 320] against [16, 126, 236]), "at the fan's right end", "mid-flight", "adopting the same bubble again moves nothing" |
| ArrangeTests.testAReorderMovesTheCardAndTheNextViewReadsTheNewOrder, testAPlayAfterAReorderPlaysTheRightCard | `TableModel.arrange` skips its `refresh()` | "the next view reads the new order", "slot 0's card is drawn last", "the card at that slot plays", "the pile's top is the dragged card" |
| ArrangeTests.testAReorderMovesTheCardAndTheNextViewReadsTheNewOrder | `TableModel.shown` sets `s.slot` from the hand's indices | "the next view reads the new order"; also testAPlayAfterAReorderPlaysTheRightCard "it is drawn first" |
| ArrangeTests.testAPlayAfterAReorderPlaysTheRightCard | `TableModel.arrange` passes `pos` as the from slot | "the card at that slot plays" (82 against 65), "the pile's top is the dragged card"; also "a drag to where it already is moves nothing" |
| ArrangeTests.testADrawAfterAReorderLandsOnTheRight | (C) `pk_arr_sync` puts a new card at the left (`insert_at(a, 0, ...)`), library rebuilt | "the drawn card lands on the right" (14 against 93); the deal also goes through the sync, so every ArrangeTests test is red on its acquisition order |
| ArrangeTests.testTheDropIsTheKernels | `PkLayout.drop` maps `PK_DROP_HAND` to `.pile` | "a release in the row rearranges" |
| AnchorTests.testTheAnchorsAreWhereTheViewsAre (added for I44) | `TableScreen` moves the stack anchor with `.offset` again (the pre-I44 code) | "the pile's anchor is the pile" (41 against 187, 57.5 against 317.5), "the deck sits left of the pile (U3)", "on its line" |

The two rows under BeatPlayerTests above that mutated `PickemupHost.adopt`, and the `tapFan never un-calls` row under TableModelTests, mutated Swift that is now the kernel's (`pk_api_adopt`, `pk_api_tap_fan`, I29 and I30); they are replaced by the adopt and `PK_API_FAN_UNCALLED` rows in this table, which mutate the code as it is now.

### The open-items pass (`docs/OPEN_ITEMS.md`), seen red

Added or changed 2026-09-27 with no simulator; run and mutation-checked the same afternoon by the B2/B3 simulator worker on `pk-b2`, after merging, with the whole scheme at 42 tests, 0 failures before and after.
The C behind each was seen red on `build/pk_beats_test`, `build/pk_twophone_test` or `build/ios_smoke` (`pickemup/c/tests/MUTATIONS.md`, the rows marked A12, A13, A14, A15 and I37).
Each Swift mutant was applied alone by `mutation_check.sh` with `-only-testing:` on its test; the key rename was applied to `keys.h` and `strings_en.c` together (a rename in one alone does not compile), the library rebuilt before and after.

| Test | Mutation | Assertion that went red |
|---|---|---|
| BeatPlayerTests.testAPickedWildSlidesItsBandUp | `BeatPlayer.effects` skips `PK_BK_BAND` (its `case` arm removed) | "hidden under the foot until it starts" (nil against 0), "the band is part way up" |
| TableModelTests.testPlayStagesWithTheKernelsCaption | `TableModel.play` stages `after: .draw` | "a play collapses" |
| TableModelTests.testATapOnAFanStagesTheCatchAndASecondTakesItBack | `TableModel.tapFan` stages a call `after: .play` | "a call alone does not collapse the drawer (I37)" |
| BeatPlayerTests.testTheLobbyRowsFadeInAndOutOnTheKernelsBeats | `TableModel.join` plays nothing when the join does not start the game | "a join fades its row up" |
| BeatPlayerTests.testTheLobbyRowsFadeInAndOutOnTheKernelsBeats | `BeatPlayer.effects` drops the leave's `roster.gone` edit | "the row that left fades where it stood" (nil against 1) |
| BeatPlayerTests.testTheLobbyRowsFadeInAndOutOnTheKernelsBeats | `BeatPlayer.effects` answers `open` 0 while the close-up is pending | "the rows below stand one lower" (0 against 1) |
| DevFlaggedTests.testTheCollapseSlideRunsOnTheKernelsPush | `CollapseSlide.pickemup` hands the kernel seconds, not milliseconds | "the whole travel at the flip, nothing at the end" (499.9 against 0), "the host's spring, as the kernel has it" (500 against 89.46) |
| DevFlaggedTests.testTheCollapseSlideRunsOnTheKernelsPush | `CollapseSlide.pickemup` takes its steps from `PK_LAY_COLLAPSE_MS` | "the kernel's keyframes" (600 against 120) |
| DevFlaggedTests.testTheSendReminderSaysTheKernelsWordAfterItsFuse | the `SEND_HINT` key renamed in `keys.h` and `strings_en.c` (the generated key list follows it), library rebuilt | "the reminder's word is the kernel's" ("" against "Send") |

What no unit test reaches in this pass: `BandSlide` drawing the band where PkFX says, `LobbyScreen` drawing the gone row and the lowered rows, the slide on a real drawer and the reminder's place under Messages' Send button.
Each is a screenshot or a filmed take owed on a simulator (the band, the rows) or a phone in Messages (the slide, the reminder), with `dev.slide` and `dev.sendhint` in the App Group.

Every test in `PickemupKitTests` now has a row above; nothing is left unmutated.
What these tests do not reach is the conversation itself (`PickemupMessages/MessagesViewController.swift`: staging through the insert loop, send, cancel, receive); it has no test target, as uttt's has none, and what was seen of it inside Messages is in `pickemup/docs/SIM_VERIFICATION.md`.

## The B4 pass (2026-09-27, `pk-b4`, iPhone 17e, iOS 27.0)

Each mutant applied alone by an exact string replacement, the named class run on the simulator under a watchdog, the source restored and compared byte for byte.

| Test | Mutation | Assertion that went red |
|---|---|---|
| CompactTests (281, 299) | TableScreen draws the pile and deck at `scale: CGFloat = 1` | "the pile is drawn at the scale" (82 against 57.4), "the deck is drawn at the scale", "the deck on the pile's line (U3)", "the pile clear of the fan across the table" (61.7 against 50.9) |
| CompactTests (281, 299) | the pills never stack (`pillsStacked(collapse: 0)`) | "no pill over the pile" (pill.pass at 162, 139 over the pile) |
| CompactTests (281, 299, 340) | StatusCorner always gets the sub-line | "the drawer's status corner ends with its strip: no sub-line" (65.3 against 48.5); it survived until that assertion was added |
| OpenedBubbleTests.testTheClockStartsOnTheFirstBoardFrameOnScreen | `play` starts the clock whether or not the board is on screen | "nothing has played behind the white drawer" (1200 against 0), "no clock yet", "a body drawn off screen starts nothing", "at or after the first board frame" (1000 against 1001.5) |
| OpenedBubbleTests.testTheClockStartsOnTheFirstBoardFrameOnScreen | `ms()` never starts a waiting clock | "the clock starts on the first frame on screen" |

Not mutated: `OpenedBubbleTests.testAPlanWaitingForTheBoardIsNotEndedBehindIt` (its mutant is the first row's, which the other test already catches; not run alone), and the badge's stamp overlay (no Swift test places a stamp and measures the fan; owed).

