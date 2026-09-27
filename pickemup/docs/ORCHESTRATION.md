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

## Order of work

1. Design in parallel: rules and kernel doc, UI.html surface study with motion grid, reuse audit.
2. Lifts S0-S2 while the kernel is written from the rules doc.
3. Screens from UI.html against the kernel bridge.
4. Rig verification on a simulator, CI lane, docs.

## BLOCKED

- The final game name: `Pick 'Em Up` collides with two same-genre titles (README); the USPTO search and the choice are the owner's, before any store listing.
- App Store Connect record, signing and upload: the owner does these by hand; nothing in this pass touches them.

BLOCKED B1: the P8 after-run for lift S1 (and any later Swift lift).
From 2026-09-26 23:06 every iOS simulator on this Mac hangs: test launches die with `Mach error -308 (ipc/mig) server died`, `simctl install` never returns, and a fresh device and the iOS 26.3 device both stop at boot in `com.apple.addressbook.migrator`.
Restarting CoreSimulatorService did not clear it; a Mac reboot is the likely fix, and only the owner can do that.
BLOCKED B1, confirmed by the orchestrator at 2026-09-27 04:55: after killing CoreSimulatorService and erasing a second iPhone 17e, the erased device still stops at boot in `com.apple.addressbook.migrator` (Migration Elapsed over a minute, `simctl launch` never returns).
The host needs a reboot before any simulator test, screenshot or rig run can happen; everything below that needs a simulator is verified by compile only until then.
B1 cleared on its own at about 05:10 on 2026-09-27 without a reboot: a health probe booted the first iPhone 17e and launched an app in under two minutes, so the after-run for S1 and the simulator proofs resumed then.
Once it is clear, run `DEST='platform=iOS Simulator,name=iPhone 17e,OS=27.0' bash ios/scripts/mac_tests.sh --no-lib --regen` from `foolish/` on the S1 commit and compare with the baseline in `REUSE_AUDIT.md` under S1 (853 executed, 1 skipped, only the flaky `MemoryProfileTests` failing).

BLOCKED B2: the Pick 'Em Up extension has not yet been seen inside Messages.
On 2026-09-27 at about 06:12 UTC, on iPhone 17e `FC7586CF`, `PickemupKitTests` ran green (17 tests) and every test was mutation-checked there (`pickemup/ios/TESTS_MUTATED.md`), and the app installed and registered with LaunchServices, but `simctl launch com.apple.MobileSMS` (the rig's `stage`) hung for over five minutes, the B1 symptom again, so no screenshot of the lobby, table, drag, picker or catch exists yet.
The device was shut down.
Next: on a healthy simulator, `source pickemup/ios/Tools/rig.env`, then the rig's `stage`, `open` and screenshots of each screen; and on a real phone, prove that dragging a card DOWN off the deck (U24, IOS_DECISIONS I9) never collapses the drawer.
B2, second worker, 2026-09-27 06:29 to 06:56 UTC: the same iPhone 17e `FC7586CF` (iOS 26.3) never finished booting, so Messages, the rig and the fallback host were all out of reach.
Three boots (06:28, 06:37 after a shutdown and a 10 second wait, and 06:53) each stopped on the black data-migration spinner with `simctl bootstatus` at "Waiting on System App" for 90 seconds and more (the first was watched for 7 minutes).
Inside the device SpringBoard, backboardd and the data migrator were all running, and the migrator logged "System build version unchanged from 23D8133. Migration not necessary", so this is not a migration plugin; `log` itself answered `getpwuid_r did not find a match for uid 501`, which points at the host's user session under CoreSimulator, the B1 family.
The device was shut down each time and is shut down now.
What landed without a simulator, compile-checked (`PickemupKitTests` build-for-testing and the `PickemupMessagesApp` build both succeed): O6 (IOS_DECISIONS I27) with its test `ActionCardCornerTests`, the strip chips (I28), and the UI.html fixes listed in `SIM_VERIFICATION.md`.
Still owed on a healthy simulator, in this order: `PickemupKitTests` green, the O6 test's red run (its MUTATE line), `mac_tests.sh` counts, then the full two-seat game and the eleven screenshots `SIM_VERIFICATION.md` lists; the host most likely needs the reboot B1 asked for.

## Found on the way (not pickemup's to fix in this pass)

- `werewolf/docs/UI.html` fails `shared/tools/check_ui_doc.py` because of a literal template tag inside a script comment.
  The fix is one line; it is werewolf's file and out of this branch's scope, so it is reported here for the owner.
- `.github/workflows/uttt-web.yml` does not trigger on `shared/c/mixrad.*`, which uttt's replay wasm now compiles (D45).
  uttt's C tests do run on it (`uttt-c.yml` triggers on `shared/c/**`), but a change to mixrad alone would not rebuild or redeploy uttt.live; adding `'shared/c/mixrad.*'` beside `'shared/c/b32.*'` there is a one-line change to a workflow this pass may not edit.
- `foolish/e2e/validation/ci_toolchain_validation.test.ts` treats every `make ... wasm` line in every workflow as a build of foolish's test module and requires foolish's `scripts/ci_bots_test_wasm.sh` before it.
  So no other product's lane can build its own wasm without paying for foolish's (D49); the gate should look for foolish's targets, not the word.
- `pickemup/c/tests/pk_check.h`'s `seed_of` deals only 256 different games (every byte is a byte-valued function of k plus 7i).
  The fuzz and the wire tests now use `seed_wide`; `seed_of` stays for the committed 7.3 goldens.
- `REUSE_AUDIT.md` section 8 lists four defects in foolish and uttt (rig.sh restores entitlements with `git checkout`, the drawer-collapse numbers exist three times, flight timing is typed twice, foolish compiles the shared insert gating but never calls it).
