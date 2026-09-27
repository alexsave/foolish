# Pick 'Em Up - orchestration log

This file is the orchestrator's own log: the decisions it took on the owner's behalf that no single design document owns, and the order the work landed in.
Rule decisions live in `RULES_AND_KERNEL.md`, visual decisions in `UI_DECISIONS.md`, and the reuse plan in `REUSE_AUDIT.md`.
The owner can veto any DECISION here individually.

## Decisions

DECISION O1: the Messages extension is SwiftUI, built by copying foolish's card, hand, badge, deck-well and flight views (each copy carries a `COPIED from` header so the lift step that owns it can delete it).
Alternative: Core Animation with C-drawn polygons, the way uttt draws its board and never loads SwiftUI in the extension.
Why: the owner asked for a LOT of foolish to be reused (dragging, hand ordering, table layout, felt, animations) and every one of those lives in foolish's SwiftUI views.
Copying them is the only route that keeps foolish byte-identical while the lifts into `shared/` happen one reviewed step at a time.
The cost is uttt's extension-launch-weight argument; foolish already ships this weight with real users, so it is accepted.
Confidence: high.

DECISION O2: naming.
C prefix `pk_`, kernel in `pickemup/c/src/pk*.{c,h}` laid out as `RULES_AND_KERNEL.md` section 3.1 says, tests in `pickemup/c/tests/`, iOS bridge in `pickemup/c/ios/`, Makefile targets `run`, `asan`, `ios-lib`, `ios-smoke` as in uttt.
`make -C c ios-lib` writes `pickemup/ios/vendor/Pickemup.xcframework`.
Swift targets `PickemupKit`, `PickemupMessages`, `PickemupMessagesApp`, project `pickemup/ios/Pickemup.xcodeproj` from `pickemup/ios/project.yml`.
Alternative: foolish's names (`msg_*`, FoolishKit shape).
Why: uttt is the freshest second-product template and its shape is the one `shared/tools` already drives.
Confidence: high.

DECISION O3: lifts S0, S1 and S2 from `REUSE_AUDIT.md` land before the screens; S3 (CI) lands with the kernel; every other lift is copy-first and lifted later or not at all in this pass.
Alternative: lift everything the screens need before building them.
Why: the medium and high risk lifts touch foolish's animation and wire core, and foolish has real users; a copy with a `COPIED from` header costs nothing now and is deleted by the lift that owns it.
Confidence: high.

DECISION O4: hands past thirteen cards (the rules doc's open question 2).
The hand row keeps foolish's one-row and two-row layouts up to the two-row limit, then the cards overlap within the two rows down to a 16pt visible strip per card, and past that the hand becomes a horizontally scrolling two-row strip.
The kernel keeps D23 (no hand cap); the surface never truncates a hand.
Alternative: a hand cap of 13 in the rules.
Why: the owner asked for chaotic many-draw turns, and a cap would make the deck tap stop working exactly when it is funniest.
Confidence: medium.

DECISION O5: the name stays `Pick 'Em Up` as a working title, threaded through one `GAME_NAME` string, and the collision search before a store listing is the owner's (see BLOCKED).

DECISION O6: action cards (Skip, Reverse, +2) carry their suit SHAPE as well as its colour: the shape sits small in the two corners where a number card shows its rank, and the action glyph stays in the centre.
Alternative: colour only, as the study drew them.
Why: the README's reason for shape-and-colour suits is that a colourblind player must be able to read every card, and an action card is still a suit card that has to be matched.
Confidence: high.

DECISION O7: a draw is never staged on its own; the draft opens on the first draw and stays a draft until a play or a pass closes the turn (the iOS worker's I-decision that overrides the study's "re-stage after every draw" stands).
Alternative: stage a bubble after every draw.
Why: the kernel refuses to seal a turn mid-draw (rules 1.5), and Messages cannot take a staged bubble back, so a staged half-turn would be a lie in the transcript.
Confidence: high.

DECISION O8: two pre-existing failures in foolish's own proof set were fixed on this branch rather than only reported: the UBSan left-shift overflow in `foolish/c/src/replay.c` (`replay_b32_encode`, now accumulating in `unsigned`, output bytes identical, two `i32.shr_s` opcodes become `i32.shr_u` in the uncommitted wasm build) and the `import CFoolish` lint failure in `TableWireTests.swift` (test-only, introduced by this branch's own commit 5f5211ce under Xcode 27).
Alternative: leave foolish untouched and list both under "Found on the way".
Why: the owner's rule is that lint and test failures found on the way get fixed, and both fixes leave every committed artefact and every golden byte-identical; the replay.c change is the one place this branch touches foolish's shipped C, so it is called out here for veto.
Confidence: high for the lint fix, medium for the replay.c fix only because it is shipped code.

DECISION O9: the hand keeps foolish's drag-to-reorder, overriding D24 and I10.
The wire still carries acquisition order and codes a play as a position in that order; each phone keeps its own arrangement (a permutation over the acquisition order) in the kernel, persisted in the phone's own record and never sent, and every hand read and every play goes through it in C.
Alternative: D24 as written (no rearranging, acquisition order only).
Why: the owner named "card dragging and hand ordering" as the first thing to reuse from foolish; D24's reason (a second derivation of hand order) is answered by keeping the arrangement in the kernel and off the wire, so there is one owner and no second derivation.
Confidence: high.

DECISION O10: in a drawer shorter than 340pt (the iPhone 17e compact drawer is about 299pt) the pile and the deck scale down to the band between the top fan and the pill row, the scale being a kernel number beside `pk_lay_pile` that the views read, the pill row keeps every pill out of the pile's column there, and the status corner drops its sub-line while compact.
Alternative: keep the 340pt layout and let the pills overlap the pile on short drawers.
Why: the table is the thing a player must read in the compact drawer, and a pill drawn over the pile hides the very card to match; all three options in B2 are taken together because each fixes a different collision.
Confidence: medium.

## Order of work

1. Design in parallel: rules and kernel doc, UI.html surface study with motion grid, reuse audit.
2. Lifts S0-S2 while the kernel is written from the rules doc.
3. Screens from UI.html against the kernel bridge.
4. Rig verification on a simulator, CI lane, docs.

## BLOCKED

Each item is one block: what is blocked now, what the owner (or the next worker) does, and a short dated history.
Every simulator proof owed below is also listed, ready to run, in `pickemup/docs/SIM_VERIFICATION.md`.

### B1: iOS simulators on this Mac hang host-wide

Now: clear since 2026-09-27 afternoon (a fresh iPhone 17e booted in 43 seconds and ran every B2/B3 job); the history below is why a watchdog stays on every boot and launch.
The hang cleared once on its own and came back, and a fresh device on iOS 27.0 hangs too, so it is host-wide and not tied to one device's state.
The `getpwuid_r did not find a match for uid 501` line seen inside a device points at the host's directory services under CoreSimulator, which only a reboot resets.
Blocked by it: the P8 after-run for lift S1 (and any later Swift lift), plus B2 and B3 below.
Owner action: none now; if it comes back, reboot the Mac.
Then, from `foolish/` on the S1 commit, run `DEST='platform=iOS Simulator,name=iPhone 17e,OS=27.0' bash ios/scripts/mac_tests.sh --no-lib --regen` and compare with the baseline in `REUSE_AUDIT.md` under S1 (853 executed, 1 skipped, only the flaky `MemoryProfileTests` failing).
History:
- 2026-09-26 23:06: every simulator hangs: test launches die with `Mach error -308 (ipc/mig) server died`, `simctl install` never returns, and a fresh device and the iOS 26.3 device both stop at boot in `com.apple.addressbook.migrator`; restarting CoreSimulatorService did not clear it.
- 2026-09-27 04:55: the orchestrator killed CoreSimulatorService and erased a second iPhone 17e; the erased device still stopped at boot in `com.apple.addressbook.migrator` (Migration Elapsed over a minute, `simctl launch` never returned).
- 2026-09-27 about 05:10: cleared on its own without a reboot; a health probe booted the first iPhone 17e and launched an app in under two minutes, and the S1 after-run and the simulator proofs resumed.
- 2026-09-27 06:12 to 07:40: back (see B2), and a freshly created iPhone 17 on iOS 27.0 also never finished booting within 100 seconds.
- 2026-09-27 afternoon: clear without a reboot; `pk-b2` (iPhone 17e, iOS 27.0) booted in 43 seconds under a 90-second watchdog and Messages launched in about a second.

### B2: the extension inside Messages

Now: mostly cleared (2026-09-27 afternoon, the B2/B3 simulator worker, a fresh iPhone 17e `pk-b2` on iOS 27.0); what is left is below under "Still owed" and "OPEN".
The game was played inside Messages: `simctl launch com.apple.MobileSMS` returned in about a second on the first watchdogged try, the extension opened from the + menu, and a two-seat game ran through a Debug persona (IOS_DECISIONS I43); 17 screenshots are in `pickemup/docs/shots/`, and each step's "Seen" line is in `SIM_VERIFICATION.md`.
`PickemupKitTests`: 38 tests, 0 failures, every test seen red (`pickemup/ios/TESTS_MUTATED.md`); the two red tests were a test-harness gap, not a regression (I42).
Fixed on the way: every flight to or from the pile flew to the board's top-left corner (I44, `AnchorTests`).
The Release configuration was not built in this pass (the command was not permitted here); a Release-only warnings-as-errors failure the persona would have caused was found by reading and restructured before commit. Built since (B4): `xcodebuild -scheme PickemupMessagesApp -configuration Release -destination 'generic/platform=iOS Simulator' build` succeeded with no warning.
Still owed: the catch, Last card! and the win inside Messages (no hand got near one card); anything with three or more seats (the simulator can only make a group thread as SMS, which offers no apps); the +2 and the Reverse; undo and reopen after a reorder; and on a real phone I9, I35 and I36 as below.
DONE 2026-09-27, late afternoon (the B4 worker, `pk-b4`): O10 landed and was seen (IOS_DECISIONS I45, `shots/compact_before.png` and `shots/compact_after.png`); the measured drawer is a 281pt view (263 of board), and the inner pill stands above Draw rather than beside Rules, for the reason in I45. The B4 pass also fixed the opened-bubble clock in code and test (I46), built Release for the simulator cleanly, and ran the whole `PickemupKitTests` scheme green (47 tests); it was stopped before the owed steps below, which stand, with a seed that reaches them in 14 bubbles (`SIM_VERIFICATION.md`).
Was OPEN, for the owner: the compact drawer on an iPhone 17e is about 299pt of board, and U2's lift was sized for 340. Compact, the pill row's inner pill (Undo or Pass beside Draw) is drawn over the pile, the deck's layers cover the status corner's sub-line while the staged strip is up, and the pile touches the top seat's fan (`shots/compact_collision.png`, `shots/skip_staged.png`, `shots/chips_after_draws.png`).
Options: (a) in a drawer shorter than 340pt the pile and the deck shrink to the band between the top fan and the pill row (a kernel scale beside `pk_lay_pile`, the views reading it); (b) the pill row keeps its pills out of the pile's column there (the inner pill to the leading side, beside Rules); (c) the status corner drops its sub-line while compact.
Recommendation: (a) with (c); (b) alone leaves the deck on the sub-line and the pile on the fan, and the 3-seat ring puts a side seat's fan in the deck's column at the same height. A first attempt that only lowered the pile to clear the status corner made the pill overlap worse and was reverted; it is not in the tree.
Earlier, proven without Messages: `PickemupKitTests` ran green (17 tests) on iPhone 17e `FC7586CF` with every test mutation-checked there (`pickemup/ios/TESTS_MUTATED.md`), and the app installed and registered with LaunchServices.
Compile-checked only (`PickemupKitTests` build-for-testing, the `PickemupMessagesApp` build and `make -C pickemup/c run asan`): O6 (IOS_DECISIONS I27) with its test `ActionCardCornerTests`, the strip chips (I28), the UI.html fixes listed in `SIM_VERIFICATION.md`, and the architecture review's fixes (I29 to I37).
Owed after the reboot, in this order: `PickemupKitTests` green, the red run of `ActionCardCornerTests` (its MUTATE line), the eight red runs listed under "The architecture review" in `pickemup/ios/TESTS_MUTATED.md`, the `pickemup/ios/scripts/mac_tests.sh` counts, then `source pickemup/ios/Tools/rig.env`, the rig's `stage` and `open`, the full two-seat game and the eleven screenshots `SIM_VERIFICATION.md` lists (lobby, table, drag, picker, catch and the rest).
Owed on a real phone: dragging a card DOWN off the deck never collapses the drawer (U24, IOS_DECISIONS I9 and I36), and a superseded stage never inserts (I35).
History:
- 2026-09-27 about 06:12 UTC: on `FC7586CF` the tests above ran green, but `simctl launch com.apple.MobileSMS` (the rig's `stage`) hung for over five minutes; the device was shut down.
- 2026-09-27 06:29 to 06:56 UTC: a second worker booted the same device three times (06:28, 06:37 after a shutdown and a 10 second wait, and 06:53); each stopped on the black data-migration spinner with `simctl bootstatus` at "Waiting on System App" for 90 seconds and more (the first was watched for 7 minutes).
  SpringBoard, backboardd and the data migrator were all running, and the migrator logged "System build version unchanged from 23D8133. Migration not necessary", so it is not a migration plugin; `log` answered `getpwuid_r did not find a match for uid 501`.
  The device was shut down each time and is shut down now.
- 2026-09-27 07:40: the orchestrator confirmed the hang is host-wide (B1).
- 2026-09-27, later: the architecture review worker attempted no simulator.
- 2026-09-27 about 12:00 local: the S5 worker ran the whole `PickemupKitTests` scheme on a fresh iPhone 17e (iOS 27.0) through `pickemup/ios/scripts/mac_tests.sh`: 37 executed, 2 failed, with no hang.
  The failures: `ActionCardCornerTests.testAnActionCardExposesItsSuitShape` (`XCTUnwrap` nil at `LayoutTests.swift:101`) and `NoCountLeakTests.testNoOtherSeatsLabelCarriesADigit` (no other seat on the accessibility tree, `ReviewTests.swift:35`).
  The pre-S5 script gave the same two, so they are the tests or the code, not the driver; they are owed a fix before "`PickemupKitTests` green" above can be ticked.
- 2026-09-27 afternoon: the B2/B3 simulator worker found both were the tests: SwiftUI builds no accessibility elements until an assistive client switches automation on, so both walks read an empty tree (I42); fixed in the tests, every test mutation-checked, then Messages opened and the game was played (above).

### B3: the filmed and measured animation take

Now: partly cleared (2026-09-27 afternoon, the B2/B3 simulator worker on `pk-b2`): `ActionCardCornerTests` and `RenderTests` run in seconds, the whole scheme ran, and three takes were filmed at normal speed and measured (`MOTION_REPORT.md`, sheets in `shots/motion/`): a two-seat deal before and after I44, and an opened bubble with three draws and a play after I44.
Still owed: a LIVE arrival (on one simulator the receiving thread is never on screen while the other sends, so every take is an opened bubble), a three-player deal (no group iMessage thread on the simulator), and the reshuffle after I44 (the one reshuffle take predates the fix).
FIXED in code and test, film owed (IOS_DECISIONS I46): an opened bubble's plan started while the drawer was still white, so its first draws were already in the air when the board appeared (MOTION_REPORT, Take B).
Also seen: `xcodebuild test` on this host can still hang AFTER the suite prints its summary (once in four whole-scheme runs); run it under a watchdog.
Originally owed: film a live arrival with three draws, a reshuffle and a play, and a deal, at normal speed; measure them with the `animation-measure` skill; put the contact sheets in `pickemup/docs/shots/motion/` and the scores in `MOTION_REPORT.md`; and run the whole `PickemupKitTests` scheme, which was only ever run in part.
History:
- 2026-09-27 about 03:35 local: the second iPhone 17e `6E0A730D` booted within the 90-second watchdog, and `BeatPlayerTests` (7 then, 8 once A17's `testTheJoinThatStartsTheGamePlaysTheDeal` landed) and `TableModelTests` (10) ran green on it, with every `BeatPlayerTests` test seen red there (`pickemup/ios/TESTS_MUTATED.md`).
  Any test that puts a window or a renderer on screen hung for ten minutes and was killed (`ActionCardCornerTests.testAnActionCardExposesItsSuitShape`, a card hosted in a `UIWindow`, and `RenderTests.testTheBubbleRendersAt300By195`), and `xcodebuild` itself hung after every finished run until killed.
  A filmed take is a window on screen, so it was not attempted.
- 2026-09-27 afternoon: a fresh iPhone 17e `pk-b2` booted in 43 seconds, nothing hung, and the takes above were filmed; the device was shut down and deleted afterwards.

### The final game name

Now: open, the owner's call.
`Pick 'Em Up` collides with two same-genre titles (README); it stays the working title behind one `GAME_NAME` string (O5).
Owner action: the USPTO search and the choice, before any store listing.

### App Store Connect, signing and upload

Now: not started, by design.
Owner action: the owner does the App Store Connect record, signing and upload by hand; nothing in this pass touches them.

## Found on the way (not pickemup's to fix in this pass)

- `werewolf/docs/UI.html` fails `shared/tools/check_ui_doc.py` because of a literal template tag inside a script comment.
  The fix is one line; it is werewolf's file and out of this branch's scope, so it is reported here for the owner.
- `.github/workflows/uttt-web.yml` did not trigger on `shared/c/mixrad.*`, which uttt's replay wasm compiles (D45).
  Fixed on this branch in `d0ca1c99`: both its `push` and `pull_request` paths now list `'shared/c/mixrad.*'` beside `'shared/c/b32.*'`.
- `foolish/e2e/validation/ci_toolchain_validation.test.ts` treats every `make ... wasm` line in every workflow as a build of foolish's test module and requires foolish's `scripts/ci_bots_test_wasm.sh` before it.
  So no other product's lane can build its own wasm without paying for foolish's (D49); the gate should look for foolish's targets, not the word.
- `.github/workflows/uttt-c.yml` could not go green on Linux gcc: glibc hides `M_PI` under `-std=c11`, and `uttt/c/src/uttt_pen.c` used it, so `make -C c run` stopped at the first compile (seen in the `gcc:13` image on 2026-09-27).
  FIXED in the commit "uttt pen: a file-local pi, so the Linux lane compiles": `UTTT_PI`, the same double, so `uttt_pen.o` is byte-identical on the Mac and `rough-diff` and the rendered board are unchanged; `run`, `asan` and `ios-smoke` pass in `gcc:13`.
- `pickemup/c/tests/pk_check.h`'s `seed_of` deals only 256 different games (every byte is a byte-valued function of k plus 7i).
  The fuzz and the wire tests now use `seed_wide`; `seed_of` stays for the committed 7.3 goldens.
- `REUSE_AUDIT.md` section 8 lists four defects in foolish and uttt (rig.sh restores entitlements with `git checkout`, the drawer-collapse numbers exist three times, flight timing is typed twice, foolish compiles the shared insert gating but never calls it).

## Final check

Run on 2026-09-27 in the main checkout on branch `pickemup`, code as at `22c752d5` (every later commit is docs), with `WASM_CC=/opt/homebrew/opt/llvm/bin/clang`, and no simulator booted.
xcodegen and xcodebuild were not run: `pickemup/ios` belongs to the iOS worker, and its last builds are recorded under B2.

| Check | Command | Result |
|---|---|---|
| Kernel, wire, bridge | `make -C pickemup/c clean run asan wasm` | pass: `pk_test` 11,089, `pk_rules_test` 199, `pk_plan_test` 1,193,089, `pk_say_test` 55,579, `pk_fuzz` 3,921,125, `pk_msg_test` 298,141, `pk_twophone_test` 2,409, `pk_beats_test` 225 assertions and the bridge 1,207 checks, all 0 failed; the ASan + UBSan run 0 failed in every suite; wasm32 objects built |
| iOS library, Swift bridge | `make -C pickemup/c ios-lib swift-smoke` | pass: xcframework written (layout `0x2ac6101c`); swift bridge 25 checks, 0 failed |
| Timeline | `make -C pickemup/c beats-dump` | pass: both takes match `MOTION_REPORT.md` row for row |
| Surface study | `python3 shared/tools/check_ui_doc.py pickemup/docs/UI.html` | pass: 80 devices, 13 views, markup and css balanced |
| Workflows | `yaml.safe_load` of `.github/workflows/pickemup.yml` and `uttt-c.yml` | pass: both parse |
| foolish validation | `cd foolish && npm run test:validate` | as known good: 150 tests, 128 pass; the 12 failed and 10 cancelled are all the Postgres suites (ECONNREFUSED on :5432, no database) |
| uttt | `make -C uttt/c run` | pass: every suite 0 failed (`uttt_msg` 3,062,830 checks) |
| werewolf | `make -C werewolf/c tests` | pass: 2,390 passed and the bridge smoke 115 passed, 0 failed |
| foolish C | `make -C foolish/c tests` | pass: 7,389 passed, 0 failed |
| foolish C, sanitized | `make -C foolish/c tests-asan` | pass: 7,389 passed, 0 failed |
| werewolf's study (not in the list, run for the record) | `python3 shared/tools/check_ui_doc.py werewolf/docs/UI.html` | FAIL, known and reported above: a `<template>` is never closed |
