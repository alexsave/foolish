# cdom_poc: foolish's game table, drawn from C

A proof of concept: the web game table (src/components/GameBoard.tsx and the GameDisplay components) written as one plain C file that drives the DOM through cdom, over the app's own CSS.
It is local only and not wired into the web app.

## What was built

`src/table_app.c` draws a two-seat table, me against the handwritten bot, with the React table's look: the hand, the battles, the stock and the trump, the discard pile, both seats with their role marks and mini hands, the pill column, and an end screen.
The look is the React components' inline styles, copied value for value, over `src/styles/index.css` linked as the app links it, with `data-theme="default"`.
The board is the kernel's, called directly as C: a `Game`, read for the viewer with `client_adopt_board` into a `TableView` and `client_view_rules`, the pills and the meaning of a click from `client_play`, the move written by `client_move_wire` and applied by `awire_apply`, and the bot's answer from `bot_drive`.
The first board is the one `e2e/ui_dom_snapshots.test.ts` `twoSeatBout()` composes for the golden `bout_2p_attacker.html` (trump Kc, my 6s 7h Qd Ad against 9h Th Jh 6c, 6h/8h and 6d on the table).
"Play again" deals a fresh seeded game.

Playing: click a card to select it (red border) and the kernel's pills appear; click a pill, the table (attack or transfer) or a battle (cover) to play.
The bot answers 650 ms later, one visible move per step, and the game runs to its end screen.
Every region keeps what it last drew and emits only the difference: the hand, the battles, the pills, the stock and discard layers and each mini hand are cdom keyed lists; the counts, the role marks and the hand positions are patched in place.
Nothing is rebuilt on a move: the test tags the nodes before a move and finds the same nodes after it.

The wool, wood and fern textures are made in C after the first paint, only as much of each as the page shows, and kept across loads in cdom's store as the app keeps its own (Textures, below).
Not ported: the flights and other animations, drag and drop, keyboard play and the cover arrows, the tutorial hints, chat, translations, and live play.
Live play against c_base and sign-in are next steps, not part of this PoC; the network imports below are the piece they would stand on.

## Files

- `src/table_app.c`: the table (1,325 lines; 251 of them the texture pipeline, from making to the store).
- `src/ui.h`: element shorthands over cdom's `CD_LIT`, and the style builder whose result crosses as a one-off string (36 lines).
- `src/kernel_glue.c`: the one bot brain (20 lines); the allocator is cdom's.
- `src/textures.c`, `src/textures.h`: the wool, the wood and the fern, ports of their TS generators, the wool kept in a crop and the wood in rectangles (385 lines).
- `src/trig.c`: sin, cos and tan for the wool and the fern (musl's polynomials, a reduction for arguments below 2^28) and JS's `%` for the wood (141 lines).
- `src/crtrig.c`: cos rounded correctly, as Chrome's is, for the wood (139 lines).
- `src/index.html`: the page; `build.sh` inlines the minified shim into it.
- `vendor/`: cdom from `/Users/alex/Dev/cdom-fixes`, branch port-fixes, commit 8b9266f, unchanged (`src/cdom.c`, `src/cdom.h`, `src/cdom_net.c`, `src/cdom_net.h`, `src/cdom_store.c`, `src/cdom_store.h`, `src/mini.c`, `shim/cdom.js`).
  The table links `cdom_store.c` (the textures across loads) but not `mini.c` (the kernel's libc brings memcmp and strlen) nor `cdom_net.c` (it has no network yet); the network test links the network.
- `tests/net_test.c`, `tests/net_test.mjs`: cdom's network imports against a local echo server.
- `tests/tex_test.c`: the texture generators alone, for `tools/texparity.mjs`.
- `tools/`: `cdp.mjs` (headless Chrome over CDP and a static server), `test.mjs`, `shots.mjs`, `timing.mjs`, `textime.mjs` (the textures' timing against the app's), `strings.mjs`, `texparity.mjs`, `domcount.mjs`, `domdiff.mjs`, `play.mjs` (plays my seat to the end by clicks), `mutate.py`.
- `shots/`: the screenshots.

## Running it

```
./build.sh                      # out/table.wasm, out/index.html, out/net_test.wasm, out/tex_test.wasm
node tools/test.mjs             # the table test
node tests/net_test.mjs         # the network imports
node tools/strings.mjs          # the string table over three games
node tools/texparity.mjs        # the C textures against the TS generators (writes out/tex_ts.js)
node tools/shots.mjs            # shots/*.png (W=390 H=844 for a phone); run texparity first
node tools/timing.mjs 21 1      # medians over 21 fresh loads, CPU x1 (or x4); PAGE=..., BLANK=1, TEX=cold|warm
node tools/textime.mjs 9 1      # the textures, port and app, cold and warm, interleaved; CPU x1 (or x4); BEFORE=dir
CPU=4 node tools/texparity.mjs  # the generators' parity, and their times throttled
python3 tools/mutate.py         # every mutant, rebuilt and tested, the source restored after
```

Run `npm install` once in `tools/` for terser; without it the shim is inlined unminified.
`tools/texparity.mjs` and `tools/textime.mjs` bundle the app's TS with esbuild from the main checkout's node_modules (`/Users/alex/Dev/foolish/foolish/node_modules`).
`build.sh` needs homebrew LLVM (`WASM_CC` overrides it), and uses `wasm-opt` and `brotli` when present.
The tools serve `foolish/` on port 8720 and drive Chrome on 8729.
`?notex` on the page leaves the textures out (the golden comparisons use it).
To play by hand, serve `foolish/` (for example `node -e "import('./c/cdom_poc/tools/cdp.mjs').then((m) => m.serve())"` from `foolish/`) and open http://127.0.0.1:8720/c/cdom_poc/out/index.html.

## Against the React golden

The first paint is pixel-identical to the React golden under the same CSS: `tools/shots.mjs` counts 0 of 786,432 pixels differing at 1024x768 between `shots/react_golden.png` (the recorded React DOM in a page with the app's CSS) and `shots/cdom_midgame.png` (`?notex`: the golden has no textures).
The measure is checked against itself: with 6s selected the same count is 3,901.
The golden's action area is given back the `bottom: max(10px, env(safe-area-inset-bottom))` that ActionButtons.tsx writes and jsdom could not parse; nothing else in it is touched.
At 390x844 the counts differ (13,952 pixels) because the golden's hand positions were recorded at jsdom's 1024 width.

`tools/domdiff.mjs` diffs the two DOMs tag by tag.
The only differences: the `data-turn` attribute the test reads, the keyboard-play state div and the cover arrows' SVG layer (not ported), the action area's bottom (above), the seat ring's `left: 49.99999999999999%`, which C prints as `50%`, and the texture hooks: `var(--tex-wood, <the golden's stripes>)`, `var(--tex-fern-bg, rgb(180, 14, 9))`, `background-image: var(--tex-fern)` on the card backs and `background-image: var(--tex-wool)` on `.bg-wool`, which fall back to the golden's look until a texture lands.
The test holds that: the first-paint check compares the whole DOM to the golden with exactly those differences taken out.
After the first paint the DOM is the browser's: a patched card is one `style.setProperty`, so Chrome writes the style attribute back in its own form (`transition: 0.1s, visibility` for `all 0.1s ease, visibility 0s`), as it does for React's own updates.

With textures, `shots/react_golden_tex.png` is the golden with the app's TS textures, at the app's sizes (the fern 1000x1400, the wood 1920x1080 whole, the wool 3840x2160 whole), made from the C's seeds and put where WoolBackgroundLayer, getTextureStyle, CardBack and PlayerRing put them.
Against `shots/cdom_tex_midgame.png` 6,643 of 786,432 pixels differ, every one of them on a card back: 4,025 on the deck, 1,520 and 1,098 on the two mini hands (the fern, Textures); the wool and the wood are the same pixels.
At 390x844 the count is 21,032: the 13,952 above (the hand, laid out for jsdom's width in the golden), the card backs, nothing on the wool.
`shots/fern_backs.png` and `shots/fern_backs_390x844.png` put a card back of each side by side, 4x (React's left).

## Textures

The three textures the React table shows are made in C, after the first paint, and only as much of each as the page can show.
- The wool behind the table: `paintWool` (src/utils/woolPixels.ts), the app's 3840x2160 weave, of which only the middle 1952x1104 is woven.
  `.bg-wool` covers the viewport with the picture and is scaled by 2 about its middle (layouts.css), so whatever the window's shape, only the middle 1920x1080 of the picture is ever on screen.
  The C runs the weave's every step in the app's order but skips, before its trig, each step whose pixels fall outside the crop, and a whole fibre whose position is outside it; a step's few constant terms (`zValue()`, the plaid's `tan`) are worked out once per fibre.
  The crop starts on JPEG's 16x16 block grid as the whole picture does (column 944, row 528), and is put at its place in a 3840x2160 image (`cd_encode_image_in`, the rest black and never seen): the browser then sizes, snaps and resamples it as it does the app's picture, encoded as JPEG at 0.92 as WoolBackground does.
  (Given its own size and position, the crop landed 0.67 px off after Chrome snapped the background to pixels, and every pixel differed.)
- The wood on the buttons: `generateWoodPixelData` (src/components/WoodTexture.tsx), its 1920x1080 tile, worked out only inside the rectangles its surfaces show.
  A button shows the tile from where getTextureStyle's seed puts it; the C knows every surface's seed and size (the back button, the five pills, Play again and the results panel, measured and given room to spare) and works out each (column, row) of the TS's loops that reaches one of them, in the TS's order for each pixel, and nothing else.
  The wood is two pictures of the one tile: the table's surfaces, and the end screen's (`--tex-wood-end`), made last because it is shown last.
  WoodTexture draws its float colours through WebGL with a one-point-per-pixel blit and no maths of its own, so C keeps the same float32 colours and turns each into a byte as the blit does; no WebGL is needed.
  `tools/test.mjs` checks, after every click of games played out until every pill has shown, that every wood surface on the page lies inside the rectangles of its picture.
- The fern on the card backs: `generateFernPoints` (src/utils/fernFractal.tsx), at 4 times the deck's card back (50x70) in device pixels, 200x280 on a 1x screen and 400x560 on a 2x one, with the app's 5,000,000 points scaled by the area (200,000 and 800,000), so it is as dense.
  The app draws a 1000x1400 picture that the browser shrinks 20 times onto a 50x70 back.
  The C draws the points into 4 samples per pixel at Metal's standard positions and resolves them, as the app's multisampled WebGL canvas does (ANGLE on Metal here).

Trig: the wool's `sin(i - 1)` for fibre steps up to 8,294,400 sent musl's sin through its slow reduction (above 2^20 pi/2), and the wood's and the wool's double-double sin and cos were 2.5 and 5.6 times slower than the TS.
`src/trig.c` keeps musl's polynomials with a reduction good to 2^28 (pi/2 in three 25-bit parts and a fourth, Cody and Waite), and the wool and the fern use it: within 1 ulp of Chrome's for 2.9%, 2.8% and 3.4% of arguments (sin, cos, tan; 3,000,000 arguments), and neither picture changes.
The wood keeps `src/crtrig.c`, cos rounded correctly (0 of 1,500,000 arguments differ from Chrome's): its 25 iterations of `b = cos(...) * b - 2.8` per pixel grow a last-bit difference into another picture (with musl's cos, 71% of its pixels differ, by up to 207).
Working the wood out only where it shows made that cost small.
`src/jsmath.c` (1,206 lines, musl's 1,200-bit 2/pi table) is gone.

How they land: `cd_encode_image` (cdom) encodes the pixels in C memory (RGB for the wool and the wood, so the room is a quarter less) and hands the file's bytes back to C; `cd_set_image_bytes` puts them on the mount as `--tex-fern`, `--tex-wood`, `--tex-wool` and `--tex-wood-end`, which the styles read through `var()`, falling back to the golden's look; with the fern, `--tex-fern-bg: #000000` gives the backs CardBack's black.
Across loads: the bytes go into cdom's store (`cd_store_put`, IndexedDB) under a key with the generator's version and what the picture depends on (the fern's size, a hash of the wood's rectangles); a later load asks the store first (`cd_store_get`, 1,500 ms deadline) and sets what it finds, making nothing.
The app does the same with TextureCacheDB (src/utils/textureCache.ts: keys fern_v1, wood_v2, wool_v1, reads raced against 1.5 s).

When: the store is asked two frames after the first paint; making waits 50 ms more (started on a timer right after the first frame, making held `cdom.start` to 165 ms at x4 against 60).
Each texture is made in slices of about 8 ms on animation frames, in the order fern, wool, the table's wood, the end screen's wood, and the next one starts when the last one's file is back: Chrome encodes a PNG in idle time, and every-frame slices left none (at x4 the fern, made in 50 ms, landed 700 ms late).
One block of 6,465,024 bytes (an 8 MiB size class) holds each in turn and goes back to cdom's free lists at the end; no block is taken when all four come from the store.

How the React app schedules its own (read in src/, unchanged): on mount each hook checks TextureCacheDB first; the wool weaves on a worker (woolWorker.ts, OffscreenCanvas, JPEG), 3840x2160 fixed ("the weave's apparent scale is tied to the pixel grid"), falling back to the page with idle-callback pauses; the wood generates on the page's thread in 24-column chunks with setTimeout yields, then a WebGL blit, 1920x1080 fixed; the fern computes 5,000,000 points on the page's thread with setTimeout yields every 100,000, then a WebGL point draw, 1000x1400 fixed.
None of them looks at the display's size or device pixel ratio.

Parity (`tools/texparity.mjs`, seed 20261028, the TS run in headless Chrome with Math.random replaced by the same mulberry32 the C uses):
| texture | compared with | pixels differing | max channel difference |
| --- | --- | ---: | ---: |
| wool 3840x2160 | paintWool's buffer | 0 of 8,294,400 | 0 |
| wool, the page's crop 1952x1104 | the middle of paintWool's buffer | 0 of 2,155,008 | 0 |
| wood 1920x1080 | generateWoodPixelData's float32 colours | 0 of 6,220,800 values | 0 |
| wood 1920x1080 | the app's WebGL picture (the PNG it makes) | 0 of 2,073,600 | 0 |
| wood in six rectangles (both tile edges, row 0) | the app's WebGL picture | 0 of 78,752 (the rest the base colour) | 0 |
| fern 1000x1400 | generateFernPoints' points, one pixel each | 0 of 1,400,000 | 0 |
| fern 1000x1400 | the app's WebGL picture, multisampled | 31,678 of 1,400,000 (2.3%) | 255 |
| fern at the page's 200x280 (200,000 points) | the app's picture shrunk to 200x280 | 33,199 of 56,000 | 255 (mean 282 summed over the channels of a differing pixel) |
| fern at 400x560 (800,000 points) | the app's picture shrunk to 400x560 | 155,395 of 224,000 | 255 (mean 201) |
| for scale: the C fern at 1000x1400, shrunk | the same | 1,244 of 56,000; 9,065 of 224,000 | 255 |
The page's fern is another draw of the same fern, not the same pixels: at 200x280 a point is 25 of the app's, so the picture's fine grain differs everywhere while the shape, the colours and the density are the same.
What a player sees is the picture shrunk onto a 50x70 (deck) or 25x35 (mini hand) back, and there the difference is small: on the screen, the deck's top back differs from React's in 1,730 of 3,500 pixels by at most 73 per channel and a mini back in 690 of 875 by at most 53, where the C's full 1000x1400 fern (the slow one) differed in 1,612 by at most 49 and 662 by at most 41 (1024x768; at 390x844 1,737 and 689 at most 73 and 50, against 1,644 and 674 at most 59 and 41).
`shots/fern_backs*.png` show them side by side.
The 2.3% at full size: the GPU's rules at the edges of a point's square (which samples a point exactly on a boundary covers, and the precision of positions after the vertex shader) are not documented; four variants did not do better than the plain square, and on another GPU the app's own fern differs from this one in the same way.

## Numbers

Measured on this Mac (headless Chrome 156), CPU x1 and x4 throttled, medians of 21 fresh loads with the cache off; script is the click handler, C and its DOM calls, and the second figure adds style and layout forced at once.
"Before" is the build at df7fda82d (`out_before/`), "after" this one with `?notex`, timed in the same session, one after the other (loads 2.6-3.1).

| | before x1 | after x1 | before x4 | after x4 |
| --- | ---: | ---: | ---: | ---: |
| first contentful paint | 33.6 ms | 30.1 ms | 60.7 ms | 58.8 ms |
| cdom.start resolved | 18.5 ms | 18.9 ms | 32.1 ms | 31.8 ms |
| select a card: script / with style and layout | 0.34 / 0.84 | 0.17 / 0.65 | 0.37 / 1.17 | 0.10 / 1.04 |
| play the Attack pill | 0.69 / 1.75 | 0.47 / 1.39 | 0.99 / 2.22 | 0.74 / 1.79 |
| the bot's step | 0.92 / 2.34 | 0.65 / 1.95 | 1.02 / 2.80 | 0.98 / 2.81 |

The clicks are faster after because a patched card writes the properties that changed (`cd_set_style_num` on left, bottom, width, height, `border-color`) rather than a whole new 600-byte style string, which also had to be hashed and interned.
DOM calls per action (`tools/domcount.mjs`, now counting `style.setProperty` too): selecting 6s is 16 before and after (the border is one setProperty where it was one setAttribute); playing it 26 before, 25 after (6 setProperty and 8 setAttribute where it was 14 setAttribute); the bot's step varies with the bot's draw from run to run (76 to 80 calls in both builds), with setAttribute 36 -> 26 and setProperty 0 -> 10.
First contentful paint waits on the stylesheet's chain of `@import`s; the table itself is in the DOM by the time `cdom.start` resolves.

The textures (`tools/textime.mjs`, 9 rounds at each throttle, each load from a blank page in a fresh renderer, cache off; median and 25th-75th percentile, ms after navigation).
Each round loads, in turn: the port with `?notex`, then the port and the app cold (both stores emptied), then both warm (each reading what its cold load stored); which side goes first alternates.
"TS" is the app's own code as the app starts it on mount, bundled from src/: generateFernPattern, WoodTexture's generateWoodTexture and WoolBackground's generateWoolTexture on its worker, each checking TextureCacheDB first (`out/ts_tex.html`); it starts at navigation, while the real app first loads and mounts its bundle, so its absolute times are earlier than the app's would be.
"Before" is this port at d526bba10, before this pass (3 rounds, cold; it has no store).
Measured under load: load average 3.8-4.5 on 8 cores around the runs (another agent's bot matchups, `new_opus`/`cnitro_tests` at 87% of a core, and the c_base server), below the core count; every arm ran interleaved under the same load.

| CPU x1 | fern | the table's wood | wool | the end screen's wood | all decoded |
| --- | ---: | ---: | ---: | ---: | ---: |
| before, cold | | | | | 9,485 (9,485-9,513) |
| port, cold | 142 (133-144) | 614 (608-625) | 417 (406-418) | 879 (875-881) | 904 (899-905) |
| TS, cold | 1,921 (1,770-1,948) | 1,640 (1,488-1,678) | 1,155 (1,093-1,160) | | 2,010 (1,867-2,039) |
| port, warm | 63 (60-72) | 71 (65-76) | 69 (63-74) | 74 (66-76) | 123 (121-126) |
| TS, warm | 17 (16-25) | 17 (17-26) | 18 (17-27) | | 111 (108-121) |

| CPU x4 | fern | the table's wood | wool | the end screen's wood | all decoded |
| --- | ---: | ---: | ---: | ---: | ---: |
| before, cold | | | | | 36,278 (36,278-36,477) |
| port, cold | 335 (324-336) | 1,880 (1,855-1,886) | 1,172 (1,169-1,184) | 2,680 (2,650-2,690) | 2,688 (2,662-2,721) |
| TS, cold | 6,970 (6,900-8,632) | 6,762 (6,559-8,264) | 2,001 (1,782-2,216) | | 7,069 (6,997-8,727) |
| port, warm | 218 (217-227) | 238 (231-242) | 229 (226-238) | 245 (234-250) | 280 (273-290) |
| TS, warm | 39 (37-41) | 39 (38-43) | 40 (39-43) | | 141 (140-147) |

All decoded, port over TS: cold 0.45 at x1 and 0.38 at x4; warm 1.11 and 1.99, because the port asks its store two frames after its first paint (at 70 and 218 ms), and the TS page at navigation.
A warm port load sets each texture 1 to 10 ms after asking (the store's answer); a cold one makes them in 16, 85, 129 and 120 ms of the page's thread at x1 (fern, table wood, wool, end-screen wood; 51, 353, 548 and 494 at x4), spread over frames.
First contentful paint does not move: 73 (70-76) with `?notex`, 73 (68-74) cold, 70 (68-71) warm at x1; 215 (206-241), 212 (209-218) and 218 (216-228) at x4.
Wasm memory: 3.3 MiB after a warm load, which takes no block; 11.8 MiB after a cold one, the 8.5 more being the 8 MiB block (given back to cdom's free lists, but wasm memory does not shrink) and the wood's floats; before this pass 33.2 MiB.
The generators alone (`tools/texparity.mjs`, x1): the app's paintWool 686 ms and its JPEG 110, generateWoodPixelData 1,713 (1,834 with its WebGL draw and PNG), generateFernPoints 329 (721 with its WebGL draw and PNG); the C's full 4K wool 424 and its crop 105, the C's wood in the six test rectangles 174 (its full tile, still exact, 4,038), the fern at 1000x1400 228 and at 200x280 9.
At x4 (`CPU=4`, measured while the load average climbed to 12 on 8 cores, so upper bounds): paintWool 2,819 and its JPEG 404, generateWoodPixelData 7,073 (6,994 with WebGL and PNG), generateFernPoints 1,302 (2,135 with WebGL and PNG); the C's crop 438, its six wood rectangles 702, the fern at 200x280 37 and 400x560 148.
The app's warm read in the same harness (textureCache.ts getCachedTexture, each opening TextureCacheDB): fern 5.5 ms, wood 1.0, wool 0.9 at x4.
No React figure is given: the real app's numbers need its server and bundle, which this PoC does not run.

The string table (`tools/strings.mjs`, three games played out by clicks):
| | before | after |
| --- | ---: | ---: |
| after the first paint | 174 strings, 16 KB | 128 strings, 3.6 KB |
| after game 1 | 358, 65 KB | 178, 4.5 KB |
| after game 2 | 388, 74 KB | 178, 4.5 KB |
| after game 3 | 523, 125 KB | 178, 4.5 KB |
Every computed string (a style, a data-card, an index) now crosses as a one-off (`cd_tmp`) and is never interned; what is left is literals and labels, all seen by the end of the first game.
Live node ids at each end screen: 58, 64, 60 (the board on screen differs; nothing accumulates).

Sizes:
- `table.wasm` 71,391 bytes raw, 29,539 brotli (q11), 32,872 gzip (-9); before the texture pass 71,725, 29,589 and 32,956, before the first port-fixes pass 54,992, 22,122 and 24,870.
  musl's 2/pi table went with jsmath.c; the crop, the rectangles, the store and the keys came in.
  For scale, the React app's kernel alone, web.wasm, is 67,311 raw and 31,219 gzip (c/Makefile), before any JavaScript, and its textures are in its JavaScript.
- The shim: cdom's own, unchanged, written to be read and inlined minified: 6,678 bytes minified, 2,632 brotli (6,553 and 2,615 in its golfed form before); cdom's PERF_LOG.md, SHIM_LINES, says what each import is for.
  Before the first port-fixes pass the port carried 172 lines of its own patched shim.
- The C the table is written in: 1,381 lines (`table_app.c` 1,325, of which the texture pipeline 251, `ui.h` 36, `kernel_glue.c` 20), plus the generators (385), trig.c (141) and crtrig.c (139).

## Against the TSX it replaces

The TSX for the same pieces is 1,795 lines: GameBoard 126, CardFace 204, CardBack 56, DeckAndFlipped 80, DiscardPile 44, TableBattles 400, PlayerRing 319, ActionButtons 289, handShape 147, RoleMark 130.
That is not a like-for-like count: the TSX also carries the flights, the row glide, the turning role coin, drag and drop, the tutorial's hints and chat bubbles, and it leans on contexts, `sdk/ts/table/client_table.ts` and generated readers for every kernel struct it touches.
The C needs none of that bridge: a `TableView` is a struct, and `client_play`, `hand_layout` and `bot_drive` are plain calls with no marshalling and no generated layout code.
For what both do, the C is about as long as the TSX, most of it the same inline styles as string literals.

## The tests

`tools/test.mjs` (headless Chrome, real mouse events):
- the first paint is the golden's DOM, but for the listed differences;
- the seeded deal reads back: the hand, the battles, a badge of 21 over 20 stock layers, the marks, no pills;
- a resize re-lays the hand at the new width through `hand_layout` (the resize arrives through cdom's event path, `cd_listen_window`);
- a click selects a card and the kernel's pills follow (7h offers nothing, 6s offers Attack), and a second click deselects;
- Attack plays 6s, the bot answers with a legal move, the surviving hand cards, battles and seats are the same DOM nodes, and Anna's count stays her mini hand's last child;
- the textures land after the first paint: with the store emptied, none is set when the table first shows, then all four and the black back are, a card back, a button and the wool show a `url(blob:...)`, all were made on this load, and the wool's image is the app's 3840x2160;
- the next load in the same profile sets all four from the store and makes none (no generator ran);
- a game played out by clicks (`tools/play.mjs`, textures on) reaches the end screen, and Play again deals six new cards;
- after every click of that game and of more games, until every pill has shown, every wood surface on the page (the back button, each pill, the results panel, Play again) lies inside the rectangles its picture was worked out in.

`tests/net_test.mjs`: a wasm module sends 8 binary bytes over a WebSocket and gets them echoed, then POSTs them and gets them back reversed (cdom's network, with no cd_flush anywhere).
`tools/strings.mjs`: from game 2 to game 3 the string table may grow only by what a game can show for the first time late (an end title, a pill, a medal: at most 10 strings and 200 bytes); it had been "may not grow", which failed whenever game 3 was the first to show such a literal (seen once in this pass, 180 to 181).
`tools/texparity.mjs`: the PRNG exact; the wood's cos exactly Chrome's and trig.c's sin, cos and tan within 1 ulp; the wool (whole and the page's crop), the wood (floats, the WebGL picture, and six rectangles with the base colour around them) and the fern's points exact; the multisampled fern within 3% of the WebGL picture; and, measured but not judged, the page-size fern against the app's picture shrunk.

Mutation check (`tools/mutate.py`), each of 24 mutants caught by the check written for it:
- a card face's radius 5px to 4px: the golden check names the first differing tag;
- the bot never chooses a move: the bot-answer check times out on `data-turn`;
- the hand rebuilt on every render: "the three cards left in my hand are the nodes that were there";
- the hand patch writes black for a selected card: the selection check finds a black border;
- cdom's shim clone not attached: the golden check, then the seeded deal (empty battles);
- no end screen: the play-out check finds no `[data-end]`;
- no resize listener: the resize check times out;
- the count badge not the list's end anchor: the bot-answer check finds the count away from the end;
- no black under the fern: the texture check;
- styles interned again: `tools/strings.mjs` (14 strings and 6 KB from game 2 to game 3);
- the wood's edge alpha off by 1e-7, the wool's clamp truncating, the wool skipping steps 9 pixels inside its crop, the wood dropping a stripe's stores into the row above, trig.c's reduction rounding k (pi/2) in one product, the fern's sample positions flipped, a double-double product in crtrig dropping a term: `tools/texparity.mjs` ("wood", "wool", "wool", "wood", "trig ulp, wool", "fern multisampled", "cr_cos, wood");
- the pills' wood rectangles 40 wide: "Attack 560 pixels outside"; the results panel's 60 tall: "bg-wood 5882 pixels outside";
- the store ignored: "every texture from the store, no generator run" (the warm load made all four);
- the wool not placed in the 3840x2160 image: "the wool image is the app's size" (1952x1104);
- the WebSocket bytes flipped in the shim: "ws failed"; the HTTP body not sent: "http wrong".

## WHAT THE APP NEEDED FROM CDOM

Each item is a gap hit, a helper wished for, or a note on where plain C felt awkward or fine.
Items 1 to 16 were folded into cdom on branch port-fixes (`/Users/alex/Dev/cdom-fixes`, commit 6380ee7), each with a test that its mutant fails (cdom's PERF_LOG.md, "Port fixes"), and this port moved onto it; the port's own workaround for each is gone.

### Closed

1. **A clone inside an open element was never attached.**
   Closed: the shim's clone appends to the open element, as open does (one `push` helper for both).
2. **No SVG.**
   Closed: `<svg>` and everything opened inside it is made in the SVG namespace.
   C keeps the depth and calls an `open_svg` import; measured against the port's JS check per open, C is faster (11.7 against 12.0 ms for 40,000 opens).
3. **No timer.**
   Closed: `cd_timer_start(ms, token)` answered by the app's `cd_on_timer(token)`, and `cd_raf_request(token)` by `cd_on_raf(token, t)`; `cd_now()` for work cut into slices.
4. **No network.**
   Closed: cdom's `src/cdom_net.c`, taken from this port's: `cd_ws_open/send/close` and `cd_http`, answered by `cd_on_ws_open/msg/close` and `cd_on_http`, bytes copied into a buffer cdom owns.
5. **No viewport and no window events.**
   Closed: `cd_listen_window(CD_EV_RESIZE)` delivers through `cd_on_event` with the viewport's size, and `cd_viewport()` reads it; the page's `vw` import and its resize listener are gone.
   Still open: a measured element rect (the hand plane is the window less 20 px, written down in C).
   Since the texture pass `cd_viewport()` also reads `devicePixelRatio` (times 100), which sizes the fern.
6. **Every entry point outside `cd_event` must flush itself.**
   Closed: every cdom export flushes after the app's hook; this port has no `cd_flush` left.
7. **A list cannot share its parent with a trailing sibling.**
   Closed: `cd_list.end`; the mini hand's count badge is the list's end anchor and the `cd_move` after each update is gone.
8. **A cleared region leaves its list and its named ids behind.**
   Closed: `cd_list_forget` (the port's `list_forget`, now cdom's), and regions free every id made while they were built; freeing a free id now traps.
   cdom's counter test fills and clears lists and regions 4,000 times with flat ids and nodes.
   This found a leak here: the end screen's Play again button id was never freed.
9. **Optional children have no primitive.**
   Closed: `cd_region_show(&r, want)` / `cd_region_end`; the deck column, the discard pile, the end screen, each seat and each count badge are regions, and their bookkeeping is one call.
10. **`cd_name` names the last node made, not the element just closed.**
    Closed: `cd_name_open(id)` names the innermost open element; `cd_name` keeps its meaning (the text nodes here use it) and says so.
11. **Every string is interned forever.**
    Closed: `cd_set_style(id, prop, value)` and `cd_set_style_num(id, prop, v, unit)`, and one-off strings (`cd_tmp`) for whole styles; the table stays at 178 strings, 4.5 KB, from game 1 on (523 and 125 KB after three games before).
12. **Interning a literal costs a hash per call.**
    Closed: cdom ships `CD_LIT("...")`, the per-site cache through a helper, with the `"" lit` guard; a test checks that a variable does not compile.
13. **Numbers into CSS need a formatter.**
    Closed: `cd_fmt` and `cd_fmt_len`, checked against JS's `String()` for 30,000 values and every unit; `ui.h` lost its own.
14. **Template variants are separate templates.**
    Not changed, on purpose: an attribute slot saves no DOM call (the clone's style is one setAttribute either way), the "10" face differs in an inner element's style, which a slot could only match by removing an attribute, and the thin card is another structure.
15. **cdom's allocator assumes it owns the heap.**
    Closed: cdom's malloc, realloc, free and calloc are weak and `cd_alloc` goes through malloc, so there is one heap whoever brings it; `kernel_glue.c` lost its allocator.
    A link test interleaves cdom's and the app's allocations under both arrangements, and an app that replaces malloc alone traps at the first free.
16. **`mini.c` collides with the kernel's libc.**
    Closed: its two functions are weak.
16a. **Images were set, not kept** (texture pass).
    Closed: `cd_encode_image` / `cd_encode_image_in` hand the encoded file's bytes back to C (RGB or RGBA, placed in a bigger image when only a part of a picture is made), and `cd_set_image_bytes` sets bytes as a `url(blob:)`; they replace `cd_set_image` and `CD_EV_LOAD`.
16b. **Nothing outlived the page** (texture pass).
    Closed: `cdom_store.c`, a key/value store over IndexedDB (put, and get with a deadline that answers a miss); bytes in, bytes out, the version in the key.
    cdom's unit page tests both with mutants (PERF_LOG.md, port-fixes 5d4b07b and cfa702a).
17. **`bot_drive` pulls in every brain through the roster.**
    The kernel's, not cdom's, and still open: `kernel_glue.c` answers the two roster calls for the handwritten brain alone.
    A link per app, by what it calls, is the kernel's doctrine; the roster's dispatch table defeats it.

### Open, and new findings

- **First paint and a texture start**: a timer right after the first frame held `cdom.start` to 165 ms at CPU x4 against 60; two frames and 50 ms later it does not (the store, which costs nothing on the thread, is asked after the two frames alone).
  A cdom "after the first paint" hook would put that knowledge in one place.
- **Chrome encodes a PNG in idle time**: slices on every animation frame left none, and the fern's file came back 700 ms after it was made at x4; the port now waits for each file before the next slice of work.
  A texture made in a worker with its own instance would leave the page's thread free, and is the next lever if the wood or the wool must land sooner at x4 (1.9 and 1.2 s).
- **Timing harness**: the page that makes textures keeps working after a test navigates away from it, and that slowed the next load by 18 ms at x4 when loads followed each other in one renderer; `tools/timing.mjs BLANK=1` starts each load fresh.
- **Correct rounding costs**: the wood's cos in double-double is about 3 times musl's (48 against 17 ns natively); the wood now needs it only for its surfaces (85 ms at x1), and a table-driven correctly rounded cos would cut that again.
- **The fern's multisampling** is the GPU's; 2.3% of its pixels still differ (Textures).
- **Wasm memory does not shrink**: a cold load still grows it by the 8 MiB block (11.8 MiB in all, against 33.2 before this pass); a warm load takes none.
  Weaving the wool in bands, or in a worker's own memory, would take the rest.
- **The fern is not the app's pixels**: drawn at 4 times the card back in device pixels, it is another draw of the same fern; on screen a back differs from React's about as much as the C's full-size fern did (Textures).
- **Resizes and the device pixel ratio**: the wool and the wood do not depend on the window; the fern's size is read once, so a window moved to a screen of another ratio keeps the first one's fern (sharp enough, not re-made).
- **The style attribute after a patch is the browser's serialisation**, not the string C wrote; a test that compares style text after an update has to read properties instead.
- **A one-off string must be used in the very next op.**
  `face_open` had to stop taking a style argument (the template slot writes in between would have sent it early); cdom traps on a stale one-off, which is how it showed.
- **The table grew**: `table_app.c` is 1,325 lines, 251 of them the texture pipeline (making, the crop and rectangles, the store, the keys); in the first port-fixes pass it went from 1,015 to 1,117 with per-property patching (a hand card remembers its box and writes only what moved), which keeps the string table flat and made the clicks faster.
- **Open for live play (not now)**:
  - fetch has no request headers (no Authorization: Bearer, so c_base /v1 is unreachable) and no response headers;
  - the sockets array never frees ids;
  - no send backpressure (bufferedAmount);
  - no fetch abort or deadline;
  - the store has no delete and a failed put is silent;
  - ws_send's .slice() copy is unneeded (send copies).

### Where plain C felt fine

18. **The kernel is just there.**
    `TableView`, `ViewRules`, `ClientPlay` and `HandLayout` are structs read in place; `client_play`, `hand_layout` and `bot_drive` are calls.
    No generated readers, no marshalling, no second copy of a rule: the pills are the kernel's answer and the click is the kernel's move.
19. **Events by tag are enough.**
    A 32-bit tag per listening node (`kind | card id`) and one switch in `cd_on_event` replaced every `onClick` closure, and delegation found the card under a click through its child spans.
    Pill hover needed no event at all: buttons.css already has `:hover`.
20. **The diffing is the app's, region by region, and it was short.**
    Each region compares what it would draw with what it drew (a style id, a count, a mark) and writes only the change; with interned strings, "did the style change" is an integer compare.
21. **No allocation in the render.**
    The board, the selection and every region's state are static; the only allocations are cdom's own (lists and strings).

### Where plain C felt awkward

22. **Inline styles as string building.**
    Most of `table_app.c` is the React components' style objects turned into `sb_s` calls; a style is spelled once per variant instead of being an object spread.
    Classes in a stylesheet would be shorter in C than in TSX, but matching the golden pixel for pixel meant keeping the inline styles.
23. **Tree building reads flat.**
    `OPEN`, attributes, `CLOSE` on separate calls lose the shape the JSX shows; the table indents nothing and relies on comments.
    Small per-component functions (`face_open`, `mark_emit`, `result_row`) were the readable unit.
