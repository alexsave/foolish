# Package M report: memory under budget and a drawer that is never blank

## The measurement

- A private iPhone 17 Pro Max simulator `cnmem` (iOS 27.0, 440 by 956 points, dark), the Debug build installed by the rig, `dev.fill 6` (six seats, opened from the `+` menu), then the drawer expanded with `idb ui swipe 220 628 220 150`.
- `footprint -p` of the `ChuiniuMessages.appex` process, `vmmap --summary` where a category needed explaining.
- The cold open: the extension killed, the start bubble tapped, filmed (`simctl io recordVideo`) and timed by the Debug log.
- The machine was busy with other work for much of the session (load averages of 8 to 76), so times are medians of what was seen, and the same take was repeated before a number was written down.

## Memory, before and after (Debug, simulator, six seats)

| | before | after |
|---|---|---|
| compact, steady | 74 MB | 49 MB |
| compact, peak | 97 MB | 69 MB |
| expanded, steady | 104 MB | 50 MB (48 in a process opened from the bubble) |
| expanded, peak | 116 MB | 72 to 73 MB (75 after a peek) |

By category, expanded:

| | before | after |
|---|---|---|
| Malloc Large (the arena from `allocate`, the `Data` copies) | 53 MB | 0 (the arena is now its own mapping) |
| the arena (`Untagged`, an `mmap`) | - | 14 MB dirty (the textures), 23 MB reclaimable (the frame's pages, given back at rest) |
| CoreAnimation (its copy of each frame) | 9.3 MB | 96 KB |
| Image IO (the planks, 1032 by 1660) | 7.0 MB | 7.0 MB |
| Malloc Small | 14 MB | 10 MB |
| IOSurface | 0 | 0 resident in this process on the simulator (see below) |

THE SIMULATOR DOES NOT COUNT IOSURFACE PAGES. The picture surfaces are mapped (vmmap: 12 MB virtual over three regions) but none of their pages are resident in the extension's own footprint on the simulator; on a phone they count.
The picture on show at 2x on this drawer is 1136 by about 1850 pixels, 8.4 MB; while a frame draws there is a second.
So a phone would carry about 50 + 8 = 58 MB steady expanded and about 73 + 17 = 90 MB at the very worst peak (two full-size surfaces at once and every frame page touched), where before it carried 104 and 116 plus nothing (the old copies were all counted).
The same caveat applied before: the old CoreAnimation 9.3 MB was CA's copy, counted.

### What each change saved

1. ZERO-COPY FRAMES (a).
   The kernel draws straight into an IOSurface the layer shows as it is (`cn_api_stage_external`, `cn_api_stage_target`).
   Gone: the `Data(bytes:count:)` copy of every frame, the CGImage, Core Animation's own copy (`prepare_image`'s memmove), the previous frame's copies held while the next landed, and the picture inside the arena (4 bytes a pixel of every frame's arena share).
   About 31 MB expanded (104 MB less the 50 left, less the 23 MB the rest gives back).
   A picture's row is rounded up to 16 pixels (64 bytes, which any surface accepts); the extra columns are more of the same view, so the canvas is that much wider (`shot.canvas[2]`).
   `director.last` and the layer hold the same surface, so there is nothing to clear.
2. THE ARENA AT REST (b).
   Half a second after the last frame lands with nothing moving (`StageDirector.restDelay`), the stage gives the arena's frame pages back (`cn_api_stage_rest`, `madvise(MADV_FREE_REUSABLE)`) and drops every spare surface; the textures stay.
   About 23 MB at rest.
   Re-attach cost, measured: the first frame after a rest landed in 17.5 ms against 10 to 12 for the frames after it (the pages taken again with `MADV_FREE_REUSE` and a surface made); nothing is uploaded again (`cn_stage_test`: `scene_gen` unchanged).
   That is far under the 40 ms line, so nothing had to be cached: a full purge (the extension resigning, a memory warning, after a bubble) still uploads the textures again, 21 ms of `prepare` on the first frame.
3. NOT DONE: the 1.5x cap on tall drawers (c).
   Not needed: the expanded peak is 72 MB on the simulator without it.
4. NOT DONE: the planks (d).
   Image IO is the one decoded tile, 6.9 MB, shared by every tile layer (CoreAnimation holds no copy of it now: 96 KB in all).
   Decoding it smaller would soften the planks on a 3x screen; the tile is already 2x.
   It is now decoded once, off the main thread, while the drawer opens (`CnTextures.preload`, `kCGImageSourceShouldCacheImmediately`), instead of lazily inside a commit.
5. THE BUBBLE (e) never holds a second big frame.
   The bubble's frame is its own 608 by 390 surface (0.9 MB), drawn after the arena is purged and before it is purged again; the table's surface on show is the only other picture.

### At most one frame being drawn plus the one on show

A frame's surface belongs to the frames that hold it (`StageSurface`, a class): the stage draws only into a pooled surface that the pool alone holds (`isKnownUniquelyReferenced`) and that the render server no longer reads (`IOSurface.isInUse`).
The director holds one frame (the one on show) and asks for the next only after it landed, so in steady drawing there are two surfaces, and a third only while the render server holds an old one a moment longer.

## The white drawer (the cold open)

Tap to first frame, six seats, the bubble tapped with the extension killed (the Debug launch log, `launch <what> <ms since the process started>`):

| | ms |
|---|---|
| tap to the process starting | about 500 |
| process start to `viewDidLoad` | 1960 to 2700 |
| of it, dyld on the simulator (`dyld_sim` loading the Debug image's dependents) | about 420 (sampled) |
| of it, UIKit loading the accessibility bundle (`_accessibilityBundlePrincipalClass`) | about 1200 (sampled) |
| `viewDidLoad` itself | 13 to 19 |
| to `willBecomeActive` (Messages) | 100 to 250 |
| to the table screen's begin (SwiftUI's first pass, the drawer rising: the hosting view is shown only when the drawer is up) | 130 to 155 |
| the begin (layout, six throws baked) | 8 |
| the first frame: `prepare` (the textures and their copies) 21, bands 12, queue and present | 51 to 66 |

So nearly all of the blank time is the process starting.
On the simulator it is dominated by the accessibility bundle, which UIKit loads because the rig's `idb` describes the screen (accessibility automation is on); a phone without VoiceOver does not load it, and a Release build has no `.debug.dylib`.
The extension's own share, `viewDidLoad` to the first frame, is about 300 ms, and the drawer is never Messages' colour during it.

What changed:

- `MessagesViewController.viewDidLoad` paints the view the planks' dark (`Ink.hold`) before anything else, so from the extension's first commit the drawer is the table's colour, not white.
- `ChuiniuRoot` shows the bare planks while the model is `.empty` (no conversation read yet), where it showed a lobby with the title for a frame (the "flash" before the table).
- `KernelSeam.warm()` from `viewDidLoad`: the texture pack read and the stage opened on the stage's queue, and the planks decoded on a utility queue, while the drawer opens.
- The throw still starts at its first frame when it is pending: nothing here touches the clock or `cn_api_roll_pending`.

What was not cut: Messages shows its own placeholder until the extension's view appears (`viewDidAppear` came 35 ms after the first frame in every take), so the frames before that are Messages', and the extension cannot paint them.
Filmed: grey placeholder for 1.0 to 1.3 s, then the table (no white, no lobby, no transcript flash); `chuiniu/docs/shots` was not updated, the takes are in the session's scratch.

## Frame times (the Debug frame log)

| | land (ask to picture) | present (the commit) |
|---|---|---|
| compact throw, before | 4.6 to 6.5 ms | 0.33 to 0.61 ms |
| compact throw, after | 4.8 to 6.4 ms | 0.02 to 0.54 ms |
| expanded peek (2x), after | 10.0 to 11.9 ms | 0.41 to 0.55 ms (V1 measured 1.4 median before) |
| first frame after a rest | 17.5 ms | 1.2 ms |

## Release

Not measured: a Release build has no dev harness and no `dev.fill`, and installing it restarts Messages, which drops the simulator's conversations, so a six-seat table could not be reached cheaply.
Release carries the same arena, surfaces and textures (none of it is Debug-only); the Debug-only parts are `__DATA` (13 MB here, the Debug image's), the `.debug.dylib` and the unoptimised Swift, so expect it lighter by roughly that.

## Tests, and each seen red

| test | mutation | red at |
|---|---|---|
| `cn_stage_test` external frames | the canvas not widened (M1) | `:746` both frames |
| | the rest span 4 KB into the textures (M2) | `:773` (the exact-span check, added after the first form of the test stayed green: the scribbled 4 KB was a copy no frame read) |
| | the rest span the whole arena (M2b) | `:771`, `:775` |
| | a buffer half the size taken (M4) | `:751`, `:755` |
| | rows not rounded to 16 (M3) | `:743` |
| `BridgeKernelTests.testFramesAreTheKernelsSurfacesAndTheStageRests` | the pool draws into a surface a frame holds (S1) | `:278`, `:279`, `:288` |
| | the rest records no pages (S2) | `:287` |
| `StageViewTests.testTheDirectorRestsTheStageOnceTheTableIsStill` | no ticket: an overtaken wait rests anyway (S3) | `:318`, `:321`, `:322`, `:324` |
| | the director never rests (S4) | `:321`, `:322`, `:324` |

A double band-aid removed on the way: each asked frame also bumped the rest ticket, which the landing of that same frame bumps again (and a frame in flight, a dirty director or anything moving is the wait's own guard).
Removing the ask's bump was the first mutation tried and the test stayed green, which is what showed it was redundant; it is gone, and S3 (no ticket at all) goes red.
