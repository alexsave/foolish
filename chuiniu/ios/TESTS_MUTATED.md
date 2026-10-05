# ChuiniuKitTests, every test seen red

Each row is one mutant, applied alone by a throwaway driver (an exact string replacement, the source restored and checked byte for byte afterwards), and the full `ChuiniuKitTests` scheme run on a private iPhone 17e simulator (iOS 27.0), shut down and deleted afterwards.
Every run executed all 9 tests; a row lists the assertion that went red, always in the test the mutant was aimed at.
Run 2026-09-27 on the commit that added this file.
The unmutated suite is 9 tests, 0 failures.

The first unmutated run was red on its own: at 358 x 380 the 80pt badges overlapped at six seats and the side seats crossed the 180pt bid plate.
The badge is now 72pt wide and the plate 160pt, and that run is what M8, M9 and M11 re-create.

## DieTests

| # | Test | Mutation | Assertion that went red |
|---|---|---|---|
| M1 | testEveryFaceHasAsManyPipsAsItsValue | face 6 drops its right middle pip | "face 6 shows 6 pips" (5 against 6) |
| M2 | testPipsSitOnTheFaceAndNeverTouch | `pipOffset` 0.25 becomes 0.1 | "face 3: pips 0 and 1 do not touch" (0.141 against 0.2), 23 in all |
| M3 | testAnUnknownFaceIsBlank | the default face draws a centre pip | "0 is blank", "7 is blank" |

## BidPickerTests

| # | Test | Mutation | Assertion that went red |
|---|---|---|---|
| M4 | testRaiseIsOffBelowTheInjectedMinimumAndOnAtIt | `raiseEnabled` allows `least - 1` | "four 3s again is below the minimum", "four 2s ...", "three 6s ..." |
| M5 | testRaiseIsOffBelowTheInjectedMinimumAndOnAtIt | `raiseEnabled` drops the `maxQuantity` bound | "past every die is not" |
| M6 | testCallFollowsTheMenu | `callEnabled` always true | "nothing to call before the opening bid" |
| M7 | testTheStepperSpansTheLeastQuantityToEveryDie | `quantityRange` takes the `max` of the faces | "least of the faces to the table" (5...20 against 4...20) |

## DiceTableLayoutTests (retired 2026-10-05, package E1)

`DiceTableLayout` and its test are deleted: the seats, the plate, the shelf and the short board are the kernel's (`cn_lay`, pinned by `cn_lay_test.c` to the study's own numbers), and Swift places nothing.
Rows M8 to M11, M18 and M19 recorded mutants of a function that no longer exists.

## BridgeKernelTests (the tie-together, 2026-09-27)

Driven by a throwaway Python driver on the same simulator model (iPhone 17e, iOS 27.0, `6E0A730D`): each mutant an exact string replacement in the source, the full `ChuiniuKitTests` scheme run (12 tests executed every time, none a compile error), the file put back and compared byte for byte.
The unmutated suite is 12 tests, 0 failures.

| # | Test | Mutation | Assertion that went red |
|---|---|---|---|
| M12 | testAFaceWithNoRaiseLeftIsAboveTheStepper | `BridgeKernel` passes the kernel's 0 through instead of `maxQuantity + 1` | "only ten 6s is left" (`[0, 0, 0, 0, 10]`), "ten 4s is no raise" |
| M13 | testARaiseACallAndTheRevealMapIntoTheModel | the reveal's slice starts one die late (`s * stride + 1`) | "the reveal shows the round's dice" |
| M14 | testARaiseACallAndTheRevealMapIntoTheModel | `sent` cancels the staged move instead of committing it | the bid after the send, "every cup lifts once it is sent" |
| M15 | testARaiseACallAndTheRevealMapIntoTheModel | the staged bid takes the face as its quantity | `stagedBid`, "two 4s" (read "four 2s") |
| M16 | testARaiseACallAndTheRevealMapIntoTheModel | `isNewer` hands `cn_api_prefer` its links the other way round | "the kernel's ranking" |
| M17 | testARaiseACallAndTheRevealMapIntoTheModel | a lobby seat's name is the lobby row | "the name a bubble shows never says You" |
| M20 | testARaiseACallAndTheRevealMapIntoTheModel | Next round's look is ignored (phase always `.revealed`) | "the next round's table" |

`testTheLayoutOfTheReadersIsTheLibrarys` is a startup check (`cn_api_layout_hash` against `SG_LAYOUT_HASH`); it was not mutated, because a mismatched pair is a rebuilt library or a regenerated reader, not a source edit.

## StageViewTests (package E1, 2026-10-05)

The host side of the kernel's stage, on real group games played into the bridge.
Each mutant was applied by `~/.claude/skills/ios-sim-verify/mutation_check.sh` (exact replacement, restored and verified byte for byte), `-only-testing:ChuiniuKitTests/StageViewTests`, on a private iPhone 17e simulator (`EA27629F`, iOS 27.0); the driver was a Python script, never a shell loop.
The unmutated suite is 16 tests, 0 failures.
These tests were written after the code, so the mutants are their red runs.

| # | Test | Mutation | Assertion that went red |
|---|---|---|---|
| M21 | testTheTurnedLayerMapsEveryPointAsTheKernelsCamera | `tilt` reads `ca` (about the origin) instead of `ca_screen` | "390x340: (0.0, 0.0) lands at x the kernel's camera says" (0 against -1.24), every size |
| M22 | testTheTurnedLayerMapsEveryPointAsTheKernelsCamera | `m23` and `m32` swapped | SURVIVED: they act on z only, and every painted point is flat (z 0); replaced by M22b |
| M22b | testTheTurnedLayerMapsEveryPointAsTheKernelsCamera | `m24` negated (the turn's sign on the y-to-w term, what a wrong rotation sign does to a flat point) | "390x718: (0.0, 718.0) lands at x" (-19.95 against -39.21), every size |
| M23 | testAViewWithNoSizeNeverAsksForAFrame | `begin` drops its no-size guard | "nothing begun for a drawer with no size" (2 against 0) |
| M24 | testAViewWithNoSizeNeverAsksForAFrame | `wake` drops its no-size guard | "a view shrunk to nothing asks for no frame" (2 against 1) |
| M25 | testMyCupsEllipseIsTheOneTapAndItTipsTheCupByTheKernelsEase | the ellipse test is `<= 1.2` | "just past its edge", "just above it" |
| M26 | testMyCupsEllipseIsTheOneTapAndItTipsTheCupByTheKernelsEase | the peek tween is linear, not `cn_api_peek_ease` | "halfway, the kernel's ease" (0.50 against 0.89) |
| M27 | testTheThrowsClockIsTheStudysAndNothingIsStagedBeforeMyDiceRest | dt clamped to 0.1 | "dt clamped to .05" (100 against 50) |
| M28 | testTheThrowsClockIsTheStudysAndNothingIsStagedBeforeMyDiceRest | at rest from `roll_at_ms`, not `rest_ms` | "my dice are in the air", "just before rest_ms nothing may be staged" |
| M29 | testTheThrowsClockIsTheStudysAndNothingIsStagedBeforeMyDiceRest | the tap ignores `atRest` | "no peek while they are" |
| M30 | testTheFrameInBandsIsTheFrameOnOneThread | `BridgeStage.frame` draws `bands - 1` bands (a Kernel file, mutated for the check only and restored) | "at 1200 ms, peek 0.0: the bands' bytes are one thread's", and at 4052 ms peeking |

## The bubble and the arena (package E2, 2026-10-05)

`testTheBubbleIsTheStagesFreesTheArenaAndPutsTheTableBack`, on a private iPhone 17e (iOS 27.0, `C6058B4C`), `-only-testing:ChuiniuKitTests/BridgeKernelTests` (5 tests executed every run, none a compile error).
Each mutant was one edit by hand, run, then edited back; `git diff` showed the source unchanged afterwards.

| # | Mutation | Assertion that went red |
|---|---|---|
| E1 | `BridgeStage.bubble` does not begin the table on show again | `:280` "the table on show draws its own picture again" (936000 bytes against 2312640) |
| E2 | `BridgeStage.bubble` does not purge | `:277` "no arena after the bubble", `:285` |
| E3 | `BridgeStage.begin` takes an arena (`ready(arena: true)`) | `:266` "a begin takes no arena", then `:277`, `:285` |
| E4 | `BubbleSnapshot.render` asks the stage only when the game is over | `:274` "the stage drew the picture, once" (0 against 1), `:276` the scale (3 against 2), `:277` |

## lint_architecture.sh (package E2, 2026-10-05)

Each mutant was one edit by hand to a product file or to the lint, the lint run, the edit taken back, and the lint clean again.

| # | Mutation | What the lint said |
|---|---|---|
| L1 | `MessagesViewController.swift` gains `d.withUnsafeBytes { $0.load(fromByteOffset: 2, as: UInt16.self) }` | `ChuiniuMessages/MessagesViewController.swift:39: withUnsafeBytes` and `: load(fromByteOffset` |
| L2 | `KernelSeam.swift` imports `CChuiniu` | `ChuiniuKit/Kernel/KernelSeam.swift:16: import CChuiniu (only BridgeKernel.swift may)` |
| L3 | `KernelSeam.swift` declares `struct MutantView { var bidQ: Int; let bidF: Int }` | `ChuiniuKit/Kernel/KernelSeam.swift:18: struct MutantView declares bidQ, bidF` |
| L4 | `BridgeKernel.swift` spells a bid `b.quantity << 8 \| b.face` | first SURVIVED: `grep -q` closed the pipe early and `pipefail` read the SIGPIPE as "this file does not reach the kernel". The detector now reads a here-string; then `ChuiniuKit/Kernel/BridgeKernel.swift:238: shift << 8 on a file that reaches the kernel` |
| L5 | `ALLOWED` gains `ChuiniuKit/Kernel/BridgeKernel.swift\|bytes` (with L4 in place) | the shift was let through and `ALLOWED has 1 entries; it only shrinks, and it is empty: fix the code` |
| L6 | the detector loses `withUnsafeBytes` from its tokens | `detector self-test: four byte reads (found 3, want 4)` |

Before the lint went in it found three hand byte reads, all removed: `BridgeKernel.swift` handed the seat records and the participant id to C through `withUnsafeBytes` and `bindMemory` (now a `[UInt8]` argument), `MessagesViewController.swift` read the participant UUID with `withUnsafeBytes(of:)` (now its sixteen bytes spelled out), and `BridgeKernelTests.swift` imported `CChuiniu` for the tests-only view (now `BridgeKernel.everyonesView()`, Debug only).

## The baked planks (package T, 2026-10-05)

`chuiniu/c/tests/cn_tex_test.c`, "planks: six planks, seamless, a running bond, nails, the deep palette", run by `make -C chuiniu/c tex-test` (402 assertions green before and after).
Each mutant was one edit by hand to `tools/cn_texgen.c`, the test run, the edit taken back by re-editing, and the test green again.

| # | Mutation | Assertion that went red |
|---|---|---|
| T1 | the gap pass reads `u0 * 5`, not six planks | `:254` "plank edge 1 (x 172) is not a gap" (and edges 4 and 5), `:258` "plank 1's middle is dark" |
| T2 | no running bond: `shift = (pi % 2) * .0` | `:272` "plank 1 ends at row 0, not 830" (and 3 and 5) |
| T3 | the texel's v is `(y + .5) / (H - 40)` (the tile no longer wraps) | `:266` "the top and bottom edges part: 32.25 a texel against 1.07 inside", and `:272` |
| T4 | the march's palette warm (base 16 10 6, gains 14 9 5: foolish's walnut direction) | `:280` "the planks lean warm: 26.5 19.1 12.1" |
| T5 | one nail a plank, not one inside each edge | `:299` "48 nail heads, 48 missing" |
| T6 | the crust draws no wrapped copies (`w < 1` for the patches and the shells) | `:333` "left and right edges part more than the study's: 111.66 against 5.31", `:334` "top and bottom edges part: 114.62 against 5.38" |
| T7 | no shells (`per = 0`) | `:323` "only 0 solid texels: the shells are missing", and `:333` |

## Not tested

The conversation (`ChuiniuMessages/MessagesViewController.swift`) has no test target, as pickemup's has none.
The screens and the conversation were played inside Messages on the real kernel, a round and a call step by step and a whole game to a winner (`chuiniu/docs/SIM_VERIFICATION.md`); that is looked at, not asserted.
`RevealScreen`'s use of the kernel's frame (I10) has no test of its own; `revealMotion` is asserted only at 0 ms (cups down) and at 60 s (done).
`StageUIView`'s paint (the planks, the names, the canvas layer) is looked at on the simulator, not asserted, beyond the canvas frame in `testAViewWithNoSizeNeverAsksForAFrame`.
