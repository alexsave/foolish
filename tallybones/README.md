# Tallybones

A five-dice category-scoring game for 2 to 8 in an iMessage thread: roll five dice up to three times, keep some, reroll the rest, and score one category of your card per turn.
A proof of concept.

**Read [LEGAL.md](LEGAL.md) before touching the names or the card.**

## THE NAME IS A PLACEHOLDER

This is the Yacht family, and the best-known member of it is a Hasbro trademark.
The mechanic is public domain (Yacht, 1938; Generala; Poker Dice); the branded card's layout, its words, its bonus and joker rules, its logo and its palette are not, and none of them is here (LEGAL.md).
"Tallybones" is a working title (DECISIONS T1): one string in the kernel's word table once the kernel lands, and a USPTO and App Store search before any listing is the owner's (BLOCKED).

## What exists

| Part | Where | What |
|---|---|---|
| Decisions | `docs/DECISIONS.md` | the spec: naming (T1), rules (T2 to T5), randomness and the no-preview reroll (T6, T11), the wire (T7), words and motion (T8, T9), iOS (T10, T12 to T18) |
| Legal | `LEGAL.md` | what is borrowed and what is not, and the never / always list |
| iOS | `ios/` | `TallybonesKit` (dice tray, drawn dice, tumble, scorecard, seat badges, lobby, bubble picture), `TallybonesMessages` (the extension), `TallybonesMessagesApp` (the container) |
| Kernel | `c/` (another branch) | NOT HERE YET: the C kernel and its bridge are written separately; the app runs on a local stand-in behind one protocol until they are wired (`ios/README.md`, "Wiring the kernel") |
| CI | `../.github/workflows/tallybones.yml` | `make -C c run` and `make -C c asan` on Linux, red until the kernel merges |

## Build and test

```
cd ios && xcodegen generate
xcodebuild -project Tallybones.xcodeproj -scheme TallybonesMessagesApp -destination 'generic/platform=iOS Simulator' build
ios/scripts/mac_tests.sh                                                  xcodegen, the tests, the shipping build (a simulator)
DEST='platform=macOS,variant=Mac Catalyst' ios/scripts/mac_tests.sh unit  the tests with no simulator free (T18)
```

After any xcodegen run, `git status --short -- '*.entitlements'` must be empty.

## What is verified, and how

- **The shipping build**: `TallybonesMessagesApp` builds for the iOS Simulator SDK with warnings as errors, on the stand-in kernel.
- **The Swift tests**: `TallybonesKitTests`, 8 tests, 0 failures, RUN as a Mac Catalyst bundle on 2026-09-27; they also build for testing for the iOS Simulator but have not run on one, because both simulator slots on the build Mac were taken.
- **Every Swift test was seen red**: ten mutants, each with the assertion it failed on, in `ios/TESTS_MUTATED.md`.
- **The screens, as pictures**: the table, a staged KEEP with its blank dice, the card with a staged score, the lobby and the bubble picture were rendered with `ImageRenderer` on the Mac and looked at; not yet inside Messages.

## What is NOT verified yet

- Anything with the real kernel: every die, score and caption on screen today is the stand-in's.
- The extension inside Messages on a simulator or a phone: no screenshot, no send, no receive.
- The conversation layer, `ios/TallybonesMessages/MessagesViewController.swift`, which has no test target and has never run.
- The tumble on a device, filmed and measured.

## Docs

| File | What it is for |
|---|---|
| [LEGAL.md](LEGAL.md) | the trademark research and the never / always list |
| [docs/DECISIONS.md](docs/DECISIONS.md) | every decision, T1 onward, and BLOCKED |
| [ios/README.md](ios/README.md) | the app's build, test, rig, file map, and what the kernel integration adds |
| [ios/TESTS_MUTATED.md](ios/TESTS_MUTATED.md) | each Swift test's mutation and the assertion that went red |
