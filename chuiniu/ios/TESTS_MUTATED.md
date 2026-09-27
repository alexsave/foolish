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

## DiceTableLayoutTests

| # | Test | Mutation | Assertion that went red |
|---|---|---|---|
| M8 | testSeatsNeverOverlapEachOtherOrThePlate | the arc's step divides by `others + 3` | "seats 2 and 3 do not overlap" (4 seats), "seat 2 clears the bid plate" (6 seats), 65 in all |
| M9 | testSeatsNeverOverlapEachOtherOrThePlate | the horizontal radius halved | "seats 2 and 3 do not overlap" (5 seats), "seat 1 clears the bid plate", 112 in all |
| M10 | testMySeatIsTheBottomBand | the others run right to left | "the seat after mine is the leftmost" (295.9 against 179) |
| M11 | testSeatsNeverOverlapEachOtherOrThePlate | the plate sits at `0.7` of the radius instead of `0.3` | "seat 1 clears the bid plate" (2 seats), 71 in all |

## Not tested

The conversation (`ChuiniuMessages/MessagesViewController.swift`) has no test target, as pickemup's has none.
The screens and the roll were looked at once inside Messages on the same simulator, on the FakeKernel (`chuiniu/docs/shots/`).
