# Rebase inventory: what `origin/main` brought, and how the replay handled it

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

Appended per batch below: what conflicted, how it was resolved, and every incoming change deliberately dropped with its reason.
