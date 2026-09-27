# TallybonesKitTests, every test seen red

Each row is one mutant, applied alone by a script (an exact string replacement that must match once, the source restored and checked byte for byte by SHA-256 afterwards), and the full `TallybonesKitTests` scheme run.
Run 2026-09-27 on the commit that added this file.

**Where they ran: Mac Catalyst, not an iOS simulator.**
Both simulator slots on the build Mac were taken by other agents' simulators, so none was booted for this (the two-simulator rule).
TallybonesKit imports no Messages, so the scheme builds as a Mac Catalyst test bundle with command-line overrides only (`DEST='platform=macOS,variant=Mac Catalyst' scripts/mac_tests.sh unit`, which adds `SUPPORTS_MACCATALYST=YES` and ad-hoc signing).
The same bundle also builds for testing for `generic/platform=iOS Simulator`; it has not yet run on one.
The unmutated suite is 8 tests, 0 failures.
Every mutant compiled, and every one went red on the assertion it was aimed at.

| Test | Mutation | Assertion that went red |
|---|---|---|
| testEveryValueHasThatManyPipsAndUnknownHasNone | `DiceFace.pips(5)` drops the centre pip | "pips per value 0...6" ([0, 1, 2, 3, 4, 4, 6]) |
| testATapTogglesTheKeepMarkAndASecondTapClearsIt | `TallyTable.toggle` sets the mark instead of toggling it | "a second tap on die 2 clears it", "a die that is not there changes nothing" |
| testAStagedKeepShowsBlanksUntilItIsSentThenTumblesIn | `TallyTable.toggle` drops its `canKeep` guard | "the refused tap left no mark behind", "exactly the rerolled dice tumble" ([3] against [2, 3]) |
| testAStagedKeepShowsBlanksUntilItIsSentThenTumblesIn, testAKeepBubbleCarriesTheBlanks | the stand-in's draft view derives the reroll (`shownDice[i] = h2[i]` for `0`): T11's guard gone | "T11: the rerolling dice are blank while staged", "T11: the picture has blank slots for the rerolling dice" ([3, 3, 5, 5, 3]) |
| testAStagedKeepShowsBlanksUntilItIsSentThenTumblesIn | `TallyTable.refresh` tumbles kept dice too (`newRoll` without `!kept[i]`) | "exactly the rerolled dice tumble" (all five against [2, 3]) |
| testTheTumbleTurnsSwellsFlickersAndLandsAtRest | the flicker runs to 100% instead of 70% | "the landed face shows from 70%" |
| testRowsAreThirteenInTwoHalvesWithPreviewsOnlyWhereOpen | `ScoreRow.rows` shows the preview whatever `canScore` says | "a read-only card shows no preview" |
| testAScoreTapStagesTheScoreAndTheRowShowsItStaged | `ScoreRow.rows` marks the wrong row staged (`d != c`) | "the staged row shows its score" (filled(16) against staged(16)) |
| testTheBubblePictureIsFoolishsSize | `BubbleSnapshot.size` 300 x 200 | "300 x 195 points", the pixel height (400 against 390) |
| testAKeepBubbleCarriesTheBlanks | `BubbleContent.of` paints blank dice as ones | "T11: the picture has blank slots for the rerolling dice" ([3, 3, 1, 1, 3]) |

The stand-in's T11 row tests the stand-in, not the kernel: it proves the screens and the bubble picture draw what the view gives them and add no value of their own.
The kernel's own T11 group (`tb_test`, DECISIONS T11) is the proof that no draft ever yields dice.
