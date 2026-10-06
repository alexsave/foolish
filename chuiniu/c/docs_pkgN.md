# Package N report: the names lie on the table

The owner: "the player names should be behind the cup when it is necessary. like they should act as objects in 3d".

## What was wrong

A seat's name and its turn bar were CATextLayers in the host's turned layer, painted over the kernel's picture.
A cup in front of a name never hid it, and a cup's shadow never fell on it.

## What it is now

- **The renderer has decals** (`CN_SCENE_F_DECAL`, flag 16, `cn_scene.h`).
  A decal is a face whose texture is premultiplied RGBA with its alpha respected.
  It is depth tested like any face, so a body in front of it hides it.
  With `CN_SCENE_F_RECEIVE` it takes the shadow map and the contact dark the table under it would, and its pixel is its texel laid over the table's own dark: premultiplied `f c + a K` at alpha `a + f d` (`decal_word`), so the planks show round the letters in both output forms, a texel with no alpha is the bare table's very bytes, and an opaque letter stays opaque in a shadow (darker, not more transparent).
  The edges pass and its edge samples treat a decal as a surface of its own.
- **Decals do not hide decals.** Two decals in the same plane at a pixel (two names' halos that meet on a short board, a glow over a letter) are laid one under the other, premultiplied, instead of the later one losing the depth test with its empty halo.
  The study showed the bug first: its bar's halo cut the bottoms off the letters above it.
- **Textures that change**: `cn_scene_tex_mark` / `cn_scene_tex_drop` give back the textures made after a mark (a name's), and `cn_scene_tex_mips` makes a texture's half-size copies at once.
  The stage's warm frame (one throwaway frame drawn only to make the copies) is gone: the set's copies are made by `cn_scene_tex_mips`, one rule with one owner.
- **The stage owns where a name lies** (`cn_stage_name`, `cn_stage_name_rect`): the host hands over the bitmap and its size in points; the stage sets the block (the bitmap less `CN_STAGE_NAME_HALO`, 8 points) on cn_lay's anchor by `name_how`, exactly as the old layers did, and lays the quad flat at `CN_STAGE_NAME_Z` (.3 points), one body number per name.
  The stage keeps its own copy of each bitmap, so a purge's rebuild asks nothing of the host.
  `names_upload` is the one place that decides which names a frame draws (a name is drawn exactly when it has a texture; none in a bubble).
- **A name never costs the picture its scale**: names are uploaded only while the arena still holds the still frame at the scale it would get without them (the reveal's with every cup up); one that does not fit is skipped, never a crash or a blank frame.
- **The seam**: `cn_api_stage_name(seat, rgba, w, h, w_pt, h_pt)`; Swift's `TableStage.name(seat:bitmap:)` runs it on the stage's queue, in order with the frames.
- **Text stays the host's.** `NameDecal.swift` draws a name (IM Fell English SC, 14 or 12 on a short board, tracked .14em, the old layer's black shadow a point down, the ink as before: bright on the turn, dim, the glow for the winner, .6 once out) and the turn's bar with its glow, at three texels a point, premultiplied.
  `NameDecals` hands a seat's name over only when its look changes.
  The CATextLayer names and bar layers are gone from `StageUIView`; the reveal's loser stamp stays as it was (positioned from `NameDecal.block`), and so do the accessibility elements.
- **The study** (`docs/UI.html`) does the same: each `[data-decal]` name and turn bar is drawn with canvas text into a premultiplied texture and laid as a decal (the same flag) where the page laid it out, measured untilted (and once its tab is laid out); the page's element keeps its place, unseen.
  The study needs the module re-embedded (`make docs-roll`); this branch leaves the base64 line as it found it.

## Cost and memory

`make names-shot` (thread CPU, the least of 15, one thread, the Mac in use; the bands divide this):

| still frame, six seats, 2x | no names | six names |
|---|---|---|
| 390 by 718 (968 by 2034 pixels) | 26.37 ms | 27.15 ms |
| 430 by 830, their turn (1100 by 2438) | 33.66 ms | 34.22 ms |

Six names with their half-size copies: 0.55 MB (the tool's), 0.79 MB (`cn_stage_test`'s wider synthetic names).
The worst drawer, 430 by 830 on their turn at 2x: the set 12.25 MB, the names 0.79, the frame 34.08: 47.11 of the 48 MB arena.
The stage's handle grows by 1.5 MB of name copies (6 by 512 by 128 RGBA), untouched beyond each name's bytes (`cn_stage_init` does not zero them).

## The occlusion proof

`cn_stage_test` "the names lie on the table": over 390 by 718, 430 by 830 and 375 by 541, the table on both turns and the reveal, 4 to 6 seats, every texel of a name's rect where the bare frame shows a cup (opaque, opaque all round) is the bare frame's byte for byte (none differs), and some names are half hidden, half shown.
`cn_scene_test`: a cup's side standing in front of a decal covers 1,408 of its pixels, each the cup's own; without the cup those pixels are the ink.
Pictures: `chuiniu/docs/shots/n_name_behind_cup.jpg` (the reveal on 390 by 718, Dee's name half behind a lifted cup; the same table without names on the right), `n_study_names.png` (the study's table with its names as decals; my cup's shadow now falls on my name).

## Goldens

No golden moved: every frame without a name is the old bytes (`cn_scene_test` 658558ff.. and 2ffa9034.., `cn_stage_test` 563a3970 and 50980f05 unchanged; the warm frame's removal changes no pixel).
New pins: the name frame `51c7e76864cdb7eb` (`cn_scene_test`, clang, gcc 16 and wasm with and without SIMD128 alike), the stage with six names `a48f41f0` (throw) and `44078389` (still peeking), clang and gcc 16 alike.

## Tests, each seen red

| test | mutation | red at |
|---|---|---|
| scene: the cup hides the name | decals skip the depth test (M1) | "the cup's side covers the name: 0 of 1408", the pin |
| scene: shadow on a letter, the shadowed gap is the table's bytes | decal never shadowed (M2); no light place kept (M12) | "in the roof's shadow the letter is darker", "between the letters in the shadow" |
| scene: alpha 0 between letters, half alpha | texel alpha forced 255 (M3) | "between the letters in the light: alpha 0", half-covered |
| scene: straight / premultiplied / BGRA | straight not divided by alpha (M5); BGRA not swapped (M6) | half-covered, "agree at every pixel" |
| scene: the ink exactly | texel truncated, not rounded (M11) | "without the cup those pixels are the ink (1061 of 1408)" |
| scene: two names meet | no laying under (M13) | "the first's letter under the second's empty halo" |
| scene: mark and drop | earlier copies kept after a drop (M7) | "made its copies again, not read from the new texture's bytes" |
| stage: arena failure path, the reserve follows the table | reserve ignored (S2) | "no room for a name", "room for two", "begun again: two" |
| stage: the same names change nothing | no sameness check (S3) | "the same names again" |
| stage: the bubble | names uploaded in a bubble (S4, after the second guard was removed: it was a double band-aid) | "the bubble draws no names" |
| stage: occlusion | names lifted to z 80 (S5) | "every pixel of a name under a cup is the cup's (3288 differ)", pins |
| stage: placement | a tall board's own name set top on the anchor (S7) | the pins |

`S6` (begin no longer re-uploads names) went green until the reserve test was added; it now goes red there.
Swift (`StageViewTests`: the picture's size, non-empty, premultiplied, bright against dim, the bar only on the turn; the seam called once a seat and again only on a change, the frame with and without names) passed on the simulator before the coordinator's merge; its mutation runs (no change check, no bar, one ink, the bridge not calling the seam, the view not handing names) were cut off when the machine was needed, and no simulator was booted after, so they are not recorded as red.
After the merge the Swift test target builds (`build-for-testing`), `lint_architecture.sh` is clean, `swift-smoke` and `ios_smoke` pass.
