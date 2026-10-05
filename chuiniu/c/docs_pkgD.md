# Package D report: the stage

The stage is the one module that turns the kernel's table state and a clock into pixels and HUD frames.
The decisions are in `chuiniu/docs/DECISIONS.md`, I19 to I26, beside a summary of packages A to C.
Measured 2026-10-05 on an Apple M1.

## Files

- `src/cn_stage.h`, `src/cn_stage.c`: the stage. It is freestanding (memcpy and memset only, the kernel's own maths) and is in `SRC`, so it is in every test, in `make wasm` and in every iOS slice.
- `ios/cn_api.c`, `ios/include/cn_api.h`: `cn_api_stage_*`. The bridge holds one static `CnStage`, about 700 KB, mostly the six bakes.
- `ios/cn_api_layout.h`, `ios/layout.args`: `CnStageHud` and `CnStageShot` are structgen roots and snapshots, with the constants a host needs.
- `tests/cn_stage_test.c`: 136 assertions. The C smoke has 94 checks and the Swift smoke 47.
- Swift: `TableStage` on the seam (`KernelSeam.stage()`), `BridgeStage` in `BridgeKernel.swift`, and `testTheStageDrawsTheRevealTheRollAndTheBubble`.
- Makefile:
  - `cn_tex.c` and `cn_stage.c` are in `SRC`.
  - `cn_geom_test`, `cn_cam_test`, `cn_lay_test` and `cn_stage_test` now run in `run` and `asan`. Package B's three tests had been built but never run.
  - `ios-lib` runs `tex-ios`, so the pack ships beside the readers as a build output (git-ignored).
  - The icon tool's link stopped naming `cn_tex.c` twice.

## API

```
cn_stage_init(st, arena, bytes, pack, len)      cn_stage_purge(st)   cn_stage_attach(st, arena, bytes)
const CnStageHud *cn_stage_begin(st, &in)       kind TABLE / REVEAL / BUBBLE
cn_stage_frame(st, t_ms, peek, lift, &w, &h)    one thread
cn_stage_prepare(st, t, peek, lift)  cn_stage_band(st, pass, band, n)  cn_stage_finish(st)
cn_stage_shot(st)   cn_stage_done(st, t)   cn_stage_total_ms(st)   cn_stage_objects(st, t, peek, lift, &n)
```

The bridge fills `CnStageIn` from the resident game, so a host passes only the screen, the drawer's size, its scale and `roll`.
A reveal's lift is read from the current plan's LIFT beat, and a roll starts at its SHAKE beat.
The band form takes the pass as well as the band, because each of the renderer's three passes must finish before the next starts.

## Memory (48 MB arena, six seats, Fay out)

The textures, with their half-size copies, take 12.2 MB.
The still frames come out as follows:

| drawer | turn | canvas (pts) | pad (study's) | still scale |
|---|---|---|---|---|
| 390x340 | mine | 438x330 | 40 (40) | 2x |
| 390x340 | theirs | 414x398 | 8 (40) | 2x |
| 375x541 | mine | 409x477 | 8 (87) | 2x |
| 375x541 | theirs | 429x611 | 42 (161) | 2x |
| 390x718 | mine | 456x760 | 114 (258) | 1.5x (36.1 MB at 2x) |
| 390x718 | theirs | 484x1017 | 271 (467) | 1.5x |
| 430x830 | mine | 524x1054 | 296 (501) | 1.5x |
| 430x830 | theirs | 556x1219 | 361 (903) | 1x |

Throw frames are drawn at 1.5x where they fit.
Package A's 33.9 MB for a 2x frame assumed no side room and a 40-point pad; the study's canvas is wider by pad_x and much taller.
Cutting each frame's top to its bodies' reach is what brought 375x541 and the collapsed drawer to 2x.

## Determinism

The pinned hashes are `0xc83b39dc` for the throw frame at 1.2 s (1.5x) and `0xbc77a1a7` for the still frame peeking (2x), both 375x541 with six seats.
Apple clang -O2, GCC 16 -O2 and ASan/UBSan all give these hashes.
`make wasm` builds the stage freestanding.
Sixteen bands, seven bands and reverse band order give the single thread's bytes.
So does `DispatchQueue.concurrentPerform` in the Swift smoke.

## Mutation checks

Each was applied alone, rebuilt from clean and run, then the original text was written back.
The line numbers are those of `tests/cn_stage_test.c` and `ios/cn_api_smoke.c` as committed (some runs predate two added tests; the assertions are the same).

| Mutation | Went red |
|---|---|
| HUD cup_x without the board's offset | `:109` every seat where the study puts it |
| bodies emitted without pad_x | `:167` each crown over its HUD cup, `:326`, `:327` goldens |
| the up face ignored when painting | `:221` dealt value up (303 wrong), goldens |
| a side face overwritten after painting | `:222` opposite faces sum to 7, `:223` each face once |
| attach keeps `uploaded` | `:268` the same bytes after a purge, `:290` |
| no scale fallback | `:251` 390x718 drawn at 1.5, `:290` |
| no warm frame (half-size copies made by the first frame) | `:284` a still frame the same after a throw frame, `:251` |
| a 7-band frame cut as 8 | `:317` 7 bands |
| shadow dark .55 to .5 | `:326`, `:327` goldens |
| CN_STAGE_SCALE_ROLL 2 | `:319` a throw frame at 1.5, `:251` |
| CN_STAGE_SCALE_STILL 3 | `:321`, `:343` the bubble 600x390 |
| the bake's seed drifts between begins | `:324` begun again: the same frames |
| bubble step 61 | `:348` the cups in a row |
| bubble canvas 24 taller | `:343`, `:344` |
| the out mask dropped | `:367` lying, `:368` dim, `:102`, `:109`, `:110`, `:159` |
| done at `>` total | `:392` done exactly at the total, `:321` |
| the roll clock ignores roll_at | `:400` nothing moves before roll_at |
| my rest time not recorded | `:390` my dice rest inside the roll |
| the hit ellipse not turned by the camera | `:130` the crown's top on the ellipse's edge |
| bridge: the lift always 1 | smoke `:82` the cups lift with the LIFT beat; Swift `BridgeKernelTests.swift:215` the same |
| bridge: roll_at not from the SHAKE beat | smoke `:86` |
| bridge: a reveal's dice not filled | smoke `:71`, `:75` |

Two guards cannot go red alone, so they state their rule and are kept:

- `textures()` refusing a set that did not fit: any arena too small for the crowns has no room left for a frame either.
- `total_ms` held to the SHAKE beat's end: every throw outlasts the beat, since the shortest shake is 1.5 s and the beat is 0.76 s.

Two fixes on the way:

- `st->below` was never read and is gone.
- The purge's own reset of the texture state duplicated the attach's and is gone, leaving the attach as the one owner.
