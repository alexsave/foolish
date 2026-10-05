# Package V1 report: 2x on every drawer, smooth silhouettes, Core Animation's own pixels, frames off the main thread

## 1. Memory: a frame keeps only its picture whole

### What was wrong

The renderer kept every buffer the size of the whole frame: the picture, the depth, and a G-buffer for the deferred shadow (texel tint, light's cosine, shade, crease, the place on the shadow map, the kind of fragment), about 25 bytes a pixel.
A still frame at 2x on the tall drawers did not fit the 48 MB arena beside the 12.2 MB texture set, so 390 by 718 drew at 1.5x and 430 by 830 on their turn (its far cups lean 361 points above the board) at 1x.

### What it is now

- Pass 2 (the picture) and pass 3 (the shade) run together a strip of `CN_SCENE_STRIP` (16) rows at a time.
  What pass 2 keeps for pass 3 (depth, tint, shade, crease, light, kind: 20 bytes a pixel) lives in a strip a band, one slot for each of the 16 bands, reused strip after strip.
- A frame keeps whole only its picture (4 bytes a pixel), each pixel's surface (`kb`, 1 byte) and the face that won it (`kf`, 2 bytes), which the edges pass reads across band boundaries: 7 bytes a pixel, and a quarter for the contact dark.
- Each drawn face's setup (corners on the screen, tangent frame, texture copy, tint) is made once in `cn_scene_prepare` (`Tri`), since a tall face now reaches several strips.
- `cn_scene_render` draws the shadow map as one band and every other pass in 16 bands in turn: the same bytes, and each strip reads only its band's faces.
- With the edges left out (`cn_scene_skip(16)`) the picture is the old renderer's byte for byte: the old goldens `78fcf817e8be90f8` and `b65b95f13c6d841a` are asserted that way in `cn_scene_test`.

### Memory per drawer (frame bytes beside the 12.2 MB texture set, 48 MB arena)

| drawer | scale | frame | with textures |
|---|---|---|---|
| 375 by 541 | 2x | 13.8 MB | 26.0 MB |
| 390 by 718 (bench frame, 40 above) | 2x | 26.2 MB | 38.4 MB |
| 430 by 830, my turn (296 above) | 2x | 30.5 MB | 42.7 MB |
| 430 by 830, their turn (361 above) | 2x | 34.3 MB | 46.5 MB |
| 390 by 718 (bench frame) | 3x | 39.9 MB | 52.2 MB: does not fit |
| 430 by 830, their turn | 3x | 55.9 MB | does not fit |

Every drawer now draws its still frame at 2x (`cn_stage_test` asserts both 430 by 830 turns and 390 by 718).
3x does not fit the tall drawers: the picture alone is 18.4 MB there, and its surface and face bytes another 13.8.
Still frames stay capped at 2x for a second reason: a peek animates on still frames, one a display frame, and 3x would be 2.25 times the work of each.

## 2. The edges: four samples where one surface meets another

### What was wrong

A pixel's colour was the one fragment at its centre, so every silhouette (a cup on the table, a die on a cup's floor, a crown against its side) was a staircase.

### What it is now

- A face carries its body's number in its flags (`CN_SCENE_F_ID(n)`, bits 8 up); `cn_stage` numbers every body, the study (`docs/UI.html`) and the test frame builder too.
  A pixel's surface is its face's body and texture, numbered 1 to 255 for the frame (`surface_of`).
- Pass 3 is THE EDGES: a pixel whose surface differs from a neighbour's above, below, left or right is drawn again from four samples on a rotated grid.
  Coverage: the faces that won the centres of the pixel and its four sides are tested at each sample (inside, nearest by 1/w, the earlier face on a tie as in pass 2), from a 40-byte plane record per drawn face (`Cov`).
  Colour: a sample of the pixel's own surface takes the pixel's colour; a sample of another surface takes the nearest neighbour's toward the sample that showed that face (else that surface), already shaded by pass 2's strip; only a sample no neighbour shows is shaded where it lies (`edge_shade`, out of line: none in the bench frame).
  A sample nothing covers is the table where the table showed in the 3-by-3 neighbourhood, else the pixel's own surface (a facet thinner than a pixel inside a body).
  The pixel is the four samples' mean by coverage.
- Every colour read is the shade's: a band holds each row's results until it has done the row after it, and its first and last rows (which the bands beside it read) to pass 4, THE COMMIT. So the bytes are the same for any band count (`cn_scene_test` bands test, 2 to 16, in reverse order).
- Only the surfaces' edges are sampled; a body's facets share its surface and cost nothing.

The silhouette of a slanted square strays 0.22 device pixels from its line against 0.48 to 0.50 from centres alone, at 1x, 1.5x and 2x (four samples quantise coverage to quarters).

Pictures: `chuiniu/docs/shots/v1_sim_cup_edge_before_after.png` (the simulator, expanded drawer: before E1's still frame at 1x, after V1's at 2x with edges), `v1_sim_cup_edge_zoom.png`, `v1_bench_cup_arc_before_after.png` and `v1_bench_dice_before_after.png` (the bench frame at 1.5x), `v1_study_cup_edge.png` (the study, same module).

### Cost (cn_scene_bench split, thread CPU, least of 200, one thread, quiet machine)

| | prepare | shadow map | picture and shade | edges | whole |
|---|---|---|---|---|---|
| 2x before | 0.39 | 2.76 | 17.54 | - | 20.93 ms |
| 2x after | 0.51 | 2.75 | 18.11 | 1.27 | 22.88 ms |
| 1.5x before | 0.38 | 2.75 | 10.79 | - | 14.16 ms |
| 1.5x after | 0.50 | 2.75 | 11.39 | 0.91 | 15.79 ms |

+1.95 ms at 2x, +1.63 ms at 1.5x.
How it got there, each step after a `sample` of the bench loop and its heavy lines:

- Walking each face over the edge pixels (rows, crossings, a pool) cost 11.6 ms; per-face rejection by tiles did not move it (the walk was intrinsic: 106,000 face-rows to test 94,000 pixels).
- Inverted: each edge pixel tests the faces of its neighbourhood (7.1 to 2.7 ms).
- The other surface's colour from the neighbour that showed it, not shaded again (61,000 shaded samples to 0).
- `shade` kept out of the strip loop (inlined it cost the picture 0.5 ms).
- The coverage test as planes about one corner on one cache line, compared by 1/w (no divide); the candidate gather branch-free; the surfaces a byte so the scan compares 16 pixels a vector and walks the set lanes; the scan only over each row's walked columns; five candidates instead of nine (783 of 17,000 edge pixels changed by a step or two at 1.5x).

The picture-and-shade line's +0.57 ms is the strips (faces set up once per strip they reach, the surface and face bytes written); 8-row strips cost 0.6 ms more, 32-row strips do not fit the tall drawer.

## 3. Core Animation's own pixels

`cn_scene_output(form)`: `CN_SCENE_OUT_RGBA` (straight, the default: the study's `putImageData`), `CN_SCENE_OUT_PREMUL_RGBA`, `CN_SCENE_OUT_PREMUL_BGRA`.
The iOS stage asks for the last (`cn_api_stage_output(CN_API_STAGE_CA)`) and tags the CGImage `premultipliedFirst | byteOrder32Little`.
Premultiplied RGBA alone was not enough: Core Animation still redrew the image through vImage on every commit; BGRA with alpha first is its own layout.

`sample` of the extension through a throw (3 s), main thread:

| form | main-thread samples | `CA::Render::prepare_image` | of it, vImage conversion |
|---|---|---|---|
| straight RGBA (before, `dev.straight`) | 1914 | 94 | 29 |
| premultiplied BGRA (after) | 1984 | 34 | 0 |

What is left under `prepare_image` is Core Animation copying the image into its own buffer (a memmove); an IOSurface as the layer's contents would remove that copy too (not done).
The present (the commit) measured in the frame log: 0.8 to 0.3 ms compact, 3.2 to 1.4 ms expanded (medians).

## 4. Off the main thread

- `BridgeStage` keeps every renderer call (begin, frame, the arena, purge, the bubble) on one serial queue, in `StageWorker`, which alone owns the arena; frames are drawn one at a time, in the order asked.
- `TableStage.submit(atMs:peek:then:)`: the clock, the peek and the reveal's lift (the resident plan's, `cn_api_stage_lift`, which only the main thread may read) sampled at the submit; the frame drawn on the queue (`cn_api_stage_prepare_at`, bands by `concurrentPerform`); the picture lands on the main actor.
- `StageDirector` keeps one frame in flight (`requestFrame`), puts it up when it lands, and drops a frame asked before the latest begin; the display link runs while a frame is in flight.
- A begin, a purge or `frame` (the bubble, the tests) waits on the queue for the frame in flight, so a purge during a draw is safe.

### On the simulator (iPhone, 440 by 956 points, six seats, harness throw, the rig; medians / p90 / max over 200 to 230 throw frames, two takes each)

| | main thread a display tick | present | main thread a frame | ask to picture |
|---|---|---|---|---|
| compact, before (drawn on main, straight) | 5.0 / 5.7 / 12.8 | 0.8 / 0.9 / 2.8 | 5.8 | 4.1 / 4.8 / 11.6 |
| compact, after | 0.0 / 0.1 / 0.1 | 0.3 / 0.5 / 0.6 | 0.3 | 4.2 / 5.1 / 33.2 |
| expanded, before | 11.7 / 13.5 / 16.1 | 3.2 / 3.9 / 7.2 | 14.9 | 8.3 / 9.4 / 12.2 |
| expanded, after | 0.1 / 0.1 / 0.1 | 1.4 / 1.9 / 4.7 | 1.5 | 8.4 / 9.6 / 34.8 |

Both columns are the same build: `dev.syncframes` draws on the main thread as before and `dev.straight` asks for the old pixels (Debug only, read once a process); the Debug log line `stage frame ... land= present=` and `stage tick main=` is what the table is made of.
The 30-odd ms maximum after is the frame queued behind the first one (the texture upload).

## Tests, and each seen red

| test | mutation | red at |
|---|---|---|
| `cn_scene_test` pinned table (`658558ffc67caa06`, `2ffa90346b8d21d5`) | every sample the pixel's own (M1); bodies not told apart (M2); facets as surfaces (M3); a band's first row written at once (M6); the scan a pixel short (M7); an uncovered sample always the table (M8) | `:228`, `:231` |
| edges: the slanted side | M1 | `:290` 0.50 pixels at 1x, 1.5x, 2x |
| surfaces: facets never an edge, two bodies are | M2 | `:322`; M3 `:303`, `:322` |
| premultiplied and BGRA | table pixels left straight (M4) | `:352`; body pixel unswapped (M11) `:345` |
| memory: 7.25 MB a million pixels | the surface byte kept as two (M5) | `:376` |
| bands: any count, any order | M6 | `:258` at 2, 3, 7, 13 bands |
| `cn_stage_test` goldens (`95a28700`, `5b91b1fe`) | the stage numbers no body (M9) | `:363`, `:364` |
| `StageViewTests` off the main thread, one at a time, in order, BGRA | submit draws at once (S1) | `:308`; queue concurrent (S2) `:311`, `:313`, `:316`; pixels straight (S7) `:328` |
| purge during a draw | purge without the queue (S3) | the test process crashed (the arena freed under the draw) |
| one frame in flight at its clock | no in-flight guard (S4) | `:369`, `:370`, `:373`; clock sampled late (S5) `:373`, `:385` |
| a frame of an older begin dropped | no generation bump (S6) | `:402`, `:406` |

Clang, GCC 16 and wasm (with and without SIMD128) draw the same four hashes (still 1x, throw 1.5x, premultiplied 1.5x, throw 2x).
`make run asan wasm docs-roll-check ios-lib swift-smoke`, `node docs/roll_wasm_check.mjs` and `chuiniu/ios/scripts/mac_tests.sh` (lint, 27 unit tests, the shipping build) are green.
