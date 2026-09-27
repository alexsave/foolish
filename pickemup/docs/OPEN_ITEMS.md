# Pick 'Em Up - open items, closed or deferred

Every item the earlier workers left open, owed, deferred or named as a candidate in `ANIMATION_DECISIONS.md`, `IOS_DECISIONS.md` and `RULES_AND_KERNEL.md`, and what a grep of `ORCHESTRATION.md`, `DECISIONS.md`, `c/README.md` and `ios/README.md` for "open", "owed", "later", "next", "not yet", "deferred", "candidate", "TODO" and "unfinished" found beside them.
One line each: the item, where it came from, and its state.
DONE names the commit; DEFERRED names why it cannot be closed without a simulator, a phone or the owner.
This pass ran with no simulator (another worker holds the only free one).

## The checklist

1. A12, the wild's band slide (`ANIMATION_DECISIONS.md` A12): the Swift card snaps its band; play the kernel's BAND beat. DONE `8e504dc2` (A18).
2. A13, the lobby's Join and Leave rows (`ANIMATION_DECISIONS.md` A13): the bridge has no lobby entry to beats. DONE `86667c43` (`pk_api_beats_lobby`, `pk_api_adopt` for a lobby over its lobby, `PK_API_W_LOBBY_GONE`; A19).
3. A14 and I20, uttt's `CollapseSlide` (`ANIMATION_DECISIONS.md` A14, `IOS_DECISIONS.md` I20): compile it in behind a dev flag, with its curve in the kernel. DONE `86667c43` (`pk_lay_collapse_push`, `PK_LAY_COLLAPSE_*`, `dev.slide`; A20); how it looks inside Messages is DEFERRED to a phone.
4. A15 and I21, the shared Send reminder `SendHint` (`ANIMATION_DECISIONS.md` A15, `IOS_DECISIONS.md` I21): compile it in behind a dev flag, with its word and fuse in the kernel. DONE `86667c43` (`SEND_HINT`, `PK_T_SEND_HINT`, `dev.sendhint`; A20, I41); its place under Messages' Send button is DEFERRED to a phone.
5. I37, the drawer-collapse flag each touch stages with (`IOS_DECISIONS.md` I37): move to C. DONE `0277acef` (`pk_api_collapses`; I40).
6. I37, the order the badge stamps are shown in (`IOS_DECISIONS.md` I37): move to C. DONE `0277acef` (`pk_api_stamp`; I40).
7. 7.3.7, native against wasm on the same games (`RULES_AND_KERNEL.md` 7.3.7). DONE `552ada41` (`make cross`: 100 games natively and in wasm32 under node 26, alike).
8. D53, `-Wpedantic -Wshadow -Wconversion` under a Linux gcc (`RULES_AND_KERNEL.md` D53). DONE `04ef2388` (gcc 13.5 in Docker; the flags are in every build, D58).
9. `.github/workflows/uttt-c.yml` does not run uttt's `ios-smoke` (`DECISIONS.md` "Found on the way", `REUSE_AUDIT.md` S3). DONE `985d3d2e` (the YAML parses); the lane itself is red on Linux gcc before and after, because `uttt/c/src/uttt_pen.c` uses `M_PI` under `-std=c11`, which is uttt's to fix (item 25).
10. The repository `.gitignore` carries an em dash and names the agent tool in a comment (`DECISIONS.md` "Found on the way"). DONE `985d3d2e`.
11. `ios/README.md` says the board is static and the flights are "the next layer", which the BeatPlayer has been since A1. DONE `c512dbe2`.
12. `project.yml` says SendHint is not compiled "see IOS_DECISIONS I11", the wrong decision number. DONE `86667c43` (the comment names A14 and A15).
13. I4, the study's Draw x1 and "x on a draws-only bubble" rows (`IOS_DECISIONS.md` I4). DONE: the rows were fixed by an earlier docs pass, and the "turn" view's first frame, which still drew a staged field after two draws, in `c512dbe2`.
14. Open question 2, hands past thirteen cards (`RULES_AND_KERNEL.md` 8.2). DONE: answered in 8.2 by ORCHESTRATION O4 (flagged for a veto); the pickemup docs pass wrote the same answer in parallel, and its wording was kept in the merge.
15. Open question 1, the name (`RULES_AND_KERNEL.md` 8.1, `ORCHESTRATION.md` O5 and BLOCKED). DEFERRED: owner-only (the trademark search and the choice).
16. I9 and I36, a downward drag off the deck never collapses the drawer (`IOS_DECISIONS.md` I9, I36). DEFERRED: a phone (Messages' own recognizer, in another process).
17. I35, a superseded stage never inserts, seen on a phone (`IOS_DECISIONS.md` I35). DEFERRED: a phone.
18. I25, snapshot goldens once the flight layer settles (`IOS_DECISIONS.md` I25). DEFERRED: a simulator (references are recorded from a render).
19. I15, the suit and action glyphs as C polygon lists, "a later lift can move them" (`IOS_DECISIONS.md` I15). DEFERRED: a simulator; the lift is only worth making with a before-and-after render showing the curves unchanged.
20. I26, the placeholder icons (`IOS_DECISIONS.md` I26). DEFERRED: owner-only (art direction).
21. B1 to B3, every simulator proof: `PickemupKitTests` green, the Swift red runs under "Not mutated" in `ios/TESTS_MUTATED.md` (this pass added nine planned rows), `mac_tests.sh` counts, the two-seat game and its screenshots, the filmed and measured takes (`ORCHESTRATION.md` B1, B2, B3, `SIM_VERIFICATION.md`, `MOTION_REPORT.md`). DEFERRED: a simulator.
22. `werewolf/docs/UI.html` fails `check_ui_doc.py` (`DECISIONS.md` "Found on the way"). DEFERRED: `werewolf/` is outside this pass.
23. foolish's `ci_toolchain_validation.test.ts` reads every `make ... wasm` line as foolish's (`DECISIONS.md` "Found on the way", D49). DEFERRED: `foolish/` is outside this pass; it is also why `make cross` is not in the pickemup lane.
24. `REUSE_AUDIT.md` section 8's defects in foolish and uttt (rig.sh's `git checkout`, the collapse numbers three times, flight timing twice, the uncalled insert gating, uttt's stale README, two XCTest counts). DEFERRED: `foolish/`, `uttt/` and `shared/` are outside this pass; A20 adds pickemup's copy of the collapse numbers as a fourth, so the `shared/c` lift that removes them all now has four callers.
25. Found in this pass: the uttt-c lane stops at the first compile on Linux gcc (`M_PI` under `-std=c11` in `uttt/c/src/uttt_pen.c`). DEFERRED: `uttt/` is outside this pass; recorded in `DECISIONS.md` and `ORCHESTRATION.md` with the one-line fix.
26. Found in this pass: `make beats-dump` did not build under gcc (`-Wunused-variable` on the harness's test name). DONE `04ef2388`.
