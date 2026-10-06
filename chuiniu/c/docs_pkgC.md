# Package C - the texture pack

The cups and dice in the study (`docs/UI.html`, `TEX` and `TEX3`) wear textures made by browser canvas code.
This package moves them into C: the expensive noise is baked at build time, and every per-seat and per-die texture is derived from the baked tiles when it is uploaded.

## Files

- `tools/cn_texgen.c` - the host-only baker (libc and stdio, no CoreGraphics, no libm).
  It also carries the comparison against the study and the contact sheet writer (`--compare`, `--sheet`).
- `tools/fonts/IMFeENrm28P.ttf` and `tools/fonts/OFL.txt` - IM Fell English roman, SIL OFL 1.1, from `google/fonts` (`ofl/imfellenglish`).
- `tools/cn_tex_capture.mjs` - the measuring harness that captures the study's own textures in headless Chromium; never shipped, never in `make run`.
- `src/cn_tex.h`, `src/cn_tex.c` - the freestanding runtime (memcpy / memset only, no libm, no heap).
- `tests/cn_tex_test.c` - the tests; the baker is compiled in with its `main` left out.
- The `make tex` block at the end of `chuiniu/c/Makefile`.

## Why baked

A procedural texture rendered at launch once took the iMessage extension down on a real device.
So the fbm (the only expensive part) runs once, in `make tex`, and the runtime only reads and copies.

## What the pack holds

| Record | Size | Bytes |
|---|---|---|
| verdigris tile (`TEX.verdWith(VERD)`) | 256 x 256 RGB | 196,608 |
| bone tile (`TEX.stone(DIE_MATS.tallow)`, the die's default material) | 128 x 128 RGB | 49,152 |
| numerals 0-9 at 112 and 184 texels, coverage masks | 20 glyphs | 98,708 in all |
| header (24) + 22 records (20 each) + 4-byte alignment | | 476 |

The pack is **344,944 bytes** raw (117,003 gzipped), against a 4 MB budget the test enforces.
It is stored raw: decompression would buy 228 KB of disk and cost a decoder in the freestanding runtime.
One verdigris tile is shared by every seat: a seat's side, inside, floor and crown are that tile at the seed's own offset with the study's gradient, numeral or tint, made at upload, never stored.
Every die's atlas is likewise the one bone tile at six offsets, with the strike wear and the pips drawn at upload.

The layout is in the header comment of `src/cn_tex.h`: a 24-byte little-endian header (magic `CNTX`, version 1, entry count, total length, FNV-1a 32 over everything after the header), then 20-byte records (kind, bytes a texel, digit, font size, width, height, a glyph's offset from the pen's anchor, the block's offset), then 4-aligned blocks.
`cn_tex_pack_open` refuses a short, mismatched, damaged or incomplete pack and points into the caller's bytes without copying.

## The runtime

- `cn_tex_cup_side`, `cn_tex_cup_inner` (1024 x 512), `cn_tex_cup_floor`, `cn_tex_cup_crown` (256 x 256), `cn_tex_die_atlas` (768 x 128) write a texture into the caller's RGBA.
- `cn_tex_stamp_numeral` blits the count's shadow and face at the study's anchors; `cn_tex_tint` lays a flat colour over a texture (the "out" crown).
- `cn_tex_relief` and `cn_tex_die_relief` are the study's `reliefOf` and `dieHeight`: an int8 (dx, dy) normal map at a 20th, the format `cn_scene.c` takes.
- `cn_tex_mip` is the renderer's own 2-by-2 rounded mean, for a host that makes the chain itself.
- `cn_tex_upload` takes a `CnTexSink` (a context and an `alloc(w, h, has_bump) -> id` callback filling a `{w, h, rgba, bump}`), so it never names the scene.

Upload memory, per seat at full size: side 2 MB + 1 MB relief, inside the same, crown and floor 256 KB + 128 KB each, a die 384 KB + 192 KB.
That belongs to the renderer's arena, not to this package; the pack itself adds 345 KB.

## Determinism

The baker runs the study's arithmetic in IEEE double in the JavaScript's own order of operations, with `-ffp-contract=off`, and stores each texel the way `ImageData` does (clamped, rounded half to even).
The runtime is integer arithmetic plus double `+ - * /` and `__builtin_sqrt`, each correctly rounded on every IEEE target.
The goldens in `cn_tex_test.c` (the pack, seed 7's side, crown and die atlas, and the die's relief) come out identical under Apple clang at -O0, -O2, -O3 and under ASan/UBSan, under GCC 16, and in a wasm32 build run in node.

## Against the study

`make tex-compare REF=dir` compares every texture with a capture from the study in headless Chromium (seeds 1, 7 and 42; side, inside, floor, crown with a 3 at 112, a 5 at 184, an out crown, and the die atlas).
Mean absolute difference, on the 0-255 scale, per RGB channel:

| Texture | MAD | Max | Relief MAD (int8) |
|---|---|---|---|
| verdigris tile | 0 | 0 (every texel identical) | - |
| bone tile | 0 | 0 (every texel identical) | - |
| side | 0.014 | 1 | 0.019 |
| inside | 0.212 | 1 | 0.002 |
| floor | 0.215 | 1 | 0.001 |
| crown, 3 at 112 | 0.347 | 160 | 0.032 |
| crown, 5 at 184 | 0.403 | 129 | 0.035 |
| crown, out | 0 | 0 | 0.001 |
| die atlas | 0.096 to 0.110 | 3 | 0.049 |

The tolerance is 1.0 of 255; the worst texture is 0.403.
The ones of max 1 are the canvas's own gradient rounding.
The crowns' max is at the numeral's edge: Chromium on macOS draws text through CoreText's font smoothing, which thickens a stroke by about a quarter of a texel each side (the reference glyphs cover 7 to 16 percent more texels than the exact outline); the shapes and placement agree to about 0.2 of a texel.
The pack keeps the exact outline: an iOS host draws text without that smoothing, so the browser's weight is an artifact of where the study ran, not of its design.
The contact sheet (reference, C, difference times four) is `build/cn_tex_sheet.png`.

The numeral's placement is the canvas's: `textAlign = 'center'` puts the pen at minus half the advance, and `textBaseline = 'middle'` is the middle of the em box Chromium derives from OS/2 `sTypoAscender` and `sTypoDescender` (measured 418.9 units of 2048 above the baseline, the formula gives 418.92).

## Tests

`make tex-test` (in `make run`; `tex-asan` in `make asan`): 367 assertions.
The study's hash against values from its JavaScript run in node; two bakes are identical and match the pinned golden and length; the layout arithmetic and the 4 MB budget; the file `make tex` wrote is this bake; a flipped texel, a bad magic, a bad version and a truncation are each refused; the same seed gives the same texture, two seeds different ones, and a side is the tile at the seed's offset texel for texel; every numeral at both sizes lands inside the crown's circle (radius 120 of 128), stamps enough texels and is centred on the anchor; an out crown is dark and carries no numeral; over 40 seeds, cell k of a die atlas has k dark clusters; the 1's pip is red; the relief of a flat texture is zero and of a ramp is the study's number, edges clamped; a mip is the rounded mean; an upload through a sink matches the direct calls and a full sink returns -1.

## Mutation checks

Each was applied, rebuilt from clean (so make's same-second mtime could not keep a stale binary), run, and restored by rewriting the original text.

| Mutation | Went red on |
|---|---|
| a hash multiplier changed | "hash is the study's" |
| the pack's check not verified | "a flipped texel" |
| the numeral's anchor 72 texels lower | crown golden, "past the crown" |
| face 4 draws three pips | die golden, "face 4: 3 pips" |
| the verdigris seed 37 -> 38 | pack golden |
| the relief's sign flipped | die relief golden, "ramp slope" |
| the mip rounds with +1 instead of +2 | "not the rounded mean" |
| the die upload skips its relief | "die upload" |
| the side's offset from the wrong hash bits | side golden, "not the tile's" |
| an out crown also stamps the numeral | "an out crown carries a numeral" |
| the middle baseline at the em top | pack golden |
| the 1's pip in the plain pip colours | die golden, "the 1's pip is not red" |

## Not done here

- The renderer is not wired to the pack: `cn_scene.c` belongs to package A, and `cn_tex_upload` is ready for its `scene_tex_new` / `scene_tex_rgba` / `scene_tex_bump` behind a sink.
- `make tex-ios` copies the pack into `ios/ChuiniuKit/Resources/`, but no Swift reads it yet.
- Only the default die material (tallow) is baked; the study's other bone palettes are mixer options, not the locked look.
