# Package Z - the planks cover the whole view

## The bug

"The background doesn't cover the top of the screen in expanded mode sometimes" (s03_six_expanded.png: six seats, 440 by 956 screen, a bare dark band about 32 points tall between the drawer's rounded top and the first planks).

The planks' layer was the study's overdraw: 1.9 by 2.2 of the drawer, from -45% and -60%.
The camera's turn (`ca_screen`, perspective(D) rotateX(theta) scale(zoom) about my cup) brings a flat rect's far edge DOWN the screen, and a taller drawer turns the camera more (theta .11 at 281 points, .51 at 956 on their turn).
The overdraw's top edge turned lands at screen y -166 on a 281 drawer, +14 on a 718 drawer on their turn, +117 (mine) / +227 (theirs) on a 956 drawer.
StageView also reaches under the safe areas (Messages' grabber strip, about 32 points, above the drawer), so on a tall drawer the strip above the turned planks showed the flat wood under it, which was the planks under a 62% black shade: black.
The unit test reproduces it with the old rect: 430 by 830 with a 32-point top inset leaves the view's top corners 36 points outside the turned planks.

## The fix

`cn_cam_cover` (cn_cam.c) is the inverse of the camera: the four corners of a screen rect (grown by CN_CAM_COVER_SPARE, 8 points) mapped back through the inverse homography, boxed and rounded outward to whole points.
The preimage of a rect wholly in front of the horizon is a convex quad, and its box, also wholly in front (checked: w > 0 at every corner), turns into a convex quad that holds the rect.
A rect that reaches the horizon has no cover (0).
`cn_cam_planks` is the cover of the drawer and CN_CAM_REACH (80 points: the grabber strip, the home indicator, a landscape notch at 62) past each side, held to CN_CAM_COVER_FAR (8192) past the drawer.
The stage puts it in the HUD (`CnStageHud.planks`), and StageView sizes the planks' layer from it and nothing else.
The tiles keep the study's phase and the light keeps the study's stage (its gradients are placed in the overdraw and run on past it), so the pixels the drawer showed before are the same; only what was bare is now wood.
The flat wood under the turned layer is the same planks with no shade, so a rounding gap can never show black.

`cn_lay_cam` is the layout's camera without the layout's fits (`board_of` is now the one derivation both use), so the exhaustive test runs in two seconds instead of seven minutes.

## Sizes tested

C (cn_cam_test): every width 360..440 step 1, every height 281..1000 step 1, my turn, theirs, the roll's shelf and the reveal: 233,280 cameras, every one covered with the full 8 points of room past the 80-point reach.
A sample at every seat count (600 layouts) checks cn_lay_cam is cn_lay_make's camera bit for bit.
Swift (StageViewTests): the four study drawers plus 440x281, 375x340, 440x718, 440x956, each with Messages' insets (32 top, 34 bottom) and with the whole 80-point reach as insets: the view's four corners are at least 4 points inside the planks' layer's corners turned by Core Animation (CALayer.convert).

## The limit

The horizon comes into the reach on drawers taller than about 1150 points on their turn (1250 on mine), so no phone's drawer: there `cn_cam_planks` returns 0, StageView falls back to the study's overdraw, and the flat planks show above the horizon.
An exact cover for any inset would need the host's insets in CnStageIn (BridgeKernel, package X's file); the fixed reach was chosen instead.

## Memory

The planks' layer and its light have no bitmap of their own (contents nil); every tile is a CALayer whose contents is the one decoded cn_planks image (1032 by 1660, about 6.9 MB, already resident before this package).
The layer is at most 3746 by 7157 points (440 by 1000 on their turn): at most 90 tile layers, a few hundred bytes each.
On a 956 drawer it is 1656 by 2797 (mine) or 2686 by 4906 (theirs).
On short drawers the cover is smaller than the old overdraw (281: 610 by 435 instead of 836 by 618).

## Tests seen red

| # | Mutation | Assertion that went red |
|---|---|---|
| M1 | `cn_cam_cover` with no spare | cn_cam_test.c:226 "360x281 kind 0: the cover holds the drawer by 0.69", least room -0.00 |
| M2 | `cn_cam_planks` returns the study's overdraw (the bug) | cn_cam_test.c:226 from 360x534 up, least room -349.76 at 360x1000 |
| M3 | the HUD's planks made for a w by w drawer | cn_stage_test.c:142 "390x340 mine 1: the planks' layer ..." at every size |
| M4 | `cn_lay_cam`'s cup one point lower than the layout's | cn_cam_test.c:242 "cn_lay_cam is cn_lay_make's camera (0 of 600)" |
| S1 | `StageUIView.plankRect` ignores the HUD (the study's overdraw) | StageViewTests.swift:198 "430x830 inset 32: the view's corner (0.0, 0.0) is under the turned planks" (-35.7), and :189 at every tall drawer |
| S2 | `tile` lays from the phase without pulling it back | StageViewTests.swift:209 "430x830 inset 32: the tiles cover the layer" |
| S3 | no planks under the turned layer | StageViewTests.swift:218 "planks under the turned layer", :219, at all 16 cases |

Every mutant was put back by re-editing; `git status` showed the tree clean afterwards.
