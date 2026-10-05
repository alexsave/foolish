# The icon

The placeholder icons of DECISIONS I7 (two dice on felt green, drawn by a throwaway script) are replaced by the table's own cup and die, drawn by the game's own renderer.

## What it is

One verdigris cup, mouth down, tipped a little toward us about the far edge of its mouth (the peek's turn, `cn_cam_peek_tilt`, 0.2 radians).
One bone die in front of it and to the left, a 5 up, turned so two side faces show.
The table is the deep's teal (the study's `.sea` / `.icon` colours) with a pool of cold light behind the cup and to its right, so the cup's shaded side stands against the brightest of it, and the deck's vignette at the edges.
One light, high on the left and a little behind; the shadows fall to the right and are softened.
No numeral on the crown, no pirate anything, no published product's cup or box (`chuiniu/LEGAL.md`).

Every surface is the game's: the meshes are `cn_geom_cup_mesh` and `cn_geom_die_mesh`, the textures come from the same baked pack the app reads (`build/cn_tex.pack`, through `cn_tex_upload`), and the picture is `cn_scene.c`'s rasterizer and shadow map.

## How it is drawn

`chuiniu/c/tools/cn_icon.c` is a host-only tool (zlib for the PNGs), never linked into the app.
It follows uttt's pattern (`uttt/c/tools/uttt_icon.c`): one tool, every size drawn at its own pixel size, never shrunk from the 1024.

The renderer draws what an eye over the table sees on the table's plane (a shifted lens).
A hero shot wants an eye low and in front, looking at the cup.
Both are the same rays from the same eye, so the tool puts the renderer's eye where the camera is, renders the table-plane picture of the part of the table the frame sees, and for every output pixel follows 4 by 4 rays down to z = 0 and reads the picture there (bilinear).
That is the same homography the game's leaning head applies to its screen (`cn_cam.h`); a body's pixel is where its ray says.

The frame is fitted to the bodies' extent as the camera sees them, with a tenth of the frame clear at the tighter edges, so the square and every 4:3 size hold the same subject.

Two things are the tool's own and not the renderer's:

- The shadow is softened: the table's pixels (alpha below 255) are blurred among themselves only, three box passes of 2.5 points, weighted so no body's silhouette bleeds a halo onto the table.
- The table's colour and the vignette are painted under and over the renderer's output (the renderer draws the table only as shadow with alpha).

The output is opaque RGB (the App Store refuses an icon with alpha) and byte-identical from run to run on this Mac.

## Files

| File | Size | Catalogue slot |
|---|---|---|
| `ChuiniuMessages/Assets.xcassets/iMessage App Icon.stickersiconset/sq-58.png` | 58 x 58 | 29pt @2x (iPhone, iPad) |
| `.../sq-87.png` | 87 x 87 | 29pt @3x |
| `.../msg-120x90.png`, `msg-180x135.png` | | 60x45pt @2x, @3x (Messages app drawer) |
| `.../msg-134x100.png`, `msg-148x110.png` | | iPad 67x50pt, 74x55pt @2x |
| `.../msg-54x40.png`, `msg-81x60.png` | | 27x20pt @2x, @3x |
| `.../msg-64x48.png`, `msg-96x72.png` | | 32x24pt @2x, @3x |
| `.../msg-1024x768.png` | 1024 x 768 | ios-marketing |
| `ChuiniuMessagesApp/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png` | 1024 x 1024 | the container app |

The two `Contents.json` files are unchanged: the files keep their names and sizes.

## Regenerate

```
make -C chuiniu/c icons
```

It builds `build/cn_icon` and the texture pack if needed, writes every PNG straight into both catalogues, and writes a contact sheet to `chuiniu/c/build/cn_icon_sheet.png`.
Commit what it changes.
The sheet shows every small size at its pixel size on Messages' dark and light drawer (with a rough rounded mask), the 29pt and 27x20pt icons at 1x below them, and the two 1024s at half size.

One frame for experiments: `./build/cn_icon --pack build/cn_tex.pack --one W H out.png`.
The composition is the block of constants at the top of `cn_icon.c` (cup radius, die side and place, tip, seeds, camera elevation and azimuth, light, pool, margin, softness).

## Legibility at 29pt

At 58 by 58 (29pt @2x) the die reads clearly as a die with its five pips, and the cup reads as a dark, rounded, tapering shape beside it with its shadow.
The verdigris pattern does not survive that size: the cup is a dark bronze-green mass, and it is the die that says "dice game" first.
At 29 by 29 (1x, only on the contact sheet; no current device uses it) the cup is still a dark tapered silhouette against the teal, and the die is a pale square whose pips no longer read.
At 27x20pt @2x (54 by 40) the die is still a pale square with pips.

## Against the study

`docs/UI.html`'s Icon tab proposed two bone dice in the deep, a 5 and a wild 1, "no cup, because at 29pt a cup is a thimble".
This icon follows the brief it was given instead (one cup and one die, the app's own look); the study's worry is partly borne out at 29pt, where the cup is a shape and not a material.
If the owner prefers the study's two dice, the tool draws them by changing the bodies in `draw()`; the rest (camera, table, soft shadow, sizes) stands.

## Found on the way

`cn_geom.h`'s face slot enum and `cn_tex.h`'s texture kind enum both define `CN_TEX_DIE`, so no file can include both headers.
`cn_geom.h`'s die slot is now `CN_TEX_DIE_ATLAS` (package D renamed it so the stage could include both headers), and `cn_icon.c` includes `cn_geom.h` plainly; the icons came out byte for byte the same.
DECISIONS I7 still describes the placeholder icons; its icon half should point here.
