# Rebase inventory: what `origin/main` brought, and how the replay handled it

> **Closed ledger, 2026-09-17.** The rebase landed; the commit SHAs below are
> pre-rebase and will not resolve. Kept for the batch-invariant technique and
> the measured 273-byte `bots.wasm.gz` delta.

This is the checklist for the final step of `docs/C_GAME_SHAPE_MIGRATION.md` ("Final step: rebase onto main, replayed slowly").
The owner chose a rebase replay over one merge so that nothing incoming is swallowed by a single conflict resolution, and this file is the record that proves it was not.

Branch point: `a844b2a1`.
Ours: 187 commits.
Theirs: 27 commits, 41 files, 3,143 insertions, 135 deletions.
Working branch: `rebase-attempt`.
The escape hatch is the branch `worktree-c-game-shape` and the tag `pre-rebase-c-game-shape`, both left where they were.

## 1. What main brought, by area

`git log --oneline a844b2a1..origin/main` is 27 commits and they are one body of work: the throw-in slide, the undo holds, the pass preview's slot, the role marks, and the rig that filmed all of it.
Build 72 and build 73.

### Kernel C (5 files, 140 insertions)

Two new pure functions and their iOS bridges.

- `c/src/anim_plan.c` / `.h`: `anim_shown_table_rows(live, n_live, sweep, n_sweep, n_pending, hold_leaving, out_sweeping)`, which is `anim_shown_table` given the rows so a sweep that covers the whole live table and more is drawn as a sweep - a card leaving a table that stays is hidden by its own flight instead of by a paint 90ms early.
- `c/src/anim_plan.c` / `.h`: `anim_pass_slot_shown(previewing, dragging, seen_this_drag, over_dead_pair, held_at, n_battles, rules)` with `ANIM_PASS_HOLD` and `ANIM_PASS_STICKY`, which stops the pass preview's empty slot opening and closing three times for one pass.
- `c/ios/ios_api.c` / `c/ios/include/ios_api.h`: `fio_shown_table_rows` and `fio_pass_slot_shown`, the thin bridges, plus `FIO_PASS_HOLD` / `FIO_PASS_STICKY`.
- `c/tests/tests.c`: 56 lines of C cases for both.

This is the area that overlaps ours, and the good news of the inventory is that it overlaps by ADDITION: main appended two functions at the end of an existing section, and every one of our commits that touched those files touched other parts of them.

### Swift, shipping (10 files)

- New: `ios/FoolishKit/Boards/PassSlot.swift`, `UndoFlightSource.swift`, `UndoGate.swift`.
- Changed: `MessageTableView.swift` (+181), `FRoleMotion.swift` (+248), `FBattleGrid.swift`, `FActionBar.swift`, `BoardFlight.swift`, `FHandFan.swift`, `CollapseRuler.swift`, `CollapseTween.swift`, `MessagesViewController.swift`.
- `sdk/swift/PreTableWire.swift`: `PassSlotWire.shown(...)` and a second `shownTable(...)` overload taking `holdLeaving`, both of them calls into the two new kernel functions.

None of these files is touched by our 187 commits, and `PreTableWire.swift` is NOT one of the files Phase 10 deleted - it is still in the tree at our head, unmodified since the branch point.
The `PreTableWire.swift` case the migration doc worried about therefore does not arise: main's additions to it are already "read through the kernel", which is the Phase 10 rule.

### Swift, tests (10 files)

New: `PassSlotTests`, `RoleCoinRotateInTests`, `TableThrowInSlideTests`, `UndoFlightSourceTests`, `UndoGateTests`, `UndoHoldsTableTests`, `UndoKeepsTiltTests`, `UndoReleaseHandHoldTests`.
Changed: `HoldbackTests`, `VeilOutsTests`.
None touched by us.

### Rig (Python and shell, 10 files)

New: `lib/board.py`, `lib/squareplot.py`, `lib/squares.py`, `lib/tablesquares.py`, `shots/anim_reel.sh`, `shots/throwin_slide.sh`.
Changed: `rig.sh` (new `throwin` / `cover` commands, `clearstage stay`), `README.md` (a block of new traps), `lib/bars.py`, `lib/tween.py`.
We touched `rig.sh` and `README.md` once, in `d985fb74` (iOS 27 labels, the recorder dying with the take).

### Web, server, e2e

Nothing.
Main's 27 commits touch no TypeScript at all, which is why the collision surface with this branch is as small as it is.

### Docs and artifacts

- `docs/ANIMATION_CORE_C.md`: six lines describing `anim_shown_table_rows`.
- `ios/project.yml`: `CURRENT_PROJECT_VERSION` 71 to 73, in two places.
- `sdk/ts/wasm/WASM_STAMP`: the sha line only. No `.wasm.gz` changed on main; `cf79e396` and `fb2767cb` both say the artifacts came out byte-identical.

## 2. The files both sides touched

Ten files, and the full list of them:

| File | Main | Ours |
| --- | --- | --- |
| `c/ios/include/ios_api.h` | +2 declarations, +2 defines, at the end of the anim block | rewritten around generated structs |
| `c/ios/ios_api.c` | +2 bridge functions | rewritten around generated structs |
| `c/src/anim_plan.c` | +2 functions | extended in Phase 9 |
| `c/src/anim_plan.h` | +2 declarations, +2 defines | extended in Phase 9 |
| `c/tests/tests.c` | +56 lines of cases | many phases |
| `docs/ANIMATION_CORE_C.md` | +6 lines | Phase 9 |
| `ios/Tools/rig/README.md` | +44 lines of traps | `d985fb74` |
| `ios/Tools/rig/rig.sh` | +116 lines, new commands | `d985fb74` |
| `ios/project.yml` | build 71 to 73 | Phase 10 |
| `sdk/ts/wasm/WASM_STAMP` | the sha line | every wasm rebuild |

`WASM_STAMP` is the only guaranteed conflict, it is a generated artifact, and its rule is fixed in advance: never pick a side, re-stamp by rebuilding.

## 3. Batches

Replayed with `git rebase --onto`, one phase at a time, HEAD detached, `rerere.enabled true`.

| # | Range | What |
| --- | --- | --- |
| 1 | ..`106fa764` | Phase 0.x: the plan, structgen, the pre-migration goldens, the benches |
| 2 | ..`d1ccc28e` | Phase 2 and 3b: the C Roster, the layout hash, the C Table's first half |
| 3 | ..`35cfa0c6` | Phase 4b: every server operation on the C Table |
| 4 | ..`ec8ed92c` | Phase 5a and 4c: the contract migration |
| 5 | ..`bce797bc` | Phase 5b: as3 |
| 6 | ..`144ca23d` | Phase 6a and 6b: the display rules and the client's boards |
| 7 | ..`982c7c23` | Phase 7: the Oracle |
| 8 | ..`f6d3a6af` | Phase 8, part 1: the allowlist closes |
| 9 | ..`79ad2b9f` | Phase 8, part 2: the TS game shape is deleted |
| 10 | ..`0dd06477` | Phase 8 final pass |
| 11 | ..`02a7247c` | Phase 8 final pass, the rest |
| 12 | ..`54078863` | Phase 9: the web's timing in C |
| 13 | ..`71d7c605` | Phase 10 and the cleanup |
| 14 | ..`3d2b508a` | Phase 11: the doctrine |

## 4. The log

### The one invariant the replay was checked against

After every batch, `git diff --stat <the batch's original end commit> HEAD` was compared with main's own diffstat.
It came out `41 files changed, 3143 insertions(+), 135 deletions(-)` after all fourteen, which is main's diff exactly.
That is the mechanical statement that the replay added main and lost nothing of either side: if any of our 187 commits had been dropped, or any of main's 27 hunks swallowed, the numbers would move.

### What conflicted

Forty conflicts, every one of them `sdk/ts/wasm/WASM_STAMP`, and no other file conflicted in the whole replay.
That is the shape the inventory predicted: main changed no TypeScript, and its C is appended at the end of sections our phases did not touch.

The stamp was never resolved by taking a side.
It is a pure function of the wasm source set (`scripts/wasm_stamp.sh --hash`), so each conflict was resolved by REGENERATING it against the tree at that commit - which is a tree that now includes main's `anim_plan.c`.
Forty regenerations produced forty distinct values, so no `rerere` resolution was replayed onto a commit it did not belong to.

| Batch | Commits | Conflicts | Resolution |
| --- | --- | --- | --- |
| 1 Phase 0.x | 15 | 2 stamp | regenerated |
| 2 Phase 2, 3b | 15 | 5 stamp | regenerated |
| 3 Phase 4b | 17 | 9 stamp | regenerated |
| 4 Phase 5a, 4c | 6 | none | - |
| 5 Phase 5b | 11 | none | - |
| 6 Phase 6a, 6b | 9 | 4 stamp | regenerated |
| 7 Phase 7 | 14 | 3 stamp | regenerated |
| 8 Phase 8 part 1 | 13 | none | - |
| 9 Phase 8 part 2 | 24 | 5 stamp | regenerated |
| 10 Phase 8 final | 17 | 3 stamp | regenerated |
| 11 Phase 8 final, rest | 12 | 2 stamp | regenerated |
| 12 Phase 9 | 16 | 5 stamp | regenerated |
| 13 Phase 10, cleanup | 15 | 2 stamp | regenerated |
| 14 Phase 11 | 5 | none | - |

Batch 10 also hit a transient `index.lock` from another process in this repository; the rebase rescheduled its last pick and it went through on the next `--continue`.

### The gates, per batch

C suite (`make -C c tests`), `tools/structgen/gen.sh --check` and both `tsc` projects ran after every batch and were green each time.
The C suite grew as the phases landed - 4,953 after batch 1 to 5,730 at the tip - and 0 failed at every stop.
`gen.sh --check` reported `gen: fresh` at every batch, including `diff -r sdk/swift/gen`, so Phase 10's generated Swift is consistent with a header that now carries main's two new `fio_` declarations.
Beyond that: `test:swift-parity` after batches 2, 13 and at the tip; `test:validate` after batch 3 and at the tip; the two doctrine guards after batch 9 and at the tip; `make ios-smoke`, `make ios-goldens` and `ios/scripts/lint_architecture.sh` after batch 13 and at the tip.

`scripts/check_wasm_freshness.sh origin/main` is green at the tip and was green at batches 1 to 4.
It was RED at batch 5, and that is inherited rather than introduced: the original branch's batch 5 changes `tools/structgen/structgen.c`, which is in the stamped source set, and re-stamps only later in `da8e6e41`.
An intermediate commit is allowed to be stale; the tip is not, and the tip is not.

### The rebuild, and why no artifact byte is committed for it

Every wasm artifact was rebuilt once on the replayed tree (`make -C c WASM_CC=/opt/homebrew/opt/llvm/bin/clang wasm-bots wasm-oracle wasm-oracle-mt`).
`public/oracle.wasm.gz` and `public/oracle-mt.wasm.gz` came out byte-identical to what is committed.
`sdk/ts/wasm/bots.wasm.gz` came out 273 B smaller (80,913 to 80,640).

That 273 B is toolchain skew, not main's C, and it was measured rather than assumed: with main's five C files reverted to the pre-rebase branch and nothing else changed, the same build produces the same 80,640 B.
So main's `anim_shown_table_rows` and `anim_pass_slot_shown` cost the shipped module exactly zero bytes - they are reachable from no wasm export, which is what main's own `cf79e396` and `fb2767cb` said when they re-stamped without a byte moving.
The rebuilt bytes were therefore discarded and the committed artifacts kept, because committing them would churn a shipped module by a quantity this repo has already decided is meaningless (the reasoning is in the header of `scripts/check_wasm_freshness.sh`, and it is why the stamp exists at all).
The stamp in the tree matches the sources in the tree, main's C included.

### Files main changed that this branch had deleted

None.

The one the migration doc named in advance, `sdk/swift/PreTableWire.swift`, is not deleted here: Phase 10 left it in place and this branch has not touched it since `a844b2a1`.
Main's addition to it is `PassSlotWire.shown` and a `shownTable(…, holdLeaving:)` overload, and both are calls straight into `fio_pass_slot_shown` and `fio_shown_table_rows`.
That is a reader over the kernel, which is the rule Phase 10 was enforcing, so there was no intent to port: the change lands as written.

Nothing else main touched is missing here, which the diffstat invariant above states mechanically.

### What was dropped

Nothing.

No incoming commit was skipped, no incoming hunk was resolved away, and the only content that was deliberately not carried forward is the 273 B of rebuilt `bots.wasm.gz`, which is a rebuild of the same sources on a different toolchain and is argued above.

