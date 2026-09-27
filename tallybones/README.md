# Tallybones

A five-dice category-scoring game for 2 to 8 in an iMessage thread: roll five dice up to three times, keep some, reroll the rest, and score one category of your card per turn.
A proof of concept.

**Read [LEGAL.md](LEGAL.md) before touching the names or the card.**

## THE NAME IS A PLACEHOLDER

This is the Yacht family, and the best-known member of it is a Hasbro trademark.
The mechanic is public domain (Yacht, 1938; Generala; Poker Dice); the branded card's layout, its words, its bonus and joker rules, its logo and its palette are not, and none of them is here (LEGAL.md).
"Tallybones" is a working title (DECISIONS T1): one string in the kernel's word table (GAME_NAME), and a USPTO and App Store search before any listing is the owner's (BLOCKED).

## What exists

| Part | Where | What |
|---|---|---|
| Decisions | `docs/DECISIONS.md` | the spec: naming (T1), rules (T2 to T5), randomness and the no-preview reroll (T6, T11), the wire (T7), words and motion (T8, T9), iOS (T10, T50 to T56) |
| Legal | `LEGAL.md` | what is borrowed and what is not, and the never / always list |
| iOS | `ios/` | `TallybonesKit` (dice tray, drawn dice, the kernel's settle, scorecard, seat badges, lobby, bubble picture, `BridgeKernel` over the kernel), `TallybonesMessages` (the extension), `TallybonesMessagesApp` (the container) |
| Kernel | `c/` | the rules, the wire, the words, the beats and the bridge (`c/README.md`); the app links it as `vendor/Tallybones.xcframework` |
| Bot | `c/bot/` | an exact expected-value solver, an evaluation tool only (T30 to T35) |
| CI | `../.github/workflows/tallybones.yml` | `make -C c run` and `make -C c asan` on Linux |

## Build and test

```
make -C c ios-lib
cd ios && xcodegen generate
xcodebuild -project Tallybones.xcodeproj -scheme TallybonesMessagesApp -destination 'generic/platform=iOS Simulator' build
ios/scripts/mac_tests.sh                                                  xcodegen, the tests, the shipping build (a simulator)
DEST='platform=macOS,variant=Mac Catalyst' ios/scripts/mac_tests.sh unit  the tests with no simulator free (T56, T63)
```

After any xcodegen run, `git status --short -- '*.entitlements'` must be empty.

## What is verified, and how

- **The kernel**: `make -C c run`, every group 0 failed (tb_test 6,529, tb_msg_test 29,319, tb_fuzz 16,424,832, tb_say_test 344,338, tb_beats_test 459,317, tb_twophone_test 458, the C smoke 67), and `make -C c swift-smoke` 28 checks from Swift; each test's mutation is in `c/tests/MUTATIONS.md`.
- **The shipping build ON THE REAL KERNEL**: `TallybonesMessagesApp` builds for the iOS Simulator SDK with warnings as errors, linking `Tallybones.xcframework` and the generated readers; the stand-in is deleted.
- **The Swift tests over the real kernel**: `TallybonesKitTests`, 9 tests, 0 failures, RUN as a Mac Catalyst bundle on 2026-09-27 (both simulator slots were taken while they ran); they also build for testing for the iOS Simulator. Two phones play in one process (`TallybonesKitTests/Phones.swift`), and a receiver's dice are asserted equal to the sender's.
- **Every Swift test was seen red**: fourteen mutants, each with the assertion it failed on, in `ios/TESTS_MUTATED.md`.
- **Inside Messages on an iOS 27 simulator** (`docs/SIM_VERIFICATION.md`, screenshots in `docs/shots/`): Alex's first turn in each of two games, played on both sides, one simulator standing in for two people through the DEBUG `dev.who` file (T65): the invitation, Bo's join that starts the game, roll 1 derived and identical on both sides, keep marks, a staged KEEP with blank dice in the tray and in the bubble, the send, the reroll appearing after the send, a staged SCORE, its send, the card and the total updating, the turn passing, and the receiver opening the bubble to the same card, total and next roll.

## What is NOT verified yet

- Two real devices, or two simulators: the two sides above are one simulator, one Messages identity, with the identity swapped by a DEBUG file between drawer openings. Real `localParticipantIdentifier`s and the sender fact were not exercised by the simulator run (the Swift and C two-phone tests do exercise the resolver).
- A whole game to the end in Messages (the C two-phone test plays one to the end), a LEAVE bubble, a group of three or more, and a race between two bubbles.
- The settle filmed and measured frame by frame; the screenshots caught the tray before and after, not mid-flight.
- The kernel's STAMP, TURN and FADE beats: they are laid out but the Swift side draws only the dice settle (T62).
- Dark mode, VoiceOver, a 4.7" phone, and the expanded card at the 8-seat width.

## Docs

| File | What it is for |
|---|---|
| [LEGAL.md](LEGAL.md) | the trademark research and the never / always list |
| [docs/DECISIONS.md](docs/DECISIONS.md) | every decision, T1 onward, and BLOCKED |
| [ios/README.md](ios/README.md) | the app's build, test, rig, file map, and what the kernel integration adds |
| [ios/TESTS_MUTATED.md](ios/TESTS_MUTATED.md) | each Swift test's mutation and the assertion that went red |
| [docs/SIM_VERIFICATION.md](docs/SIM_VERIFICATION.md) | the run inside Messages: commands, device, each step seen and what was not |
