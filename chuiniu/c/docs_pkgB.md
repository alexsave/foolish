# Package B - the geometry, the camera and the layout in C

Everything `chuiniu/docs/UI.html` computed in JavaScript between the table's state and the pixels, except the textures (package C), the rasterizer (package A), paint, gestures and the display link, is now the kernel's.
Three new files, each with its test:

| File | What moved into it | Test |
|---|---|---|
| `src/cn_geom.c/.h` | `R3.cup`, `R3.die`, `setDieUp`, `dieValue`, `cupObj`, `dieObj`, the render loop's world transform and fan, the contact footprints, `ROLL.poseAt` and the place step; the kernel's own sin, cos, atan2, sqrt and the study's hash | `tests/cn_geom_test.c`, 1418 assertions |
| `src/cn_cam.c/.h` | `eyeOf`, `HC`, `EYE_DOWN`, `VIEW_B`, `ZOOM_B`, `tiltOf` with `toScreen` and `fromScreen`, `applyPeekB` as a CATransform3D and a homography, `peekTilt`, `peekAngleFor`, `PEEK_EXTRA`, the 420 ms ease | `tests/cn_cam_test.c`, 496 assertions |
| `src/cn_lay.c/.h` | `screen()`'s board geometry, `L`, `isShort`, `plateFrame`, `ringSeats`, `rowSeats`, the names, `pad`/`padX`/`padBelow`, the dice rings and their jitter, the out cup, `throwOf` and which seats throw | `tests/cn_lay_test.c`, 3142 assertions |

The Makefile carries the three sources in `SRC` and the three tests in `TESTS` (one line each), so they build in `make all`, in the iOS library (`IOS_SRC` is `SRC`) and in `make wasm` (freestanding wasm32, checked).
`make run` and `make asan` list their binaries one by one, so the merge must add these six lines beside the others (left out here to keep the Makefile edit to the two lines asked for):

```
	./build/cn_geom_test
	./build/cn_cam_test
	./build/cn_lay_test
	./build/asan_cn_geom_test
	./build/asan_cn_cam_test
	./build/asan_cn_lay_test
```

All three pass natively and under ASan + UBSan; `make run` is green with them built in.

## The API

A host fills a `CnLayIn` (seats, me, whose turn, each seat's dice, the out mask, my five faces, the drawer's width and height in points, the peek's eased fraction, the throw's seed) and calls `cn_lay_make`.
The `CnLay` it gets back holds the board rectangle (screen points), `top_m`, the short flag, the plate and the shelf (screen points, flat), my band, every cup's centre (board points, mine included), the shared cup radius `cup_r` and mine `my_r`, the dice sizes and rings, every name's anchor with how it sits on it, the ring's centre and radii, `pad`, `pad_x`, `pad_below`, the peek's full tip and its tip at the fraction, and the camera.
`cn_lay_objects` fills the bodies in the study's order (`CnObj`: pose, tilt, tint, the die's six face values, the texture seed, the footprint), `cn_lay_throws` fills one `CnThrow` per throwing seat with its seed and start, and `cn_geom_pose_at` / `cn_geom_place` play a bake.
`cn_geom_cup_mesh` and `cn_geom_die_mesh` build the meshes once per size, and `cn_geom_emit` writes a placed body straight into the renderer's arrays (6 floats a vertex, 16 a face, `i0 i1 i2 u0 v0 u1 v1 u2 v2 tex r g b km ka flags`), the format written as numbers so `cn_scene.h` is not included.

The camera exports what a Swift host needs: `cam.ca` is the CATransform3D about the turn's origin (m11 to m44, row-vector order, `m34` carrying `-1/D` through the turn), `cam.ca_screen` the same about the screen's (0, 0), and `cam.h` the 3x3 homography of the screen plane.
`cn_cam.h` says how to build it from `theta`, `D` and `zoom` with `CATransform3DMakeRotation` and `CATransform3DConcat`.
The CA sign convention for a rotation about x is checked here only algebraically (the matrix equals the CSS composition at every point of a 10 by 16 grid to a thousandth of a point); the first Swift host should map one point through its layer and compare it with `cn_cam_map`.

## The golden source

`tools/dump_study_layout.mjs` loads `docs/UI.html` in a real headless Chromium and dumps `tools/study_layout.txt`.
The Playwright npm module is not installed on this Mac (and `/opt/node-tools`, `/opt/pw-browsers` do not exist), but Playwright's cached `chrome-headless-shell` (build 1243, `~/Library/Caches/ms-playwright`) is, so the script drives it over the DevTools protocol with Node 26's own WebSocket; it falls back to the system Chrome or `$CHROME`.
The study's functions are closure-private, so the script splices three lines into a temporary copy of the page before loading it: `screen()` stashes its locals on `window.CN_last`, and the page exposes `screen` and `plateFrame`.
Nothing else changes; every number is the page's own arithmetic in Chromium's V8.
The cases are 390 by 340, 390 by 718, 375 by 541, 430 by 830 and 390 by 584, at 2 to 6 seats, on my turn (the picker up) and on theirs: 50 layouts, 10 distinct cameras, 840 dice, 200 cups.
`cn_lay_test.c` and `cn_cam_test.c` pin every one of them (seat centres, R, names, plate, pad, padX, theta, D, toScreen, fromScreen, the peek, every die's place and turn, every cup) to a hundredth of a point, and all 50 match.

## The collapsed height

The question: the study calls the collapsed drawer 340 tall, DECISIONS I17 calls a board under 280 short, and a memory note says the collapsed drawer equals the iMessage keyboard height, 584.
The 584 is not a height.
`foolish/ios/Tools/rig/README.md` (section 11) and `ui.py`'s `drawer_top` measure it as the drawer's TOP EDGE, in screen points from the top of a 6.9-inch phone (956 tall): "on a 6.9" phone the drawer's top edge is 567 or 584".
The same section measures the drawer itself as 388.7 or 372 points, and the extension's own surface as 340 or 323 (`follow geo=...->340` and `->323`), the 17-point difference being whether Messages' compose field holds the keyboard.
pickemup measured the iPhone 17e's compact drawer as a 281-point view (263 of board) (`pickemup/docs/IOS_DECISIONS.md`, `pk_lay.c`).
So the study's 340 is the 6.9-inch phone's collapsed surface, and smaller phones are shorter.

`cn_lay` takes the drawer's height as measured and applies the study's rule to the board left of it: `board_h = h - top_m - 12 - (shelf ? shelf_h + 10 : 0)` with `top_m = h > 400 ? 30 : 8`, short when `board_h < 280` (badge 72 + plate 56 + band 120 + four margins of 8).
Tested: 340 on my turn is short (220), 584 on my turn is tall (442), 281 is short (161), the edge (a 280 board is tall, 279.5 short).

One consequence the owner should see: at 340 the board is short only while the picker is up.
On their turn there is no picker, the board is 320, and it is tall, so on a 6.9-inch phone the collapsed table switches between the row and the ring every time the turn comes round.
At 323 (the keyboard up) it flips the same way (203 short, 303 tall); on the 17e's 281 it is short both ways (161, 261).
The study only ever drew the collapsed screen on my turn, so it never showed the flip; this is the study's rule, carried faithfully, and whether the short test should read the drawer rather than the board is a design call.

## Where the kernel differs from the study, on purpose

- **The die is a standard one.** The study set the up face and its opposite right and filled the four sides in order, so its 2 sat opposite its 4 and its 3 opposite its 5 (a side face is visible on every die at an angle). `cn_die_cells` makes every opposite pair sum to 7 and 1, 2, 3 run counterclockwise round their corner (a right-handed, Western die), any face up. A Chinese die is often left-handed; that is one line if the owner wants it.
- **A lying cup rests on both rims.** The study dipped the axis by `sin a = (R - rc) / h` and lifted the mouth's centre `R`, which leaves the mouth 0.38 points and the crown 0.28 points off the table at R = 43. `cn_geom_cup_obj` dips it by `tan a = (R - rc) / h` and lifts it `R cos a`, and the test measures both rims at 0. The turn moves under 0.01 and the footprint not at all against the study's.
- **A throw bakes the dice its seat holds.** The study baked five for every seat and drew only the seat's, so a seat on three dice had two dice nobody saw knocking the three about. `cn_lay_throws` sets `dice` to the seat's count.
- **Any seat may be me.** The study's `me` is 0. Here the others go round from the seat after mine, a seat's jitter and cup texture follow its own seat number, and a throw's seed and hashes follow its place round from me (so mine is always 0 and starts at once, as the study's was).

## Study quirks carried faithfully, recorded

- `tiltOf` measures the turn's line from an eye at a BOARD y to `origin[1] - 8`, a SCREEN y, so the board's top margin (30 tall, 8 short) is in theta. Kept, commented in `cn_cam.c`.
- `toScreen` / `fromScreen` apply the zoom after the perspective divide; the CSS the study paints (and `cam.ca`) applies it before. They part by 3.5 points 400 points above my cup on a 390 by 718 screen (`cn_cam_test` measures it). The layout's fit keeps the study's map so its numbers are the study's; a host paints with `cam.ca`. If the owner wants the fit exact to the paint, it is `cn_cam_to_screen` and `cn_cam_from_screen` alone, and the goldens move a few points at the far seats.
- `ringSeats` builds my cup's pillar with the far cups' perspective factor, not my own taller cup's. Kept.
- The mesh's inner wall narrows from `R - t` to `rc - t` over `h - t`, `cn_roll.c`'s physics over `h`, so the drawn wall is inside the physics' by up to `(R - rc) t / h`: 0.47 points at R = 59, at the floor. A die can sink that far into the drawn wall. Not changed here (`cn_roll.c` is not this package's); the test pins the bound.
- The kernel's sin and cos are `cn_roll.c`'s series, copied: 2.5e-13 off at pi/4, far below anything here.

## Every test, seen red

Each mutation applied alone with an editor, the binary deleted first, then the source restored by re-editing and `git status` checked clean.
Line numbers are the test files' as committed in 8e03e27c.

| Test | Mutation | Assertion that went red |
|---|---|---|
| geom: the kernel's maths | `atan_k` sums 4 terms, not 13 | `cn_geom_test.c:56` "atan2(-3, -20)" and on |
| geom: the cup | the fillet's rings drawn at .95 of its radius | `cn_geom_test.c:90` "closed", `:95` "the wall's volume", `:104` "the fillet starts where the wall ends" |
| geom: the die | the negative faces wound like the positive | `cn_geom_test.c:142` "closed", `:144` "volume -0.0", `:145` "every corner normal on its face's outer side" |
| geom: the die's values | the right-handed correction skipped | `cn_geom_test.c:176` "axis 0 sign -1 value 1: right-handed" |
| geom: a lying cup | the study's lift, `R` | `cn_geom_test.c:194` "a little under a radius up", `:206` "both rims on the table (0.376, 0.376)"; `cn_lay_test.c:1410` "lowered 0.000 onto the table" |
| geom: the emitted arrays | the hinge shifted the wrong way | `cn_geom_test.c:266` "the far edge of the mouth stays on the table" |
| geom: a baked throw | the short-way quaternion flip never taken | `cn_geom_test.c:298` "half of a quarter turn" |
| cam: the study's camera | the turn's line to the origin, not 8 above it | `cn_cam_test.c:58` "theta 0.145800, the study 0.148491", `:59`, `:64`; `cn_lay_test.c:1352`, `:1355` |
| cam: the host's transform | `m34 = +1/D` | `cn_cam_test.c:97` "ca about the origin", `:100` "ca_screen" |
| cam: the eye | the projection divides by `hc + z` | `cn_cam_test.c:126` "half way up, twice as far out", `:127` |
| cam: the peek | 2 points to spare, not 4 | `cn_cam_test.c:148` "at 49 degrees the rim's shadow"; `cn_lay_test.c:1365` "the peek" |
| lay: the study's layout | cups apart by 2, not 6 | `cn_lay_test.c:1351` "390x718 mine n 6: R 44.9066, the study 42.9066", `:1352`, `:1353` "padX 51, the study 49" |
| lay: the bodies | a die's ring jitter .12, not .1 | `cn_lay_test.c:1391` "seat's die at", `:1365` "the peek" |
| lay: short or tall | `<=` 280 | `cn_lay_test.c:1440` "a 280 board is tall" |
| lay: any seat may be me | the angles taken round from `me` again | `cn_lay_test.c:1466` "seat s sits where seat v does for seat 0" |
| lay: the throws | every seat bakes five, as the study did | `cn_lay_test.c:1495` "seat 1: bakes the 4 dice it holds" |
