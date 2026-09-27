# Pick 'Em Up

A shedding game for 2 to 8 in an iMessage thread: match the top card by suit or number, play your action cards, and get rid of your hand.

The kernel, its wire and its iOS bridge are built and tested.
The Messages extension and its container are built, and most of their Swift tests have run on a simulator, but the extension has **not yet been seen inside Messages**: every simulator on the build Mac stopped booting before that step (BLOCKED B2 and B3 in `docs/ORCHESTRATION.md`).
`docs/DECISIONS.md` indexes every decision taken on the owner's behalf, and which ones are flagged for a possible veto.

Commands below run from inside `pickemup/`.

**Read [LEGAL.md](LEGAL.md) before touching the art.**
It is the UYES post-mortem and the list of things that get a clone removed.

## THE NAME IS A PLACEHOLDER, AND THE REASON MATTERS

This is the shape of UNO, and **UNO is a Mattel trademark** - filed 1974, registered 1975, and enforced recently enough that a clone called UYES was pulled from Google Play by a DMCA takedown.

What that does and does not cover:

- **Not protectable: the mechanics.**
  Shedding, wilds, draw-twos, reverse, skip, and a penalty for failing to announce your last card.
  This is the Crazy Eights family and Crazy Eights is public domain.
- **Protected: the name, the card art, the four-colour-only suits, the oval, and the call-out word.**
  None of those are here and none of them can be.

So: **suits are shape AND colour** - circle/teal, triangle/amber, square/violet, diamond/slate.
That keeps us clear of the trade dress, and it is also the only version a colourblind player can read, which is the better reason.
Action cards carry their suit's shape in two corners as well as its colour (O6), for the same reason.
The call-out word is "Last card!" (D2), and the deck is 104 cards with no 0 (D1).
The mark appears nowhere in the product: `c/tests/pk_say_test.c` fails if any sentence the kernel can say contains it, and this file, LEGAL.md and that guard are the only places it is written.

`Pick 'Em Up` is a working title and **it already collides**: a shedding game of that name exists on TheGameCrafter, plus a *Pick 'Em Up Bitch*.
Neither looks registered and neither is Mattel, so the risk is common-law and small - but it is the same genre.
The name is one `GAME_NAME` string in the kernel's word table (O5, D38), so a rename is one line.
Search USPTO and decide before any store listing; that choice is the owner's (BLOCKED in `docs/ORCHESTRATION.md`).

## What exists

| Part | Where | What |
|---|---|---|
| Surface study | `docs/UI.html` | every screen at foolish's materials, the motion grid, the rulers; `UI_DECISIONS.md` is its record |
| Rules | `docs/RULES_AND_KERNEL.md` | the rules, the kernel design, the wire and the test plan, with decisions D1 to D57 |
| Kernel | `c/src/pk*.{c,h}` | deal, legality, apply, seal, undo by replay, the masked view, the animation plan, the lobby, the words; plain C11, no allocation, builds for wasm32 and iOS |
| Wire | `c/src/pk_code.c`, `c/src/pk_msg.c` | every bubble carries the whole game as seed + roster + one mixed-radix number (D25), base32 in `MSMessage.url`; Rule P settles races; the seat resolver |
| Timeline | `c/src/pk_beats.{c,h}` | a plan's events laid out as beats on `UI.html`'s clock, and the frame at any millisecond (A1) |
| Bridge | `c/ios/pk_api.c`, `c/ios/pk_lay.c` | the one header Swift sees (`pk_api.h`, module `CPickemup`): the resident message, staging, reads, words, beats, and every layout number (I1) |
| Generated Swift | `ios/Generated/` (build output) | readers written by `shared/tools/structgen` and strings by `datagen`, stamped with the layout hash (D46) |
| iOS | `ios/` | `PickemupKit` (the table, hand, deck and pile, seat badges, suit picker, lobby, end reveal, bubble picture, `BeatPlayer`), `PickemupMessages` (the extension), `PickemupMessagesApp` (the container) |

A tap is one kernel call, and Swift decides no rule, derives no layout number and composes no sentence (`ios/README.md`).
The board plays the kernel's beats: `BeatPlayer` samples `pk_beats_frame` every frame and holds no duration, curve or order of its own (A1).

## Build and test

```
make -C c run             every C test and the bridge smoke (about a minute)
make -C c asan            the same under ASan + UBSan
make -C c wasm            the kernel as freestanding wasm32 objects (WASM_CC=/opt/homebrew/opt/llvm/bin/clang on a Mac)
make -C c ios-lib         ios/vendor/Pickemup.xcframework and ios/Generated/ (Xcode)
make -C c swift-smoke     the bridge driven from Swift through the generated readers (a Mac)
make -C c beats-dump      the two takes of docs/MOTION_REPORT.md as tables
python3 ../shared/tools/check_ui_doc.py docs/UI.html

cd ios && xcodegen generate
xcodebuild -project Pickemup.xcodeproj -scheme PickemupMessagesApp -destination 'generic/platform=iOS Simulator' build
ios/scripts/mac_tests.sh  ios-lib, xcodegen, PickemupKitTests, the shipping build (needs a working simulator)
```

After any xcodegen run, `git status --short -- '*.entitlements'` must be empty (I23).
CI: `.github/workflows/pickemup.yml` runs `run`, `asan`, `structgen` and `datagen` on Linux; not `wasm` (D49) and not the Xcode half.
`c/README.md` has every target, the file map and the measured sizes; `ios/README.md` has the app's build, the rig and the file map.

## What is verified, and how

- **The C suites**, from `make -C c run` on 2026-09-27: `pk_test` 11,089 assertions, `pk_rules_test` 199, `pk_plan_test` 1,193,089, `pk_say_test` 55,579, `pk_fuzz` 3,921,125 over 2,800 games, `pk_msg_test` 298,141, `pk_twophone_test` 2,409, `pk_beats_test` 225 and the bridge smoke 1,207 checks, all 0 failed; the same under ASan + UBSan; `swift-smoke` 25 checks.
- **Every C test was seen red**: each has the mutation that turned it red and the assertion that failed in `c/tests/MUTATIONS.md`.
- **The two-phone game**: `c/tests/pk_twophone_test.c` plays the whole `docs/SIM_VERIFICATION.md` game phone to phone through the entry points Swift calls, and names each step beside its assertions, so every step of that game is proven in the kernel and the bridge, though not in pixels.
- **The wire fits**: the largest bubble in `make run`'s games is 893 characters (8 players), and the analytic worst case of any game is 4,720 characters, asserted at compile time against the 5,000 of `MSMessage.url` (`c/README.md`, RULES_AND_KERNEL 4.5).
- **Swift tests that ran on a simulator before the hang**, each mutation-checked there (`ios/TESTS_MUTATED.md`): `TableModelTests` (10), `LayoutTests` (5), `CardFaceTests` (1) and `RenderTests` (1) on one iPhone 17e, and `BeatPlayerTests` (8) beside `TableModelTests` on another.
- **Swift tests that only compile**: `ActionCardCornerTests` (O6, I27) has never run and has no red run, so it proves nothing yet; and the 26 tests of `PickemupKitTests` have never run together as one scheme.
- **The shipping build**: `PickemupMessagesApp` builds for the iOS Simulator SDK; `PickemupKitTests` builds for testing.

## What is NOT verified yet

- The extension inside Messages: no screenshot of any screen exists, and every step of `docs/SIM_VERIFICATION.md` owes its screenshot (B2).
- The conversation layer, `PickemupMessages/MessagesViewController.swift` (stage, send, cancel, receive), which has no test target and has never run.
- `ios/scripts/mac_tests.sh` counts, and the red run of `ActionCardCornerTests`.
- The filmed and measured animation take (a live arrival with draws, a reshuffle and a play, and a deal); `docs/MOTION_REPORT.md` gives both as the kernel's timeline instead (B3).
- On a real phone, that dragging a card down off the deck never collapses the Messages drawer (U24, I9, I36), and that a superseded stage never inserts (I35).
- foolish's own P8 simulator run after the S1 lift (B1, `docs/REUSE_AUDIT.md` S1).

## The layout is foolish's, minus three things

Measured out of the shipped Swift, not approximated; `docs/UI.html` redraws it at the same numbers, and `c/ios/pk_lay.c` holds the numbers the screens draw with.

- **The flipped trump is gone**, and with it foolish's landscape deck well in the top-left corner.
  The deck is a portrait stack of 50 x 70 backs 10pt left of the pile, with its count on the top layer (U3, D22), and the freed corner holds the status line (U4).
- **The top-right discard pile is gone.**
  This game has one discard and it is the pile you match against, so it belongs in the middle.
  The freed corner holds the direction word, hidden at two players (D13, D18).
- **The battle grid is replaced by one big pile**, centred in the board rect exactly as the grid was, and lifted 24pt in the 340pt drawer to clear the pills (U2).

Kept at foolish's values: the felt, card faces, fern back and wood (U1), the board rect inset 8/8/14/4, seats on a 0.42 x 0.35 ellipse, badges ~97pt tall, a flat hand of 72pt cards with 4pt gaps, and 96 x 40 pills 16 from the board edge.
The pills stand in a row rather than foolish's column, with Draw always in the trailing slot (U9), and the left side has one square, the rulebook, which the Last card! pill replaces while a player is exposed (U11, I14).

**The role row becomes the stamp slot.**
foolish reserves 40pt under every badge for a shield or a sword.
This game has no roles, so that 40pt holds only a player's own speech or a verdict: LAST, Caught you!, Wrong call, OUT (U12), and the badge keeps its exact height.

**Hands grow here, and that was settled.**
A shedding hand grows when you are losing, and at thirteen cards a flat card is 23.2pt, under the 40pt thin-face line.
The expanded hand goes one row, then foolish's two rows, then overlaps to a 16pt strip per card, then scrolls (O4); the drawer keeps one row (U7, I12); overlapped cards keep a full face so each strip shows its corner (U8); and a scrolling hand plays by tap + Play only (I11).
No hand is ever capped (D23).
You can drag a card sideways to rearrange your own hand, as in foolish (O9): the order is your phone's alone and never sent, new cards still arrive on the right, and a scrolling hand is not rearranged (I38).

## Why it suits a transcript

- **Hidden hands are the masking kernel's home turf**, and the deck is seed-derived, so the state is small.
- **2 to 8, and genuinely good at 2**, which the three parked forks were not.
- **The settlement half is unusually large.**
  One card can skip a player, turn the table around and hand somebody two cards, so far more happens at Send than in foolish; the channel grid in the study is where that is worked out.
- **The conflict channel is real**, unlike Ultimate Tic-Tac-Toe: turn order is strict, but the player before you can undo and replay, a catch can arrive out of turn, and races between two bubbles on one parent are settled by one total order in C (D26).

## The call-out, settled

The study asked whether the app announces one card left or somebody has to catch it.
An app cannot forget, so announcing it would delete the mechanic; the app says nothing, and no card count is ever shown (D22).
A player left on one card may say "Last card!" only in a later message (D3), and any other player may tap that player's fan to stage "Caught you!", in or out of turn (D5).
The window is not "one bubble" as first drawn but closes at the end of the next message that completes a turn, so a stray catch or say-it cannot close it on somebody else (D4).
A right catch costs the caught player 2 cards, a wrong one costs the catcher 1 (D5b), and a catch is judged against the table at the start of the catcher's message and dealt at its end, so tapping it reveals nothing (D5d).

## Design docs

Every file under `docs/`, and the other records beside the code.

| File | What it is for |
|---|---|
| [LEGAL.md](LEGAL.md) | the trademark and copyright research, and the never / always list |
| [docs/DECISIONS.md](docs/DECISIONS.md) | start here to veto: one row per decision (D, U, I, A, O), the flagged ones, and a pointer to BLOCKED |
| [docs/ORCHESTRATION.md](docs/ORCHESTRATION.md) | the orchestration decisions O1 to O9, the current state of every BLOCKED item and what the owner does about it, found on the way, the final check |
| [docs/RULES_AND_KERNEL.md](docs/RULES_AND_KERNEL.md) | the rules of play, the kernel and wire design, the test plan, and the rules decisions D1 to D57 |
| [docs/UI.html](docs/UI.html) | the surface study: every screen at foolish's materials, the motion grid and the rulers |
| [docs/UI_DECISIONS.md](docs/UI_DECISIONS.md) | the visual decisions U1 to U25 taken in the study |
| [docs/IOS_DECISIONS.md](docs/IOS_DECISIONS.md) | the iOS decisions I1 to I39, including the architecture review |
| [docs/ANIMATION_DECISIONS.md](docs/ANIMATION_DECISIONS.md) | the motion decisions A1 to A17 behind the kernel's timeline |
| [docs/REUSE_AUDIT.md](docs/REUSE_AUDIT.md) | what is reused from foolish and uttt, the lift plan S0 to S16 and its proofs |
| [docs/SIM_VERIFICATION.md](docs/SIM_VERIFICATION.md) | the two-seat game to play in Messages, the screenshot each step owes, and the bridge assertion that proves it |
| [docs/MOTION_REPORT.md](docs/MOTION_REPORT.md) | the two takes as the kernel's timeline, against the grid's budgets |
| [c/README.md](c/README.md), [c/tests/MUTATIONS.md](c/tests/MUTATIONS.md) | the kernel's targets, file map, measurements and mutation rows |
| [ios/README.md](ios/README.md), [ios/TESTS_MUTATED.md](ios/TESTS_MUTATED.md) | the app's build, rig and file map, and its mutation rows |
