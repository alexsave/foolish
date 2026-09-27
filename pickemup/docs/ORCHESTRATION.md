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
C prefix `pu_`, kernel in `pickemup/c/src/pu_*.{c,h}`, tests in `pickemup/c/tests/`, iOS bridge in `pickemup/c/ios/`, Makefile targets `run`, `asan`, `ios-lib`, `ios-smoke` as in uttt.
`make -C c ios-lib` writes `pickemup/ios/vendor/Pickemup.xcframework`.
Swift targets `PickemupKit`, `PickemupMessages`, `PickemupMessagesApp`, project `pickemup/ios/Pickemup.xcodeproj` from `pickemup/ios/project.yml`.
Alternative: foolish's names (`msg_*`, FoolishKit shape).
Why: uttt is the freshest second-product template and its shape is the one `shared/tools` already drives.
Confidence: high.

DECISION O3: lifts S0, S1 and S2 from `REUSE_AUDIT.md` land before the screens; S3 (CI) lands with the kernel; every other lift is copy-first and lifted later or not at all in this pass.
Alternative: lift everything the screens need before building them.
Why: the medium and high risk lifts touch foolish's animation and wire core, and foolish has real users; a copy with a `COPIED from` header costs nothing now and is deleted by the lift that owns it.
Confidence: high.

## Order of work

1. Design in parallel: rules and kernel doc, UI.html surface study with motion grid, reuse audit.
2. Lifts S0-S2 while the kernel is written from the rules doc.
3. Screens from UI.html against the kernel bridge.
4. Rig verification on a simulator, CI lane, docs.

## BLOCKED

BLOCKED B1: the P8 after-run for lift S1 (and any later Swift lift).
From 2026-09-26 23:06 every iOS simulator on this Mac hangs: test launches die with `Mach error -308 (ipc/mig) server died`, `simctl install` never returns, and a fresh device and the iOS 26.3 device both stop at boot in `com.apple.addressbook.migrator`.
Restarting CoreSimulatorService did not clear it; a Mac reboot is the likely fix, and only the owner can do that.
Once it is clear, run `DEST='platform=iOS Simulator,name=iPhone 17e,OS=27.0' bash ios/scripts/mac_tests.sh --no-lib --regen` from `foolish/` on the S1 commit and compare with the baseline in `REUSE_AUDIT.md` under S1 (853 executed, 1 skipped, only the flaky `MemoryProfileTests` failing).
