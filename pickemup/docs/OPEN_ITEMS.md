# Pick 'Em Up - open items, closed or deferred

Every item the earlier workers left open, owed, deferred or named as a candidate in `ANIMATION_DECISIONS.md`, `IOS_DECISIONS.md` and `RULES_AND_KERNEL.md`, and what a grep of `ORCHESTRATION.md`, `DECISIONS.md`, `c/README.md` and `ios/README.md` for "open", "owed", "later", "next", "not yet", "deferred", "candidate", "TODO" and "unfinished" found beside them.
One line each: the item, where it came from, and its state.
DONE names the commit; DEFERRED names why it cannot be closed without a simulator, a phone or the owner.
This pass ran with no simulator (another worker holds the only free one).

## The checklist

1. A12, the wild's band slide (`ANIMATION_DECISIONS.md` A12): the Swift card snaps its band; play the kernel's BAND beat. OPEN
2. A13, the lobby's Join and Leave rows (`ANIMATION_DECISIONS.md` A13): the bridge has no lobby entry to beats. OPEN
3. A14 and I20, uttt's `CollapseSlide` (`ANIMATION_DECISIONS.md` A14, `IOS_DECISIONS.md` I20): compile it in behind a dev flag, with its curve in the kernel. OPEN
4. A15 and I21, the shared Send reminder `SendHint` (`ANIMATION_DECISIONS.md` A15, `IOS_DECISIONS.md` I21): compile it in behind a dev flag, with its word and fuse in the kernel. OPEN
5. I37, the drawer-collapse flag each touch stages with (`IOS_DECISIONS.md` I37): move to C. OPEN
6. I37, the order the badge stamps are shown in (`IOS_DECISIONS.md` I37): move to C. OPEN
7. 7.3.7, native against wasm on the same games (`RULES_AND_KERNEL.md` 7.3.7). OPEN
8. D53, `-Wpedantic -Wshadow -Wconversion` under a Linux gcc (`RULES_AND_KERNEL.md` D53). OPEN
9. `.github/workflows/uttt-c.yml` does not run uttt's `ios-smoke` (`DECISIONS.md` "Found on the way", `REUSE_AUDIT.md` S3). OPEN
10. The repository `.gitignore` carries an em dash and names the agent tool in a comment (`DECISIONS.md` "Found on the way"). OPEN
11. `ios/README.md` says the board is static and the flights are "the next layer", which the BeatPlayer has been since A1. OPEN
12. `project.yml` says SendHint is not compiled "see IOS_DECISIONS I11", the wrong decision number. OPEN
13. I4, the study's Draw x1 and "x on a draws-only bubble" rows (`IOS_DECISIONS.md` I4). OPEN
14. Open question 2, hands past thirteen cards (`RULES_AND_KERNEL.md` 8.2). OPEN
15. Open question 1, the name (`RULES_AND_KERNEL.md` 8.1, `ORCHESTRATION.md` O5 and BLOCKED). OPEN
16. I9 and I36, a downward drag off the deck never collapses the drawer (`IOS_DECISIONS.md` I9, I36). OPEN
17. I35, a superseded stage never inserts, seen on a phone (`IOS_DECISIONS.md` I35). OPEN
18. I25, snapshot goldens once the flight layer settles (`IOS_DECISIONS.md` I25). OPEN
19. I15, the suit and action glyphs as C polygon lists, "a later lift can move them" (`IOS_DECISIONS.md` I15). OPEN
20. I26, the placeholder icons (`IOS_DECISIONS.md` I26). OPEN
21. B1 to B3, every simulator proof: `PickemupKitTests` green, the Swift red runs under "Not mutated" in `ios/TESTS_MUTATED.md`, `mac_tests.sh` counts, the two-seat game and its screenshots, the filmed and measured takes (`ORCHESTRATION.md` B1, B2, B3, `SIM_VERIFICATION.md`, `MOTION_REPORT.md`). OPEN
22. `werewolf/docs/UI.html` fails `check_ui_doc.py` (`DECISIONS.md` "Found on the way"). OPEN
23. foolish's `ci_toolchain_validation.test.ts` reads every `make ... wasm` line as foolish's (`DECISIONS.md` "Found on the way", D49). OPEN
24. `REUSE_AUDIT.md` section 8's defects in foolish and uttt (rig.sh's `git checkout`, the collapse numbers three times, flight timing twice, the uncalled insert gating, uttt's stale README, two XCTest counts). OPEN
