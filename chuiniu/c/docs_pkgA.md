# Package A report: the scene renderer, native on iOS

For the coordinator to fold into `chuiniu/docs/DECISIONS.md` (K14 and "iOS and rendering").
Measured 2026-10-05 on an Apple M1 (4 performance and 4 efficiency cores), clang -O2 as `ios-lib` builds.

## What moved

`chuiniu/c/wasm/cn_scene.c` is now `chuiniu/c/src/cn_scene.c` (git mv, history kept) with a header `src/cn_scene.h`.
It is in `SRC`, so every test links it, `make wasm` proves it freestanding, and `make ios-lib` puts it in the device arm64 and simulator arm64 + x86_64 slices.
`ios-lib` copies `cn_scene.h` into the xcframework's headers beside `cn_api.h`, and after the build asks each slice for ten `cn_scene_*` symbols (it fails naming the slice when one is missing; checked by building without `cn_scene.c`).
Swift does not see the header yet: module `CChuiniu` lists only `cn_api.h`, and `cn_api.h` / `module.modulemap` are the bridge package's, so the bridge must `#include "cn_scene.h"` from `cn_api.h` (or list it in the module map).
`wasm/cn_scene_web.c` is the browser's side: its 256 MB static arena and the `scene_*` export names `docs/UI.html` calls, so the page is unchanged.
The study's module is now built with `-msimd128` (Chrome 91, Firefox 89, Safari 16.4).

## API (`src/cn_scene.h`)

- `cn_scene_init(mem, bytes)`: the caller's arena; the host owns it and may free it whenever no call is running (on backgrounding), after which `cn_scene_init` comes first again.
- `cn_scene_tex_new / _rgba / _bump`, `cn_scene_begin`, `cn_scene_verts / _faces`, `cn_scene_occluder`, `cn_scene_render`, `cn_scene_fb / _w / _h`, `cn_scene_prof`, `cn_scene_skip`: the old calls under `cn_scene_` names, same arguments.
- `cn_scene_frame_bytes(...)` and `cn_scene_room()`: what a frame of given numbers takes and what the arena has left after the textures, read from the one list `cn_scene_begin` takes from.
- `cn_scene_prepare(nverts, nfaces)` then `cn_scene_band(pass, band, nbands)` for each of `CN_SCENE_PASSES` passes in order, a pass's bands on any threads at once (on iOS, `DispatchQueue.concurrentPerform(iterations: nbands)` per pass).
  Each band writes only its own rows; the picture is the same bits for any band count (1 to 16).
  `cn_scene_render` is `cn_scene_prepare` plus one band a pass.
- One renderer a process (module state); the memory is the caller's.

## Arena budget

`CN_SCENE_ARENA_IOS` is 48 MB and `CN_SCENE_ARENA_WEB` 256 MB.
A frame that does not fit fails cleanly: `cn_scene_begin` returns 0, `cn_scene_fb/verts/faces` return 0, `cn_scene_render` returns -1, and a smaller frame draws after it.
A texture that does not fit takes nothing (a refused normal map gives its colour back).

| scale (390 x 718 board, 40 above) | framebuffer | frame bytes |
|---|---|---|
| 1x | 390 x 758 | 13.4 MB |
| 1.5x | 585 x 1137 | 22.0 MB |
| 2x | 780 x 1516 | 33.9 MB |
| 2.25x | | 41.2 MB |
| 3x | 1170 x 2274 | 68.1 MB |

Textures: the study gives every seat its own cup side, inside, crown, floor and every die its own atlas; one table of those is 43.9 MB uploaded and 57.7 MB with the half-size copies, which no 48 MB arena holds.
One shared set (one of each) is 9.6 MB with copies, so 48 MB holds a 2x frame (33.9 + 9.6 = 43.5) and not 2.25x.
The host should share one texture set, or a very few.

## Performance

The six-seat frame: six full-size cups (wall, fillet, crown, inside, rim), five dice a seat, 12,864 faces, contact footprints, the study's light and eye, textures of the study's sizes whose normal maps carry the study's measured statistics (flat but for one texel in twenty).
Mean and p95 over 100 frames, still and in a throw.

| scale | one thread, before | one thread, now (p95) | 2 bands | 4 bands | 16 bands |
|---|---|---|---|---|---|
| 1x | 8.9 ms | 9.1 (9.7) | 7.1 | 6.1 | |
| 1.5x | 15.1 ms | 14.2 (15.1) | 10.6 | 8.4 | 4.8 |
| 2x | 24.0 ms | 21.0 (22.8) | 15.5 | 11.7 | 6.8 |
| 3x | 47.3 ms | 37.7 (39.3) | 27.3 | 19.7 | 12.0 |

"Before" is the original file run through the same bench; it draws the same hash on the bench's frame as the new one.
In the study (Chrome, wasm) a 699 x 1521 throw frame went from about 20 ms to 18.
A 2x frame on one thread: the picture 13.8 ms, the shade 4.3, the shadow map 2.7, clears and setup 0.8.

What the profile showed and what changed (macOS `sample`, then the hot lines and the assembly):

- The picture pass was three quarters of the frame, about 100 cycles a walked pixel.
  Its stores are bytes, which may alias every global, so each pixel reloaded the triangle's corners and the buffers' addresses and stored the counters; the loops now read only locals (no gain alone, measured).
- Knocking pieces out showed the cost spread over the normal map, the light's place, the texel fetch (3 ms each) and the bare interpolation (10 ms): arithmetic, not one hot spot.
  The picture now runs four pixels at a time in the compilers' vector types (NEON, SSE, wasm SIMD), with masks for the span's end and the depth test, the bump bytes as one 16-bit load, and a span's interior stored with one write a buffer.
  Every lane does the scalar operations in the scalar order, so the bits are the same.
- A triangle's edges are worked out once, not on every row; an open table block row is written four pixels at a time.
- The rest is the cores: the frame cut into horizontal bands.
- The study's normal maps measured 94 to 96% zero; the first synthetic maps were uniform noise, which made the light's branches random and cost 3 ms that no real frame pays.
  The bench now uses the measured statistics.

## Recommendation for an iPhone

Assumption, as given: an iPhone core is 1.5 to 2 times slower than an M1 performance core; an iPhone has 2 performance and 4 efficiency cores (about 3.4 performance cores' worth, against the M1's 5.4).
Then on one thread a 2x frame is 31 to 42 ms and a 1.5x frame 21 to 28 ms; in 16 bands roughly 14 to 19 ms at 2x and 9 to 13 ms at 1.5x.

- Throw frames: 1.5x, in 16 bands (the study's own rule: a roll at 1.5 a point at most). That holds 60 fps with room on the assumption, and 30 fps on one thread.
- Still frames: 2x, drawn once when the table changes (one 2x frame is 14 to 42 ms, not a frame rate).
- Never 3x: 68 MB of frame.
- Cap 2x by memory too: 2x plus one shared texture set is 43.5 MB of the 48.

The 16 ms gate at 2x on this Mac is met in bands (2 bands 15.5 ms, 4 bands 11.7, 16 bands 6.8) and not on one thread (21.0).
Fallbacks, measured: 1.5x instead of 2x saves a third (21.0 to 14.2 on one thread); 1x saves another third (9.1); leaving the shadow map out of a throw frame saves 2.7 ms at any scale but loses every cast shadow (cups on the table, dice in the cup), which the study treats as the point of the renderer, so it is not recommended.

## Proof the picture is the study's

- 196 frames captured from `docs/UI.html` before any change (headless Chrome over CDP: every `scene_render`'s inputs and the framebuffer wasm drew) replay through the native build byte for byte, before and after every change.
- After the change, every study frame whose inputs match a captured one matches it byte for byte (68 to 96 frames a capture; the rest are the throw's animation, timing-dependent), and all 292 frames of the final capture replay natively byte for byte.
- `cn_scene_test` pins the six-seat table's picture at 1x still and 1.5x in a throw (FNV-1a of the framebuffer); clang, gcc 16 and the wasm build (with and without SIMD) give the same hashes.
  The simulator's x86_64 slice compiles but was not run (this Mac has no Rosetta).
- ThreadSanitizer: the 16-band and 7-band loops ran clean; at the frame's right edge the depth buffer is read one pixel at a time, so no band touches another's row.

## Fixed on the way

- `docs-roll` and `docs-roll-check` passed a file to `base64` as an argument, which this macOS refuses, so `docs-roll` would have embedded an empty module; they read stdin now.
- The old `scene_begin` could return a frame with a null contact buffer, and never checked that the face order fitted (a crash on a frame that nearly fitted); every buffer is checked now.
- Faces past 65,536 were never drawn (a fixed chain array); the chains are in the arena.
- A face with an index past the vertices, negative or NaN read outside the array; it is skipped.
- A shadow map past 1,024 overflowed the 16-bit place on it; it is refused (`CN_SCENE_SHADOW_MAX`).
- Footprints and spans far off the screen converted out-of-range floats; they are held first, giving the same columns and blocks.

## Mutation checks (`cn_scene_test`, each applied alone, the binary deleted before each run, restored by re-editing, `git diff` clean after)

| Test | Mutation | Went red |
|---|---|---|
| the quad, its shadow | the shadow map is never written | `:50` the table under the square is darkened, `:101`, `:104` |
| the quad, its shadow | the depth buffer is never cleared | `:46`, `:54`, `:90`, `:101`, `:104`, `:107` the same frame twice |
| the quad | textures refused while a frame is open | `:58` a texture between frames |
| a stray index | `face_ok` takes every index | `:69` four good faces drawn (5), `:70` |
| past 65,536 faces | the bucket chain stops at 65,536 | `:89` every face drawn (65536), `:90` the quad is in the picture |
| the pinned table | the shadow bias 1.6 becomes 1.5 | `:101` still at 1x, `:104` throw at 1.5x |
| bands | a band draws one row past its end | `:128` walked counts differ at 2, 3, 7, 13, 16 bands (the picture alone did not change: overlap redraws behind the depth test) |
| bands | a band runs without `cn_scene_prepare` | `:133` |
| bands | the band range checks removed | `:138` bands out of range draw nothing |
| clean failure | `cn_scene_begin` keeps going past a failed take | `:148` 3x does not begin, `:149`, `:150`, `:157`, `:158` |
| clean failure | `cn_scene_frame_bytes` leaves out one buffer | `:156` exactly its bytes: it begins |
| clean failure | a refused normal map keeps its colour's bytes | `:166`, `:167` |
| numbers | no `H < 1` | `:172` a board of no height |
| numbers | no `pad < 0` | `:173` a pad upward |
| numbers | no lower bound on the height | `:175` a negative scale |
| numbers | no upper bound on the width | `:176` past 8,192 a side |
| numbers | no lower bound on the width | `:177` a board of no width, `:178` round to no pixel |
| numbers | the shadow cap 1,024 becomes 8,192 | `:182` |
| numbers | no light accepted | `:180` |
| no arena | a null arena is taken | `:184` |
| `ios-lib` | built without `cn_scene.c` | "build/ios/device/libchuiniu.a does not define cn_scene_init" |

Two guards were found never to be alone and were removed rather than tested: the scale guard `dpr > 0` (a board at least a point high leaves no rows at any scale of zero or less, and a NaN scale fails the size test) and `W < 1` (the computed width covers it).
`:181` a negative capacity cannot go red alone: a negative count wraps to a size no arena has, so the arena refuses it too; the guard states the rule.
Line numbers are `tests/cn_scene_test.c`'s as committed.
