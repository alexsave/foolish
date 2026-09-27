# TallybonesKitTests, every test seen red

Each row is one mutant, applied alone by a script (an exact string replacement that must match once, the source restored and checked byte for byte by SHA-256 afterwards), and the full `TallybonesKitTests` scheme run.
Run 2026-09-27 by the integration worker, on the commit that wired the real kernel (BridgeKernel over `tb_api.h`); the stand-in and its rows are gone with it.

**Where they ran: Mac Catalyst, not an iOS simulator.**
Both simulator slots on the build Mac were taken by other agents' simulators while this ran (the two-simulator rule).
With the kernel linked, Catalyst needs the xcframework's Mac Catalyst slice, which `make -C tallybones/c ios-lib-catalyst` adds and `DEST='platform=macOS,variant=Mac Catalyst' scripts/mac_tests.sh unit` builds by itself (DECISIONS T63).
The same bundle builds for testing for `generic/platform=iOS Simulator`.
The unmutated suite is 9 tests, 0 failures, and every mutant below compiled.

The tests drive the REAL kernel: `Phones.swift` plays two phones in one process (pickemup's shape), each with its own identity, nickname and seat records, over the one resident message of `tb_api.c`.

| Test | Mutation | Assertion that went red |
|---|---|---|
| testEveryValueHasThatManyPipsAndUnknownHasNone | `DiceFace.pips(5)` drops the centre pip | "pips per value 0...6" ([0, 1, 2, 3, 4, 4, 6]) |
| testATapTogglesTheKeepMarkAndASecondTapClearsIt | `TallyTable.toggle` sets the mark instead of toggling it | "a second tap on die 2 clears it", "a die that is not there changes nothing" |
| testAStagedKeepShowsBlanksUntilItIsSentThenTheRerollSettlesIn | `TallyTable.toggle` drops its `canKeep` guard | "the refused tap left no mark behind", "exactly the rerolled dice settle" (8 against 12) |
| testAStagedKeepShowsBlanksUntilItIsSentThenTheRerollSettlesIn | `TallyTable.refresh` resets my marks on any roll change (the defect T64 fixed: a staged KEEP's draft is roll 2, so the cancel forgot the marks) | "the refused tap left no mark behind" (all false), "the kept dice held", "exactly the rerolled dice settle" (31 against 12) |
| testAStagedKeepShowsBlanksUntilItIsSentThenTheRerollSettlesIn | `BridgeKernel.view`'s `canKeep` ignores my staged move | "no toggling under a staged KEEP", "no scoring blank dice", "a refused tap changes no mark" |
| testAStagedKeepShowsBlanksUntilItIsSentThenTheRerollSettlesIn, testTheKernelsSettleBlanksTumblesAndLandsOnTheView, testTheReceiverSeesTheSendersRerollAndCard | `BridgeKernel.commit` does not call `tb_api_mark_sent` | "the reroll exists once the KEEP is sent", "the send laid out a plan", "Alex sees the reroll after the send", "Bo opens Alex's KEEP bubble" (-11, TB_ESTAGED: the sender's own unsent link) |
| testTheKernelsSettleBlanksTumblesAndLandsOnTheView | `BeatPlayer.frame` frames every die, not only the plan's rolled ones | "a kept die never moves" |
| testTheKernelsSettleBlanksTumblesAndLandsOnTheView | `BeatPlayer.frame` drops the sample's `dy` | "dropping from above the tray" |
| testTheReceiverSeesTheSendersRerollAndCard | `BridgeKernel.view` reads a slot as filled by its score, not by the kernel's `filled` bit | "a row taken for zero is filled, not open" (nil against 0) |
| testAScoreTapStagesTheScoreAndTheRowShowsItStaged | `BridgeKernel.view` reads seat s's card from seat s + 1 | "the staged row shows its score", "sent, the score stands", "and the total with it" |
| testRowsAreThirteenInTwoHalvesWithPreviewsOnlyWhereOpen | `ScoreRow.rows` shows the preview whatever `canScore` says | "a read-only card shows no preview" |
| testAScoreTapStagesTheScoreAndTheRowShowsItStaged | `ScoreRow.rows` marks the wrong row staged (`d != c`) | "the staged row shows its score" (filled(11) against staged(11)) |
| testTheBubblePictureIsFoolishsSize | `BubbleSnapshot.size` 300 x 200 | "300 x 195 points", the pixel height (400 against 390) |
| testAKeepBubbleCarriesTheBlanks | `BubbleContent.of` paints blank dice as ones | "T11: the picture has blank slots for the rerolling dice" ([2, 3, 1, 1, 4]) |

These rows test the shell over the kernel: that the screens, the bubble picture and the tray draw what the kernel's view and plan give them and add no value of their own.
The kernel's own T11 group (`tb_test`, DECISIONS T11) and `tallybones/c/tests/MUTATIONS.md` are the proof that no draft ever yields dice.
