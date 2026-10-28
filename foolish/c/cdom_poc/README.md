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

The wool, wood and fern textures are made in C after the first paint (Textures, below).
Not ported: the flights and other animations, drag and drop, keyboard play and the cover arrows, the tutorial hints, chat, translations, and live play.
Live play against c_base and sign-in are next steps, not part of this PoC; the network imports below are the piece they would stand on.

## Files

- `src/table_app.c`: the table (1,117 lines; 63 of them the texture pipeline).
- `src/ui.h`: element shorthands over cdom's `CD_LIT`, and the style builder whose result crosses as a one-off string (36 lines).
- `src/kernel_glue.c`: the one bot brain (20 lines); the allocator is cdom's.
- `src/textures.c`, `src/textures.h`: the wool, the wood and the fern, ports of their TS generators (315 lines).
- `src/crtrig.c`: sin and cos rounded correctly, as Chrome's are (143 lines).
- `src/jsmath.c`: musl's tan and fmod, generated from emsdk's copy of musl (1,206 lines, mostly the reduction table and its comments).
- `src/index.html`: the page; `build.sh` inlines the minified shim into it.
- `vendor/`: cdom from `/Users/alex/Dev/cdom-fixes`, branch port-fixes, commit 6380ee7 (the sources are the same at the branch head, 19bf73b), unchanged (`src/cdom.c`, `src/cdom.h`, `src/cdom_net.c`, `src/cdom_net.h`, `src/mini.c`, `shim/cdom.js`).
  The table does not link `mini.c` (the kernel's libc brings memcmp and strlen) nor `cdom_net.c` (it has no network yet); the network test links both.
- `tests/net_test.c`, `tests/net_test.mjs`: cdom's network imports against a local echo server.
- `tests/tex_test.c`: the texture generators alone, for `tools/texparity.mjs`.
- `tools/`: `cdp.mjs` (headless Chrome over CDP and a static server), `test.mjs`, `shots.mjs`, `timing.mjs`, `strings.mjs`, `texparity.mjs`, `domcount.mjs`, `domdiff.mjs`, `play.mjs` (plays my seat to the end by clicks), `mutate.py`.
- `shots/`: the screenshots.

## Running it

```
./build.sh                      # out/table.wasm, out/index.html, out/net_test.wasm, out/tex_test.wasm
node tools/test.mjs             # the table test
node tests/net_test.mjs         # the network imports
node tools/strings.mjs          # the string table over three games
node tools/texparity.mjs        # the C textures against the TS generators (writes out/tex_ts.js)
node tools/shots.mjs            # shots/*.png (W=390 H=844 for a phone); run texparity first
node tools/timing.mjs 21 1      # medians over 21 fresh loads, CPU x1 (or x4); PAGE=..., BLANK=1
python3 tools/mutate.py         # every mutant, rebuilt and tested, the source restored after
```

Run `npm install` once in `tools/` for terser; without it the shim is inlined unminified.
`tools/texparity.mjs` bundles the app's TS with esbuild from the main checkout's node_modules (`/Users/alex/Dev/foolish/foolish/node_modules`).
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

With textures, `shots/react_golden_tex.png` is the golden with the app's TS textures made from the C's seed and put where WoolBackgroundLayer, getTextureStyle, CardBack and PlayerRing put them; against `shots/cdom_tex_midgame.png` 6,506 of 786,432 pixels differ, every one of them on a card back (the deck and both mini hands): the fern's multisampling residue (Textures).
At 390x844 the count is 20,763, the 13,952 above plus the card backs.

## Textures

The three textures the React table shows are made in C and land after the first paint.
- The wool behind the table: `paintWool` (src/utils/woolPixels.ts), 3840x2160, encoded as JPEG at 0.92 as WoolBackground does.
- The wood on the buttons: `generateWoodPixelData` (src/components/WoodTexture.tsx), 1920x1080.
  WoodTexture draws its float colours through WebGL, but the shader is a one-point-per-pixel blit with no maths of its own, so C computes the same float32 colours and turns each into a byte as the blit does; no WebGL is needed.
- The fern on the card backs: `generateFernPoints` (src/utils/fernFractal.tsx), the card size, 1000x1400, 5,000,000 points.
  The app draws the points through WebGL into a canvas that is multisampled (antialias defaults to true; ANGLE on Metal here, 4 samples); C draws them into 4 samples per pixel at Metal's standard positions and resolves them the same way.

How they land: one new cdom import, `cd_set_image(id, prop, rgba, w, h, mime, token)`, puts the RGBA bytes in C memory into a canvas, encodes them (toBlob), and sets a style property of a node to the `url(blob:...)`; then `CD_EV_LOAD` arrives with the token.
The table sets `--tex-fern`, `--tex-wood` and `--tex-wool` on the mount, and its styles read them through `var()`, falling back to the golden's look; when the fern lands it sets `--tex-fern-bg: #000000`, the black CardBack and PlayerRing give a back with a pattern.
That is 5 lines of shim (cdom PERF_LOG.md, SHIM_LINES).

When: two frames and 50 ms after the first paint, then in slices of about 8 ms on animation frames (cd_raf_request, cd_now), the fern, the wood, then the wool, each in turn in one 32 MiB block.
First paint does not move: `BLANK=1 tools/timing.mjs`, 21 fresh loads, first contentful paint 102.5 ms with textures and 101.7 without at CPU x1, 187.9 and 186.9 at x4; cdom.start resolved 64.8 and 64.6, 60.5 and 61.9.
(Starting on a timer right after the first frame did move it: 165 ms against 60 at x4.)
The cost after it, on the page's thread: fern 286 ms, wood 4,084 ms, wool 3,599 ms at x1 (all three landed 9.0 s after load), 1.0 s, 16.0 s and 14.8 s at x4 (32.6 s).
The TS takes 0.3 s, 1.6 s and 0.6 s for the same three in this Chrome; C is slower on the wood and the wool because its sin and cos are rounded correctly in double-double arithmetic, which is what makes them exact (below).
The wasm memory grows to 33.2 MB for the block, as the app's worker canvas does; wasm memory does not shrink after.

Parity (`tools/texparity.mjs`, seed 20261028, the TS run in headless Chrome with Math.random replaced by the same mulberry32 the C uses):
| texture | compared with | pixels differing | max channel difference |
| --- | --- | ---: | ---: |
| wool 3840x2160 | paintWool's buffer | 0 of 8,294,400 | 0 |
| wood 1920x1080 | generateWoodPixelData's float32 colours | 0 of 6,220,800 values | 0 |
| wood 1920x1080 | the app's WebGL picture (the PNG it makes) | 0 of 2,073,600 | 0 |
| fern 1000x1400 | generateFernPoints' points, one pixel each | 0 of 1,400,000 | 0 |
| fern 1000x1400 | the app's WebGL picture, multisampled | 31,678 of 1,400,000 (2.3%) | 255 |
And for the record: the wood against WoodTexture's CPU fallback (8-bit accumulation, a different picture) 1,831,941 differ; the fern drawn one pixel per point against the WebGL picture 587,297 differ (that is what the multisampling buys).

Where they differ and why:
- sin and cos: Chrome's Math.sin and Math.cos are correctly rounded (V8 takes them from LLVM's libc); musl's (fdlibm) differ in the last bit for about 2.8% of arguments, and the wood's 25 iterations of `b = cos(...) * b - 2.8` grow that into 71% of its pixels.
  `src/crtrig.c` rounds correctly: 0 of 3,000,000 arguments differ from Chrome's (texparity checks it every run).
  tan stays musl's: it differs from Chrome's for 3.3% of arguments, but the wool's only use of it is `> 0.3`, and the wool is exact.
- The fern's 2.3%: the GPU's rules at the edges of a point's square (which samples a point exactly on a boundary covers, and the precision of positions after the vertex shader) are not documented; four variants (float32 shader arithmetic, 1/256 and 1/16 snapping, inclusive edges) did not do better than the plain square.
  On another GPU the app's own fern differs from this one in the same way.

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
For the textures' own timings see Textures.
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
- `table.wasm` 71,725 bytes raw, 29,589 brotli (q11), 32,956 gzip (-9); before this pass 54,992, 22,122 and 24,870.
  The textures are 14.5 KB raw of it (57,214 without them): the generators, the correctly rounded sin and cos, and musl's tan with its 2/pi table.
  The rest of the growth (+2.2 KB) is cdom's: regions, style ops, the formatter, timers, the window, the image import.
  For scale, the React app's kernel alone, web.wasm, is 67,311 raw and 31,219 gzip (c/Makefile), before any JavaScript, and its textures are in its JavaScript.
- The shim: 184 lines, cdom's own, unchanged (138 at perf; the 46 new lines are justified one by one in cdom's PERF_LOG.md, SHIM_LINES).
  Before this pass the port carried 172 lines of its own patched shim.
- The C the table is written in: 1,173 lines (`table_app.c` 1,117, `ui.h` 36, `kernel_glue.c` 20), plus the textures (315), crtrig.c (143) and the generated jsmath.c.

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
- the textures land after the first paint: none is set when the table first shows, then all three and the black back are, and a card back, a button and the wool show a `url(blob:...)`;
- a game played out by clicks (`tools/play.mjs`, textures on) reaches the end screen, and Play again deals six new cards.

`tests/net_test.mjs`: a wasm module sends 8 binary bytes over a WebSocket and gets them echoed, then POSTs them and gets them back reversed (cdom's network, with no cd_flush anywhere).
`tools/strings.mjs`: the string table may not grow from game 2 to game 3.
`tools/texparity.mjs`: the PRNG, sin and cos, the wool, the wood (floats and the WebGL picture) and the fern's points exact; the multisampled fern within 3% of the WebGL picture.

Mutation check (`tools/mutate.py`), each of 16 mutants caught by the check written for it:
- a card face's radius 5px to 4px: the golden check names the first differing tag;
- the bot never chooses a move: the bot-answer check times out on `data-turn`;
- the hand rebuilt on every render: "the three cards left in my hand are the nodes that were there";
- the hand patch writes black for a selected card: the selection check finds a black border;
- cdom's shim clone not attached: the golden check, then the seeded deal (empty battles);
- no end screen: the play-out check finds no `[data-end]`;
- no resize listener: the resize check times out;
- the count badge not the list's end anchor: the bot-answer check finds the count away from the end;
- no black under the fern: the texture check;
- styles interned again: `tools/strings.mjs` (359 strings, 76 KB after game 3);
- the wood's edge alpha off by 1e-7, the wool's clamp truncating, the fern's sample positions flipped, a double-double product dropping a term: `tools/texparity.mjs`;
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
17. **`bot_drive` pulls in every brain through the roster.**
    The kernel's, not cdom's, and still open: `kernel_glue.c` answers the two roster calls for the handwritten brain alone.
    A link per app, by what it calls, is the kernel's doctrine; the roster's dispatch table defeats it.

### Open, and new findings

- **First paint and a texture start**: a timer right after the first frame held `cdom.start` to 165 ms at CPU x4 against 60; two frames and 50 ms later it does not.
  A cdom "after the first paint" hook would put that knowledge in one place.
- **Timing harness**: the page that makes textures keeps working after a test navigates away from it, and that slowed the next load by 18 ms at x4 when loads followed each other in one renderer; `tools/timing.mjs BLANK=1` starts each load fresh.
- **Correct rounding costs**: sin and cos in double-double make the wood and the wool 2.5 and 5.6 times slower than the TS; a table-driven method, or the textures in a worker with their own instance, would win that back.
- **The fern's multisampling** is the GPU's; 2.3% of its pixels still differ (Textures).
- **Wasm memory does not shrink**: the 33.2 MB texture block stays reserved after the textures are made.
- **The style attribute after a patch is the browser's serialisation**, not the string C wrote; a test that compares style text after an update has to read properties instead.
- **A one-off string must be used in the very next op.**
  `face_open` had to stop taking a style argument (the template slot writes in between would have sent it early); cdom traps on a stale one-off, which is how it showed.
- **The table grew**: `table_app.c` is 1,117 lines against 1,015, 63 of them the texture pipeline; the rest is per-property patching (a hand card now remembers its box and writes only what moved), which is what keeps the string table flat and made the clicks faster.
  `ui.h` (59 to 36) and `kernel_glue.c` (33 to 20) shrank.

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
