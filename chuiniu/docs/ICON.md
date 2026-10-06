# The icon

The placeholder icons of DECISIONS I7 (two dice on felt green, drawn by a throwaway script) are replaced by the study's locked icon, drawn by the game's own renderer.

## What it is

The study's Icon tab (`docs/UI.html`, section `icon`): two bone dice in the deep, a 5 and a wild 1, with the caustic over them.
No cup, because at 29pt a cup is a thimble.
The 1 is the icon's one drop of blood: its pip is the die atlas's own deep red (`cn_tex.c`, the 1 "in blood").
The 5 sits lower on the left turned about 12 degrees one way, the 1 higher on the right turned about 9 the other, as the study's `SHAPE.icon` places them.
The deep is the study's `.icon` gradient (`#134a4e`, `#0a2a2e` at 55%, `#04100f`) with the sea tile (`TEX.sea`, ported to C) screen-blended over it at 0.8, fading out by 80% of the height, one tile across 160/60 of the width as in the study.
The dice take the same caustic at a third of that strength, the light on them from the same water.
One light, the study's (up and to the left); the shadows fall down and to the right, softened.
No pirate anything, no published product's dice or box (`chuiniu/LEGAL.md`).

Every die surface is the game's: the mesh is `cn_geom_die_mesh`, the bone and the pips come from the same baked pack the app reads (`build/cn_tex.pack`, through `cn_tex_upload`), and the picture is `cn_scene.c`'s rasterizer and shadow map.

## How it is drawn

`chuiniu/c/tools/cn_icon.c` is a host-only tool (zlib for the PNGs), never linked into the app.
It follows uttt's pattern (`uttt/c/tools/uttt_icon.c`): one tool, every size drawn at its own pixel size, never shrunk from the 1024.

The renderer draws what an eye over the table sees on the table's plane (a shifted lens).
The icon wants an eye high and in front, looking at the dice.
Both are the same rays from the same eye, so the tool puts the renderer's eye where the camera is, renders the table-plane picture of the part of the table the frame sees, and for every output pixel follows 4 by 4 rays down to z = 0 and reads the picture there (bilinear).
That is the same homography the game's leaning head applies to its screen (`cn_cam.h`).

The frame is fitted to the dice's extent as the camera sees them, with 13% of the frame clear at the tighter edges, so the square and every 4:3 size hold the same subject.

What the tool adds to the renderer's output:

- The shadow is softened: the table's pixels (alpha below 255) are blurred among themselves only, three box passes of 2 points, weighted so no die's silhouette bleeds a halo.
- The deep and the caustic are painted under the renderer's output (which draws the table only as shadow with alpha), and the caustic again, weaker, over the dice.

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
The composition is the block of constants at the top of `cn_icon.c` (the dice's values, seeds, places and turns, the camera, the light, the caustic, the margin, the softness).

## Legibility

At 58 by 58 (29pt @2x) it reads: two pale dice on dark teal, the 5's five pips clear, the 1's red pip visible as a red dot.
At 29 by 29 (1x, only on the contact sheet; no current device draws it) it is two pale dice; the 5's pips read as a dotted face, and the 1's red pip is a single faint red pixel or two.
At 27x20pt @2x (54 by 40) the dice and both faces still read.

## The red

The 1's pip is the atlas's blood, a radial gradient from `#3A0906` through `#7F1F17` to `#B2503F` (`cn_tex.c`, `C1`), the study's own drilled-pip red.
It is close to, not exactly, DECISIONS I4's `#8B1A1A`; if the pip must be exactly that colour, the change belongs in `cn_tex.c` (the game's dice would change with it), not in the icon tool.

## For DECISIONS I7 (to fold in; this file does not edit DECISIONS.md)

> I7: the felt and wood are pickemup's baked JPEGs copied into `ChuiniuKit/Resources`.
> The icons are the study's locked icon (two bone dice in the deep, a 5 and a wild 1 with its pip in blood, the caustic over them, no cup), drawn by the game's own renderer from `chuiniu/c/tools/cn_icon.c` at every catalogue size; `make -C chuiniu/c icons` regenerates them, and `docs/ICON.md` says how.

## Found on the way

`cn_geom.h`'s face slot enum and `cn_tex.h`'s texture kind enum both define `CN_TEX_DIE`, so no file can include both headers.
`cn_icon.c` renames the slot for itself (`#define CN_TEX_DIE CN_TEX_SLOT_DIE` around the `cn_geom.h` include) until one of the two names changes in the headers.
