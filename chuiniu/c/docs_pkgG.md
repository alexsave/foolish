# Package G report: smooth shadows, and a stage that opens without its arena

## 1. The shadow on the open table

### What was wrong

The open table (pixels no body covers) took its shadow once per 4-by-4 pixel block: one map lookup at the block's middle, one alpha for all sixteen pixels.
At 2x a block is 2 points, so every shadow edge on the table was a staircase of 2-point steps (`docs/shots/e2_04_bubble_shadow_zoom.png`).
Under that, the filter itself read only the even texels of the map (four taps two texels apart), so even a pixel-exact edge creased every two texels.

### What it is now

- `lit_q` is the one filter, for the bodies and the table alike: three bilinear readings one texel apart, which is four texels each way weighted (1 - t), 1, 1, t over 3.
  Every texel counts, the light runs on without a crease, and the softness is the study's old two-texel penumbra (a spread of 0.91 texel against the old 0.82).
  The weights are kept whole (a ninth taken once at the end), so every sum is exact: all lit is exactly 1, all dark exactly 0, on every compiler.
- The open table is still walked in 4-by-4 blocks, but a block is taken whole only when it provably is one thing.
  Its pixels' places on the map are linear in the pixel (flat table, one light direction), so the block's corners bound every texel any of its pixels reads (`table_block`, widened past rounding).
  If none of those texels was written by a casting face the block is lit; that is asked of a byte map of the map's 8-by-8 tiles (`stile`, marked by pass 1 under each triangle's written box), four blocks in a row at a time.
  Otherwise the texels are read, and all-lit or all-dark settles the block; a block a shadow's edge crosses is lit pixel by pixel, four at a time (`table4`).
  A block wrongly called mixed costs time, never a pixel: `cn_scene_skip(8)` lights every open pixel alone, and the test holds the two pictures byte-equal at 1x, 2x and 3x, still and thrown.
- Pass 1's bands are cut on tile rows (8 map rows), so each band clears and marks only its own tiles; any number of bands still draws the one thread's bytes.

Before and after: `chuiniu/docs/shots/g1_bubble_shadow_before_after.png` (the bubble at 3x through `cn_stage`), `g1_table_shadow_before_after.png` (the six-seat table at 3x), `g1_dice_shadow_before_after.png` (the bench frame at 2x, dice).

What is left is the map's own resolution: a slanted edge keeps a gentle wave of about a third of a texel (under a point), which no filter of this width removes.

### Cost

Measured on this Mac under a load average near 250, so the minimums of thread CPU time over 100 frames, the old and new builds alternated (2x six seats, one thread):

| | before | after |
|---|---|---|
| shadow map pass | 2.55 ms | 2.65 ms |
| shade pass | 4.22 ms | 5.22 ms |
| whole frame | 20.21 ms | 21.37 ms |

`cn_scene_bench` at 2x, one thread: still 21.35 to 22.38 ms, throw 21.74 to 22.89 ms (+1.0 to +1.2 ms).
The 2x throw frame is the same bits on one thread, 4 bands and 16 bands.
`sample` before and after (the passes' functions kept out of line in a copy): the shade's own work fell (1,056 to 954 samples), the filter rose (`lit_of` 322 to `lit_q` 263 plus `table_block` 238), `ao_at` fell (181 to 120).
The frame takes 16 KB more (the tile bytes).
The study's module grows from 48,498 to 54,393 bytes (the filter and the block test inlined at -O3 with SIMD).

### Determinism

`cn_scene_test`'s goldens move on purpose, because every shadowed pixel changed: still 1x `0x78fcf817e8be90f8` (was `0x7c136d3580720f13`), throw 1.5x `0xb65b95f13c6d841a` (was `0xd79fc9ffeecb40dd`).
`cn_stage_test`'s move with them: throw `0x18318e6b` (was `0xc83b39dc`), still `0xe41ce178` (was `0xbc77a1a7`).
Apple clang -O2, GCC 16 -O2, and the wasm build with and without SIMD128 (the test frames linked into a module and run in node) give the same hashes.
`docs/UI.html` carries the new module (`make docs-roll`); `node docs/roll_wasm_check.mjs` loads it.

`docs-roll-check` had stopped working: it grepped the page for the module's base64 as one pattern, and at 70 KB grep runs out of memory, which read as "differs".
It now cuts the embed out of the page and compares it whole (checked red by editing one character of the embed).

## 2. The stage opens without the arena

`cn_stage_begin` never needed the arena: the layout, the meshes and the bakes are the handle's, and only `textures()` (called by a frame) uploads into the renderer.
What took the 48 MB was `cn_stage_init`, which opened the pack and attached the arena in one call, so the first begin of a process paid for it.

- `cn_stage_init(st, pack, len)` opens the pack only; `cn_stage_attach(st, arena, bytes)` gives the first frame its arena, and again after `cn_stage_purge`.
- `cn_api_stage_init(pack, len)` likewise; `cn_api_stage_attach` before the first frame.
- `BridgeStage.ready` (the minimal Swift change, in `ChuiniuKit/Kernel/BridgeKernel.swift`) inits once with the pack and allocates the arena only when a frame asks; no begin takes it, the first included.
  The memory-purge contract is unchanged: `purge` frees it, the next frame takes a new one and draws the same bytes.

No struct changed, so structgen's layout is the same.

## Tests added, each seen red

| Test | Mutation | Assertion that went red |
|---|---|---|
| `cn_scene_test` the open table's shadow edge is smooth at every scale | a mixed block's row lit by its first pixel's light | `:118` at 1x, 1.5x, 2x, 3x (1.54 to 2.05 px against 0.42 to 1.26); the goldens |
| same | the filter cut back to plain bilinear | `:118` at every scale (0.46 to 1.94 px) |
| `cn_scene_test` blocks taken whole draw what every pixel lit alone draws | `table_block` reads one texel too few each way | `:133` at 1x and 2x, still and thrown; the goldens |
| same | a triangle marks only the first tile of each row | `:133` 1x thrown, 2x; `:168`; `:189` 3 to 16 bands |
| `cn_scene_test` bands | pass 1's bands not cut on tile rows | `:189` 13 bands |
| `cn_stage_test` memory: no arena at all | `cn_stage_begin` refuses without an arena | `:244` begin with no arena, `:248`, `:250`; `cn_api_smoke.c:71` the reveal begins with no arena |

Each was applied alone by hand, the binary deleted before the run, restored by re-editing, and `git diff` was clean after.
The old renderer measured 3.1 to 3.9 device pixels on the edge test.

`make run asan wasm docs-roll-check ios-lib swift-smoke` and `chuiniu/ios/scripts/mac_tests.sh` (14 tests) pass.
