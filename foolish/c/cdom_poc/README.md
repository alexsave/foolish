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

Not ported: the flights and other animations, drag and drop, keyboard play and the cover arrows, the tutorial hints, chat, the wool, wood and fern textures (the golden has none of them either), translations, and live play.
Live play against c_base and sign-in are next steps, not part of this PoC; the network imports below are the piece they would stand on.

## Files

- `src/table_app.c`: the table (1015 lines, 905 not blank).
- `src/ui.h`: the string builder for styles and the literal-interning shorthands (59 lines).
- `src/kernel_glue.c`: one allocator for the kernel and cdom, and the one bot brain (33 lines).
- `src/index.html`: the page; `build.sh` inlines the minified shim into it.
- `vendor/`: cdom from `/Users/alex/Dev/cdom-perf`, branch perf, commit 0d0bdd7 (`src/cdom.c`, `src/cdom.h`, `src/mini.c`, `shim/cdom.js`).
  `cdom.c`, `cdom.h` and `mini.c` are unchanged.
  `cdom.js` carries this PoC's changes, each marked `[poc]`: a clone attaches to its open parent, SVG elements are made in the SVG namespace, a `timer` import, and the `net` module.
  `cdom_net.h` is new: the C side of the `net` module.
- `tests/net_test.c`, `tests/net_test.mjs`: the network imports against a local echo server.
- `tools/`: `cdp.mjs` (headless Chrome over CDP and a static server), `test.mjs`, `shots.mjs`, `timing.mjs`, `domcount.mjs`, `domdiff.mjs`, `play.mjs` (plays my seat to the end by clicks), `mutate.py`.
- `shots/`: the screenshots.

## Running it

```
./build.sh                      # out/table.wasm, out/index.html, out/net_test.wasm
node tools/test.mjs             # the table test
node tests/net_test.mjs         # the network imports
node tools/shots.mjs            # shots/*.png (W=390 H=844 for a phone)
node tools/timing.mjs 21 1      # medians over 21 fresh loads, CPU x1 (or x4)
python3 tools/mutate.py         # every mutant, rebuilt and tested, the source restored after
```

Run `npm install` once in `tools/` for terser; without it the shim is inlined unminified.
`build.sh` needs homebrew LLVM (`WASM_CC` overrides it), and uses `wasm-opt` and `brotli` when present.
The tools serve `foolish/` on port 8720 and drive Chrome on 8729.
To play by hand, serve `foolish/` (for example `node -e "import('./c/cdom_poc/tools/cdp.mjs').then((m) => m.serve())"` from `foolish/`) and open http://127.0.0.1:8720/c/cdom_poc/out/index.html.

## Against the React golden

The first paint is pixel-identical to the React golden under the same CSS: `tools/shots.mjs` counts 0 of 786,432 pixels differing at 1024x768 between `shots/react_golden.png` (the recorded React DOM in a page with the app's CSS) and `shots/cdom_midgame.png`.
The measure is checked against itself: with 6s selected the same count is 3,901.
The golden's action area is given back the `bottom: max(10px, env(safe-area-inset-bottom))` that ActionButtons.tsx writes and jsdom could not parse; nothing else in it is touched.
At 390x844 the counts differ (13,952 pixels) because the golden's hand positions were recorded at jsdom's 1024 width.

`tools/domdiff.mjs` diffs the two DOMs tag by tag.
The only differences: the `data-turn` attribute the test reads, the keyboard-play state div and the cover arrows' SVG layer (not ported), the action area's bottom (above), and the seat ring's `left: 49.99999999999999%`, which C prints as `50%`.
The test holds that: the first-paint check compares the whole DOM to the golden with exactly those differences taken out.

## Numbers

Measured on this Mac (headless Chrome 156), CPU x1 and x4 throttled, medians of 21 fresh loads with the cache off; script is the click handler, C and its DOM calls, and the second figure adds style and layout forced at once.

| | x1 | x4 |
| --- | ---: | ---: |
| first contentful paint | 48-56 ms | 74-78 ms |
| cdom.start resolved (wasm fetched, compiled, first render done) | 17-20 ms | 48 ms |
| select a card: script / with style and layout | 0.18 / 0.52 ms | 0.77 / 2.09 ms |
| play the Attack pill | 0.31 / 0.86 ms | 1.49 / 3.58 ms |
| the bot's step (it picks up five cards, three are drawn) | 0.37 / 1.07 ms | 1.77 / 4.70 ms |

First contentful paint waits on the stylesheet's chain of `@import`s; the table itself is in the DOM by the time `cdom.start` resolves.
No React figure is given: the real app's numbers need its server and bundle, which this PoC does not run.

DOM calls per action (`tools/domcount.mjs`): selecting 6s is 16 (one style write on the card, and the Attack pill built: 4 elements, a text node, 2 classes, 3 attributes, 5 inserts); playing it is 26; the bot's pickup, the refill and every repositioned card is 79.

Sizes:
- `table.wasm` 54,992 bytes raw, 22,122 brotli (q11), 24,870 gzip (-9).
  It holds the kernel parts the table uses (the engine, legal moves, the client slot, the masked view, hand layout, bot_drive and the handwritten bot), cdom, and the table.
  For scale, the React app's kernel alone, web.wasm, is 67,311 raw and 31,219 gzip (c/Makefile), before any JavaScript.
- Code by object before wasm-opt: the table 24.8 KB (`render` alone 10.4 KB, everything inlined into it), game.c 11.4 KB, legal.c 6.4 KB, cdom 5.9 KB, client_table.c 4.2 KB, the rest under 2.5 KB each.
- `index.html` with the minified shim inlined: 5,256 bytes, 2,153 brotli.
- The shim: 172 lines, 138 before this PoC's changes (38 lines added, 4 replaced; the network imports are 25 of them).
- The C the table is written in: 1,107 lines (`table_app.c` 1,015, `ui.h` 59, `kernel_glue.c` 33).

## Against the TSX it replaces

The TSX for the same pieces is 1,795 lines: GameBoard 126, CardFace 204, CardBack 56, DeckAndFlipped 80, DiscardPile 44, TableBattles 400, PlayerRing 319, ActionButtons 289, handShape 147, RoleMark 130.
That is not a like-for-like count: the TSX also carries the flights, the row glide, the turning role coin, drag and drop, the tutorial's hints and chat bubbles, and it leans on contexts, `sdk/ts/table/client_table.ts` and generated readers for every kernel struct it touches.
The C needs none of that bridge: a `TableView` is a struct, and `client_play`, `hand_layout` and `bot_drive` are plain calls with no marshalling and no generated layout code.
For what both do, the C is about as long as the TSX, most of it the same inline styles as string literals.

## The tests

`tools/test.mjs` (headless Chrome, real mouse events):
- the first paint is the golden's DOM, but for the listed differences;
- the seeded deal reads back: the hand, the battles, a badge of 21 over 20 stock layers, the marks, no pills;
- a resize re-lays the hand at the new width through `hand_layout`;
- a click selects a card and the kernel's pills follow (7h offers nothing, 6s offers Attack), and a second click deselects;
- Attack plays 6s, the bot answers with a legal move, and the surviving hand cards, battles and seats are the same DOM nodes;
- a game played out by clicks (`tools/play.mjs`) reaches the end screen, and Play again deals six new cards.

`tests/net_test.mjs`: a wasm module sends 8 binary bytes over a WebSocket and gets them echoed, then POSTs them and gets them back reversed.

Mutation check (`tools/mutate.py`), each mutant caught by the check written for it:
- a card face's radius 5px to 4px: the golden check names the first differing tag;
- the bot never chooses a move: the bot-answer check times out on `data-turn`;
- the hand rebuilt on every render: "the three cards left in my hand are the nodes that were there";
- the hand patch writes no style: the selection check finds a black border;
- the shim's clone fix taken out: the golden check, then the seeded deal (empty battles);
- no end screen: the play-out check finds no `[data-end]`;
- no resize listener: the resize check times out;
- the WebSocket bytes flipped in the shim: "ws failed";
- the HTTP body not sent: "http wrong".

## WHAT THE APP NEEDED FROM CDOM

Each item is a gap hit, a helper wished for, or a note on where plain C felt awkward or fine.

### Bugs and missing operations in cdom

1. **A clone inside an open element was never attached.**
   The shim's `clone` pushed the clone on the build stack but did not append it to the open parent, so `cd_clone` only worked as a root (the bench's rows are roots).
   A card face is a template cloned inside a battle cell, the trump slot and the hand, and all of them came out empty.
   Fixed in the vendored shim with one `appendChild`, as `open` does; it belongs in cdom.
2. **No SVG.**
   `open` used `createElement`, so the role marks' `<svg>`, `<path>` and `<circle>` were HTML elements and drew nothing.
   The vendored shim now makes `svg`, and anything opened inside an SVG element, in the SVG namespace, as the HTML parser does; the C API is unchanged and the cost is one property read per open.
3. **No timer.**
   The bot's pace needs `setTimeout`.
   Added a `timer(ms, token)` import and a `cd_timer(token)` export the app implements.
4. **No network.**
   Added the `net` module: `ws_open(url)`, `ws_send(id, ptr, len)`, `ws_close(id)` and `http(token, method, url, body)`, answered by the exports `cd_ws_open`, `cd_ws_msg`, `cd_ws_close` and `cd_http_done`.
   Incoming bytes go into C memory through `cd_net_buf(n)`, which the app answers with room for them: no strings and no JSON cross.
   25 lines in the shim, `vendor/cdom_net.h` for the C side.
5. **No viewport and no window events.**
   The hand's layout needs the plane's width, and cdom has no way to read a size or a rect, and delegates events only on the mount.
   The page passes `env.vw()` (innerWidth) and adds its own `resize` listener that calls the app's `app_resize` export.
   A measured width (the plane's own rect) and window-level events (resize, visibility, keyboard shortcuts) would make this the app's, not the page's.
6. **Every entry point outside `cd_event` must flush itself.**
   `cd_timer`, `app_resize` and the network answers each end in `cd_flush()`; one forgotten flush leaves strings and a held walk unsent.
   An `CD_ENTRY` wrapper, or the shim flushing after every export it calls, would make it impossible to forget.

### The keyed list and named nodes

7. **A list cannot share its parent with a trailing sibling.**
   A new last item is inserted before nothing, which is after any sibling that follows the list: the mini hand's count badge ended up between the cards.
   The table moves the badge back to the end with `cd_move` after each update.
   A list end anchor (insert new tail items before node X) would remove that.
8. **A cleared region leaves its list and its named ids behind.**
   `cd_clear` drops the DOM but the `cd_list` still holds its items and every `cd_name`d id stays allocated, so the table has `list_forget` (free each item's id, empty the list) and frees each sub id by hand.
   Ids are manual everywhere: every node named inside a list item has to be freed in that list's `destroy`, and a missed one leaks quietly.
   A `cd_list_reset`, and a clear that frees the ids under it, would cover the common cases.
9. **Optional children have no primitive.**
   The stock's pile, the trump's slot, the suit icon, the discard pile, a seat's count badge and the end screen each come and go, and each needs its own present-or-not bookkeeping with create and remove calls.
   A zero-or-one list (`cd_opt`) would make them all one line.
10. **`cd_name` names the last node made, not the element just closed.**
    After `cd_close()` the last node is the deepest node built inside, so naming has to come straight after `cd_open`.
    It is documented, and it is still the easiest mistake to make.

### Strings and styles

11. **Every string is interned forever.**
    The table's look is inline styles, so each card's style, which carries its position, is a 500 to 700 byte string, and each new position is a new interned string that neither C nor the shim ever frees.
    Measured with the shim's string table exposed for one run: 174 strings (16 KB of text) after the first paint, then 358 (65 KB), 388 (74 KB) and 523 (125 KB) after each of three games played out, held twice (C's pool and the shim's table).
    The positions repeat, so it grows slowly, but it only grows.
    Wished for: transient strings even with `CDOM_INTERN_TEXT=1` (an op that sends its bytes and caches nothing), or better, a `cd_set_style(id, prop, value)` op that writes one property (`left`) instead of the whole style.
12. **Interning a literal costs a hash per call.**
    `ui.h` has `ZS("lit")`, a per-call-site cache of the interned id.
    It found its own bug: one call site passed a variable, so the cache answered its first value forever and every pill's `data-pill` said Attack; `"" lit` in the macro now makes a non-literal a compile error.
    The per-site cache also costs code: inlined, it put 5.5 KB on the module, and calling a small `zs_` helper took the raw size from 60.5 KB to 55.0 KB.
    cdom should ship this macro, with the compile-time guard.
13. **Numbers into CSS need a formatter.**
    `cd_text_num` covers integers in text; lengths are fractional (`22.5px`, a hand slot at `207.333px`), so `ui.h` has a small builder with four-decimal lengths.
    Every app writing styles will need the same few functions.
14. **Template variants are separate templates.**
    Slots made a card face one clone with five text writes, but the "10" face draws its rank tighter and a thin card has another layout, so there are three templates, and a hand card that turns thin changes its key so the list makes it again.
    The root's own style and data attributes are still per-clone attribute calls; slots for attributes would cover them.

### The allocator and the link

15. **cdom's allocator assumes it owns the heap.**
    `cd_alloc` starts at `__heap_base`, and so does the kernel's bump allocator (`shared/c/wasm/bump_alloc.c`): linked together they would hand out the same bytes.
    `kernel_glue.c` answers the kernel's `malloc` with `cd_alloc`.
    cdom should say it owns the heap, or take an allocator from the app.
16. **`mini.c` collides with the kernel's libc** (`memcmp`, `strlen`), so the table links the kernel's `shared/c/wasm/libc.c` and not `mini.c`.
17. **`bot_drive` pulls in every brain through the roster.**
    The roster's dispatch reaches the Monte-Carlo searches and cordite's solver, so `kernel_glue.c` answers the two roster calls `bot_drive` makes for the handwritten brain alone.
    Not a cdom gap, but it is what kept this module at 55 KB: a link per app, by what it calls, is the kernel's own doctrine and it works here unchanged.

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
