# cdom_poc: foolish's game table, drawn from C

A proof of concept: the web game table (src/components/GameBoard.tsx and the GameDisplay components) written as one plain C file that drives the DOM through cdom, over the app's own CSS.
It is local only and not wired into the web app.

## What was built

`src/table_app.c` draws a two-seat table, me against the handwritten bot, with the React table's look: the hand, the battles, the stock and the trump, the discard pile, both seats with their role marks and mini hands, the pill column, and an end screen.
The look is the React components' inline styles, copied value for value, over `src/styles/index.css` linked as the app links it, with `data-theme="default"`.
The board is the kernel's, called directly as C: a `Game`, read for the viewer with `client_adopt_board` into a `TableView` and `client_view_rules`, the pills and the meaning of a click from `client_play`, the move written by `client_move_wire` and applied by `awire_apply`, and the bot's answer from `bot_drive`.
The first board is the one `e2e/ui_dom_snapshots.test.ts` `twoSeatBout()` composes for the golden `bout_2p_attacker.html` (trump Kc, my 6s 7h Qd Ad against 9h Th Jh 6c, 6h/8h and 6d on the table).
"Play again" deals a fresh seeded game.
A reload lands in the same game: the kernel's state blob is kept in cdom's store after every move and adopted as the table starts (Reload, below).

Playing: tap a card to select it (red border) and the kernel's pills appear; click a pill, the table (attack or transfer) or a battle (cover) to play; or drag a card onto the table or a battle, by mouse or by touch, or along the hand to rearrange it; or play from the keyboard, by letters and numbers or by the arrows' cursor (Drag and drop, Keyboard, below).
The bot answers when the kernel says (`bot_wait_ms`: 3000 ms after the last move shown with a human in the game), one visible move per step, and the game runs to its end screen.
Every region keeps what it last drew and emits only the difference: the hand, the battles, the pills, the stock and discard layers and each mini hand are cdom keyed lists; the counts, the role marks and the hand positions are patched in place.
Nothing is rebuilt on a move: the test tags the nodes before a move and finds the same nodes after it.

The wool, wood and fern textures are made in C after the first paint, only as much of each as the page shows, and kept across loads in cdom's store as the app keeps its own (Textures, below).
Every card flies as the React table flies it, from the kernel's own animation plan, and the role marks turn and change hands as React's do (Animations, below).
Not ported: the cover arrows (dead in React: nothing fills the map they draw), the tutorial hints, chat, translations, sound, and live play.
Live play against c_base and sign-in are next steps, not part of this PoC; the network imports they would stand on are proved against a real local c_base (Network, below).

## Files

- `src/table_app.c`: the table (3,142 lines, the texture pipeline, the animations, drag and drop, the keyboard and the test build's window on the kernel among them).
- `src/anim_run.c`, `src/anim_run.h`: an operation captured as the kernel's stream for my seat and read back as the web reads a push (97 lines); linked by the table and by `tests/run_test.c`.
- `src/game_keep.c`, `src/game_keep.h`: the game as a host keeps it: canonical after every operation, its blob written and adopted (48 lines); linked by the table and by `tests/blob_test.c`.
- `src/ui.h`: element shorthands over cdom's `CD_LIT`, and the style builder whose result crosses as a one-off string (36 lines).
- `src/kernel_glue.c`: the one bot brain (20 lines); the allocator is cdom's.
- `src/textures.c`, `src/textures.h`: the wool, the wood and the fern, ports of their TS generators, the wool kept in a crop and the wood in rectangles (385 lines).
- `src/trig.c`: sin, cos and tan for the wool and the fern (musl's polynomials, a reduction for arguments below 2^28) and JS's `%` for the wood (141 lines).
- `src/crtrig.c`: cos rounded correctly, as Chrome's is, for the wood (139 lines).
- `src/index.html`: the page; `build.sh` inlines the minified shim into it.
- `vendor/`: cdom from `/Users/alex/Dev/cdom-fixes`, branch port-fixes, commit 2ff8218 (the drag and keys pass: mouse and touch events, the key's character and the event's time in the record, `cd_prevent_default`, `cd_unlisten_window`, `cd_hit`), unchanged (`src/cdom.c`, `src/cdom.h`, `src/cdom_net.c`, `src/cdom_net.h`, `src/cdom_store.c`, `src/cdom_store.h`, `src/mini.c`, `shim/cdom.js`).
  The table links `cdom_store.c` (the textures across loads) but not `mini.c` (the kernel's libc brings memcmp and strlen) nor `cdom_net.c` (it has no network yet); the network tests link the network.
- `tests/net_test.c`, `tests/net_test.mjs`: cdom's network imports against a local echo server (the fast test).
- `tests/live_test.c`, `tests/live_test.mjs`: the contract test: the kernel's network client (`netclient.c`, the web's protocol owner) hosted over cdom's imports as `src/common/backend.ts` hosts it, against a real local c_base (Network, below).
- `tests/tex_test.c`: the texture generators alone, for `tools/texparity.mjs`.
- `tests/blob_test.c`: native; 300 games round-tripped through the state blob at every step (110 lines).
- `tests/run_test.c`: native; every operation of 200 games captured and planned as the table does (161 lines).
- `tools/`: `cdp.mjs` (headless Chrome over CDP and a static server), `test.mjs`, `shots.mjs`, `timing.mjs`, `textime.mjs` (the textures' timing against the app's), `reacttime.mjs` (the same against the real React app, its production build on a local c_base), `strings.mjs`, `texparity.mjs`, `domcount.mjs`, `domdiff.mjs`, `play.mjs` (plays my seat to the end by clicks, mouse drags, touch drags or the arrow keys), `input.mjs` (real CDP input: a mouse or touch drag in steps, keys), `gestureshots.mjs` (the gestures held mid-way, shots), `film.mjs` (a scenario recorded frame by frame into a contact sheet and a GIF), `consist.mjs` (the DOM against the kernel and React's rules at every click and bot step), `mutate.py`.
- `out/table_debug.wasm`: the test build, the same table with `TABLE_DEBUG` (`?debug` on the page loads it): the kernel's state as text (`app_debug_state`), the bot's steps held for the test (`app_debug_hold`, `app_debug_release`) or made 30 ms (`app_debug_fast`, the clock put back, the kernel's rule untouched), and the kernel's random streams seeded (`app_debug_reseed`); boards for the gesture checks (`app_debug_board`), and the drag's and the cursor's state (`app_drag`, `app_kb`).
- `shots/`: the screenshots; `shots/anim/` the films (contact sheets and GIFs).

## Running it

```
./build.sh                      # out/table.wasm, out/table_debug.wasm, out/index.html, out/net_test.wasm, out/live_test.wasm, out/tex_test.wasm, out/blob_test
out/blob_test                   # the state blob against the games this table plays (native)
out/run_test                    # every operation captured as the kernel's stream and planned (native)
node tools/film.mjs             # shots/anim: cover-sweep, attack-take, deal (W=390 H=844 for a phone)
node tools/test.mjs             # the table test (ONLY=<regex> runs the checks it matches)
node tools/consist.mjs 3 1      # 3 games by seeded random clicks, drags and keys, the DOM against the kernel at every step
node tools/play.mjs drag 1      # a game played out by mouse drags (clicks, touch, keys the other hands)
node tools/gestureshots.mjs     # shots/drag_*.png and keys_*.png, mid-drag and aiming (W=390 H=844 for a phone)
node tests/net_test.mjs         # the network imports
node tests/live_test.mjs        # the network imports against a local c_base (make -j4 in c_base/ first)
node tools/strings.mjs          # the string table over three games
node tools/texparity.mjs        # the C textures against the TS generators (writes out/tex_ts.js)
node tools/shots.mjs            # shots/*.png (W=390 H=844 for a phone); run texparity first
node tools/timing.mjs 21 1      # medians over 21 fresh loads, CPU x1 (or x4); PAGE=..., BLANK=1, TEX=cold|warm, GESTURES=1
node tools/textime.mjs 9 1      # the textures, port and app, cold and warm, interleaved; CPU x1 (or x4); BEFORE=dir
node tools/reacttime.mjs 11 4   # the textures, port against the real React app and the harness page; CPU x4 (Textures, React baseline)
CPU=4 node tools/texparity.mjs  # the generators' parity, and their times throttled
python3 tools/mutate.py         # every mutant, rebuilt and tested, the source restored after
```

Run `npm install` once in `tools/` for terser; without it the shim is inlined unminified.
`tools/texparity.mjs` and `tools/textime.mjs` bundle the app's TS with esbuild from the main checkout's node_modules (`/Users/alex/Dev/foolish/foolish/node_modules`).
`tools/reacttime.mjs` needs the app's production build in this checkout and c_base built: in `foolish/`, `npm ci` once (its own node_modules: this branch's lockfile is not the main checkout's), then `NEXT_PUBLIC_BACKEND_URL=http://127.0.0.1:8731 npm run build` (the URL is read at build time; `CBASE_PORT` tells the tool another one), `make -j4` in `c_base/`, and `tools/textime.mjs` run once for the harness page.
It starts c_base on that port as `tests/live_test.mjs` does (throwaway secrets and SQLite, `CBASE_DEV=1`), serves the export on 8733 with the site router's rules, signs a fresh user up on the app's own welcome page, creates a game, adds a bot and presses Ready, then times every load; `PROBE=1` stops after one load of each with screenshots in `out/reacttime_*.png`.
`build.sh` needs homebrew LLVM (`WASM_CC` overrides it), and uses `wasm-opt` and `brotli` when present.
The tools serve `foolish/` on port 8720 and drive Chrome on 8729.
`?notex` on the page leaves the textures out (the golden comparisons use it); `?debug` loads the test build.
A load adopts the game the last load kept: the tools that need the golden's board load it through `openTable` (tools/cdp.mjs), which forgets the kept game first.
To play by hand, serve `foolish/` (for example `node -e "import('./c/cdom_poc/tools/cdp.mjs').then((m) => m.serve())"` from `foolish/`) and open http://127.0.0.1:8720/c/cdom_poc/out/index.html.

## Against the React golden

The first paint is pixel-identical to the React golden under the same CSS: `tools/shots.mjs` counts 0 of 786,432 pixels differing at 1024x768 between `shots/react_golden.png` (the recorded React DOM in a page with the app's CSS) and `shots/cdom_midgame.png` (`?notex`: the golden has no textures).
The measure is checked against itself: with 6s selected the same count is 3,901.
The golden's action area is given back the `bottom: max(10px, env(safe-area-inset-bottom))` that ActionButtons.tsx writes and jsdom could not parse; nothing else in it is touched.
At 390x844 the counts differ (13,952 pixels) because the golden's hand positions were recorded at jsdom's 1024 width.

`tools/domdiff.mjs` diffs the two DOMs tag by tag.
The only differences: the `data-turn` attribute the test reads, the cover arrows' SVG layer (not ported: it never draws in React), the action area's bottom (above), the seat ring's `left: 49.99999999999999%`, which C prints as `50%`, and the texture hooks: `var(--tex-wood, <the golden's stripes>)`, `var(--tex-fern-bg, rgb(180, 14, 9))`, `background-image: var(--tex-fern)` on the card backs and `background-image: var(--tex-wool)` on `.bg-wool`, which fall back to the golden's look until a texture lands.
The test holds that: the first-paint check compares the whole DOM to the golden with exactly those differences taken out.
After the first paint the DOM is the browser's: a patched card is one `style.setProperty`, so Chrome writes the style attribute back in its own form (`transition: 0.1s, visibility` for `all 0.1s ease, visibility 0s`), as it does for React's own updates.

With textures, `shots/react_golden_tex.png` is the golden with the app's TS textures, at the app's sizes (the fern 1000x1400, the wood 1920x1080 whole, the wool 3840x2160 whole), made from the C's seeds and put where WoolBackgroundLayer, getTextureStyle, CardBack and PlayerRing put them.
Against `shots/cdom_tex_midgame.png` 6,643 of 786,432 pixels differ, every one of them on a card back: 4,025 on the deck, 1,520 and 1,098 on the two mini hands (the fern, Textures); the wool and the wood are the same pixels.
At 390x844 the count is 20,901 to 20,997 over three runs: the 13,952 above (the hand, laid out for jsdom's width in the golden), the card backs, nothing on the wool.
`tools/shots.mjs` fails over a bar at both sizes (the first paint exactly as above; with textures at most 8,000 over it), and the mutant that places the wool 16 px off makes it fail (755,744).
The shots run Chrome with `--disable-gpu-rasterization`: with GPU raster Chrome drew one large JPEG two ways, so the same wool bytes under a new url differed from themselves in about 225,000 of 329,160 pixels (by 1 to 20 a channel) in about one load in five while the machine was at a load average of 60, and a run of shots at 390x844 then counted about 245,000; it was never the texture path (the C's crop and the TS's middle decode to the same pixels, and the same bytes did it to the TS's own image).
`shots/fern_backs.png` and `shots/fern_backs_390x844.png` put a card back of each side by side, 4x (React's left).

## Consistency

The owner played the port and found no consistent state.
Reproduced first, by `tools/consist.mjs` and `tests/blob_test.c` at 6e0646936 (before any fix) and by a scripted load, wait, throw-in and reload against that build and this one.
`tools/consist.mjs` plays games by seeded random real clicks (cards picked and put down, pills, a battle, the empty table), holds the bot's steps and releases them one by one, and after every click and every bot step compares the DOM with the kernel's state (worked out afresh, from the game round-tripped through its blob, by a ClientTable of its own) and with a model of React's rules for what the kernel does not hold (spec: GameContext's selection, ActionButtons' pressed pills and rearm, DragContext's drop, WinScreen).
It checks the hand (cards, order, each card's box against the kernel's `hand_layout` slot, the red borders), the pills, the battles, the stock badge and layers, the trump card or icon, the discard pile, each seat's mark and count, whose step it is, and the end screen.

What it and the other repros found, in order, and what each was:
1. **The game the port kept was not the game its blob holds** (a real bug; `tests/blob_test.c`).
   Round-tripping the port's game through `state_blob_put`/`state_blob_load` at every step of seeded games and driving both copies on with the same seeds: seed 1, step 11, a transfer by the defender, and the two boards differ (`players[1].awaiting_attack` 1 in the resident game, 0 in the loaded one; then other moves).
   The defender's raw awaiting flag outlives its turn as defender, the blob writes it as `game_seat_awaiting`, and the passer, an attacker again, carried the dead flag into play.
   The kernel's answer is view.h `state_canonical`: a host that keeps a board between operations keeps it canonical (table.c `table_resident` does).
   Fixed: `game_keep_canonical` after every operation (a move, a bot step, a deal, a load); 300 games, 15,418 steps then agree board for board and drive for drive.
   (The same test first showed two bot seats taking turns in another order after a load: bot_drive.c `drive_seed` counts the log, which the blob drops; canonical drops it too, and the port has one bot seat.)
2. **The bot never moved on the first board** (a real bug).
   The golden's board has the bot defending against 6d; at load `data-turn` said "human" and four seconds later the table was unchanged (3 cards, Anna 4): the bot was scheduled only after a click.
   Fixed: the bot's timer runs exactly while the kernel says it may act (`bot_check` after every change, from the start).
3. **The bot answered 650 ms after a move, the port's own number** (React parity; measured 643 ms after a throw-in).
   Fixed: the kernel's `bot_wait_ms` (The bot's pace, below): 3,020 ms measured after the same throw-in.
4. **The end screen was the port's own** (React parity): "You are the fool", "You won" or "A draw" and rows "🥇Anna", "🃏Me", where WinScreen.tsx says "done - Two of us" (its title), orders the rows by the kernel's `anim_finish_rows` (everyone out, then the fool), gives the fool's card to the last place (so a draw's second out too) and writes "(You)" after my name.
   Fixed: the same title, the kernel's rows, RankIcon's rule and the "(You)" span.
   The button stays "Play again" (the app's "Continue to lobby" goes to a lobby this port does not have).
5. **A reload went back to the seeded deal** (missing; the app re-reads its server's row): before the reload my hand 1-6 3-11 3-13 0-13 0-12 2-6 against an empty table and Anna's 8, after it the golden's 0-5 1-6 3-11 3-13.
   Fixed: Reload, below.
6. **A battle's cover was only ever added** (a real bug, found by the reload check).
   With the kept game drawn over the seeded deal, the golden's 6h/8h battle stayed covered in the DOM where the kept game had 6h uncovered (the list key is the attack card, and the patch assumed a battle is only covered).
   Fixed twice over, for different reasons: a battle's patch now follows its cover both ways (its item holds the cover's card), and the board is no longer drawn before the store answers (Reload).
   No path in play replaces a covered battle with an uncovered one under the same attack card now, so the both-ways patch has no check that can reach it; it is kept as the list's honest rule.
7. **The selection rules differed in code but showed nothing** (React parity, invisible locally).
   The port cleared the selection after any move it applied (Pickup and Good included) and only when applied; React clears it when Attack, Pass or Cover is pressed (refused or not), when a drop is a move, and never on Pickup or Good.
   The port now does exactly that, but nothing a player sees changed: a selection never offers Pickup or Good (one move, one button), and an offered Attack, Pass or Cover is never refused by a local engine.
   Neither the bot's moves nor a card turning illegal clears it, as in React; the pills follow `client_play` (the consistency check holds both).
8. **No pressed pills** (React parity, invisible locally).
   Implemented as ActionButtons does (hidden until the kernel's pill falls and rises again, folded with the empty selection's pills; unpressed on a refusal), but over 3 games and 72 presses (seed 1) no pill was still offered after its move: a press only shows while a move is on its way to a server.
   Its mutant would be equivalent and is not in the list.
Two early reports of the harness were its own: a click on a card's centre that landed on a neighbour still sliding (0.1 s) or lying over it, now clicked where the card shows once every card's transition is over.

## Reload

The React app keeps no game in the browser (spec_state.md section 1): online it re-reads the server's committed row and adopts it through `client_adopt_envelope`.
The port's equivalent is the kernel's own durable form: after every operation the game is made canonical and `state_blob_put` (format v5, 122 bytes for a fresh deal) is kept in cdom's store under `foolish-game-v1`, the store the textures use (one store path).
As the table starts it asks the store (`openStore` opens IndexedDB while the wasm loads) and draws the board when the store answers: the kept game when `state_blob_load` reads it as a two-seat game playing or over, else the seeded deal.
The seats' kinds are not in the blob (the kernel's `table_load` takes them from its roster): seat 0 is the human and seat 1 the handwritten bot again.
The blob carries `has_flipped` and, with it, the trump; an unflipped trump comes back as {0,0}, which `client_adopt_board` hides behind `has_flipped` and `state_canonical` writes on the resident game too, so `tests/blob_test.c` finds the same TableView and ViewRules for both seats either way.
The blob's clock is written zero: it would be this page's time, and a load shows the board anew, so the bot waits its full pace after a reload.
The handwritten bot keeps nothing outside the Game (its random draws come from the kernel's stream, which a load starts anew): the blob is the whole game.
Not kept, as React keeps neither: the selection and the pressed pills.
The hand's order is kept: the order drawn is made the game's own after every operation and every rearrange (`game_rearrange_hand`, Drag and drop), and a load draws the game's order, so a reload draws the hand as it was on screen.
A failed write is not silent: the store answers each put (`cd_on_store_put`), a failure shows "Not saved: a reload would lose this game" at the top of the table until a later put is kept, and the save is tried again 2 s later (every move saves anyway).

When to draw: measured two ways, paired and interleaved (`tools/timing.mjs`, BLANK=1, 3 rounds of 7 fresh loads per arm per throttle, `?notex&debug`, nothing kept), first contentful paint median and 25th-75th percentile:
| | x1 | x4 |
| --- | ---: | ---: |
| A: the seeded deal drawn at once, a kept game adopted over it | 136.6 (116.7-163.7) to 165.7 | 330.0 to 353.5 |
| B: the board drawn when the store answers | 69.5 to 73.0 (66.6-114.2) | 189.4 to 200.4 |
(the range is the three rounds' medians; the store answered at 54 to 58 ms at x1 and 185 to 194 at x4 in B, before the first paint, and cdom.start resolved at 24 to 28 ms in B and 70 to 95 in A.)
Load: averages 29.9 to 41.9 on 8 cores around the runs (another agent's `cnitro_eval` and `cnitro_tests` at 44 to 70% of a core each), above the core count, so the absolute times are upper bounds; the arms ran alternately under the same load and B was faster in every round.
B is the table: the frame (the wool, the title, the back button) paints, the board joins it when the store answers, at most `CD_STORE_DEADLINE_MS` (1,500 ms) when IndexedDB hangs, and a click or a resize before then does nothing.
Why A cost so much more than the render itself was not taken further: B does not move first paint (69.5 to 73.0 against this pass's start, 73 in the texture table below).

## The bot's pace

The kernel owns it: `bot_wait_ms(g, human_mask, shown_ms, settles_ms, now)` (bot_drive.h), the server's rule, says how long the bot waits before it acts on the board in front of it: until the last operation shown has finished playing, then `BOT_PACE_MS_WITH_HUMANS` (3000 ms) with a human in the game.
The port keeps the table's BoardClock in its own page time: `shown_ms` is when the last operation (mine, the bot's, a deal, a load) was shown, and `settles_ms` when it finishes playing: the animation run's last landing, kept after the run has ended (Animations), or `shown_ms` with no run.
The bot's timer runs exactly while the kernel says it may act (`bot_drive_eligible_mask`), and asks the kernel again when it fires: a move of mine in between moves the bot's step back, as the server's loop does (measured before the animations: the bot moved 3,018 to 3,024 ms after my throw-in, 4,088 to 4,128 ms after the load; with them, 3,504 to 3,517 ms after the throw-in, whose run is 500 ms).
A timer cannot be cancelled: each schedule has a generation, and an older one's timer does nothing.
`bot_cycle_delay_ms` (the iMessage loop's fixed sleep after a cycle) is not used: it counts from the bot's own move and ignores a human's, which bot_drive.h names as the reason the server moved to `bot_wait_ms`.

## Animations

Every card flies as the React table flies it, and the kernel decides what flies, where from, where to, in what order and for how long.
The complaint was "no animations": the port drew the new board at once.

### What happens on a move

1. **The operation is captured as the kernel's stream for my seat** (`src/anim_run.c`).
   The React table animates a server's push: the kernel's evwire stream for its seat, read step by step through the client slot (`client_push_open`, `client_push_next`), each step what moved and the board it left (src/state/pushSequence.ts).
   The server writes that stream from the engine's hook snapshots (table.c `table_push`: `evwire_serialize` over the operation's snapshots and logs).
   The port is its own server, so it does both halves with the same kernel calls: `engine_snap_hook` keeps the snapshots while an operation runs (a move through `awire_apply`, a bot step through `bot_drive`, a deal), `evwire_serialize` writes them for my seat (the bot's draws masked), and `client_push_open`/`client_push_next`/`client_push_final` read them back.
   No kernel function was added: the derivation (`evwire_walk`) and the reader were there.
   `tests/run_test.c` (native, 200 games) holds it: the stream's final board is the board the table adopts, the last step's draws the same but for the roles, the plan lasts what `anim_stream_ms` says, the deal is 12 DEAL steps at 350 ms and 6,575 ms in all, the bot's draws are masked and mine named.
2. **The steps join one growing run** (useAnimationRun.ts).
   A run in the air is appended to: the steps already there keep their timing, nothing is spliced or cancelled.
   The plan is the kernel's, asked with each step's event and board (`client_plan_step`, `client_plan`): when each step opens and lands, which steps fly together (a beat: one cover event per card, merged), for how long (350 ms a dealt card, 500 everything else, the 1,500 ms rest after a bout-ending cover), and the stock's cards in the air.
3. **Every frame asks the plan where the run stands** (`anim_plan_at`, a frame loop on `cd_raf_request` while a run plays).
   The board drawn is the last landed step's (every step a skipped frame passed lands in order), or the board the run started on until step 0 lands (a deal starts on the empty table its first step carries).
   The beat in the air flies; the cards it carries are hidden where they left and where they land.
   A frame whose answer did not change draws nothing.
   When the last step has landed the run ends and the game's own board is drawn: the stream's final board, with the roles the closing beat hands on.
4. **The bot is paced from the run's end.**
   `settles_ms()` (pass 2's hook) is the run's last landing, kept after the run has ended, so the kernel's `bot_wait_ms` waits 3,000 ms after the last flight lands, not after the click (measured: the bot stepped 3,504 to 3,517 ms after my throw-in, whose run is 500 ms).
5. **Input is not blocked**, as React does not block it: a move made while a run plays joins it (its steps open after the ones there).
   A click is judged on the board on screen and applied to the game; the kernel refuses a move that the shown board offered but the game no longer allows, and nothing flies (React would fly it and fly it home: Gaps).

A reload in the middle of a flight lands on the committed board with no flight: the game is saved after every operation, and the run is not.

### A flight (FlightCard.tsx, flightGeometry.ts)

Each card of the beat in the air is one node in an overlay (AnimationOverlay.tsx: fixed, over everything, never hit), made when the beat opens and removed when it lands.
It is drawn at the centre of where its card is drawn, at scale 1.5 and the tilt it lies at there; `FLIGHT_ARM_MS` (25 ms, a cd timer, as React's setTimeout) later it is given its landing (the centre of where the card will be drawn, scale 1.8, the tilt it will lie at, the shadow) and a CSS transition of the beat's duration on the flights' curve.
The browser moves it; C decided both ends and the time.
Every endpoint is measured once, when the beat opens, with cdom's `cd_rect` (getBoundingClientRect into C memory, added to cdom for this: WHAT THE APP NEEDED, 16c); React measures in a layout effect at the same moment.

| | React | port |
| --- | --- | --- |
| path | straight between element centres: `left`, `top` and `transform` transition together | the same |
| duration | the step's `duration_ms` (350 a dealt card, 500 otherwise) | the same, from the kernel's plan; the test build's speed knob only runs the clock faster |
| easing | `cubic-bezier(0.25, 0.46, 0.45, 0.94)` | the same |
| arm | one 25 ms setTimeout after the start frame | one 25 ms cd timer (measured 26 to 28 ms from drawn to armed) |
| scale | 1.5, then 1.8 | the same |
| rotation | from the tilt it lay at to the one it lands at, about the card's bottom edge, the pivot paid back by a translate | the same transform string |
| a cover | lands laid across its attack at pi/16, aimed at the attack's centre turned about its bottom edge | the same (`laidAcross`), the attack measured upright before its turn starts |
| a card off the table | lifts off at the tilt it lay at (-pi/16 an attack under a cover, +pi/16 the cover) | the same |
| shadow | `0 0px 0px rgba(0,0,0,0.4)`, then `0 10px 20px`, through the face's own 0.2 s box-shadow transition | the same |
| overlay | `position: fixed`, z-index 10000, `pointer-events: none` | the same |
| another seat's draw | one CardBack stack of the cards from the stock, rotated 90 degrees | the same layers (`back_style`) |
| the trump drawn | the draw's last card, face up, out of its own slot (`AnimPlanStep.trump_out`) | the same |
| several cards into one unmeasured place | 3 px apart each | the same |
| the cards it carries | `visibility: hidden` where they left and where they land, shown at the landing | the same (visibility alone: a hidden card is never hit and nothing moves it) |
| the row | grows a cell for a pile as its step opens (arrivingPiles) and glides (a FLIP) over that step's duration on the same curve | the same: each cell pinned where it is drawn, let go with `transform <ms>ms EASE` |
| an attack under a cover in flight | turns with it: `transform <ms>ms EASE 25ms` | the same |
| my hand | makes room for the cards flying into it (laid out for the held and the arriving), each lands on its slot; `all 0.1s ease` | the same, landing slots from `hand_layout` |
| the stock's badge | drops as cards leave (`in_flight_from_deck`), the trump's slot empties while it flies | the same, through `client_view_rules` with the frame's counts |

### The role marks (useRoleMotion.ts, roleMotion.ts, RoleCoin.tsx, RoleFlightsLayer.tsx)

The badges wear a shown ledger, not the board's roles: a run freezes them and moves them at the kernel's three moments (anim_plan.h "the role beat"):
a good being set leads its stream (`anim_goods_opening`, as the operation joins the run), a good cleared by a throw-in and the shield a transfer hands on turn as that step opens (`anim_goods_cleared`, `anim_pass_hand_off`), and everything else at the closing beat, once the run has landed.
With no run the board's roles are shown as they come (`anim_shown_ledger_allows`, a bystander's write; a finished board's marks change in place).
- **The coin**: a mark becoming another turns over where it stands, `coinFrameAt` ported value for value: the width collapses as the cosine of a steady angle over 80 ms, waits 30 ms edge-on, the face swaps, and it opens as the sine over 110 ms; a mark that begins or ends makes one half. Every duration is a proportion of `ANIM_TIME_MS`.
- **The hand-off**: the shield and the opener's sword, the two marks that belong to a seat and then to another, fly as ghosts when they change hands: a quadratic arc lifted toward the top of the board by min(56, 0.28 x distance) px, scale 1 + 0.45 x lift, a drop shadow of `0 6l 8l rgba(0,0,0,0.45l)`, one whole spin, over 400 ms (0.8 of a card's flight) on the flights' curve (inverted by Newton, as roleFlightEase), armed 25 ms after it is drawn and stepped at most a 60 Hz frame at a time.
  The seat it leaves blanks at once (a pass's old defender then turns its sword in 133 ms later); the seat it flies to turns its own mark away to be edge-on as the ghost lands (290 ms later), and stands the real mark up in the frame the ghost goes; a seat both left and landed on stays blank until then.
  The ghosts are drawn per frame from C (left, top, transform, filter), as React draws them per frame from JS: an arc cannot be one CSS transition.

### Evidence

`tools/film.mjs` records what the page paints, frame by frame, with the compositor's timestamps (CDP `Page.startScreencast`), and keeps every frame in `out/anim/<name>/`, a contact sheet of one frame every 50 ms with its time on it, and a GIF timed as recorded, in `shots/anim/`:
- `cover-sweep`: the bot covers 6d with the trump 6c (the 6d turns under it), I say Good: the table sweeps to the pile, my two cards and the bot's three (one stack of backs) refill from the stock, and the closing beat throws the shield to me and the opener's sword to Anna;
- `attack-take`: the cover, my 6s thrown in (the row glides to make room), the bot takes all five to its seat (the covered pair lifting off at their tilts), my refill into a hand that makes room;
- `deal`: a new deal one card at a time, mine face up and the bot's as backs, the stock counting down, the trump turned last;
- `attack-take_390x844`: the same at a phone's window.

### The numbers

Measured paired and interleaved (`tools/timing.mjs`, 3 rounds of 7 fresh loads per arm per throttle, `?notex&debug`, the arms alternating), "before" the build of this pass's start (028fa3548, in `out_before/`, which replaced the older df7fda82d build that the Numbers table names), "after" this pass's flights and coins (the role ghosts came after the run and do not enter these actions).
Load averages 2.3 to 3.4 on 8 cores around every run (on top: secd, WindowServer, ctkd), below the core count.
Medians of the three rounds' medians (ms, the range of the rounds' medians):
| | before x1 | after x1 | before x4 | after x4 |
| --- | ---: | ---: | ---: | ---: |
| first contentful paint | 27.6 (27.5-28.7) | 27.8 (24.8-29.4) | 58.3 (58.0-58.8) | 59.2 (58.1-59.5) |
| select a card: script / with style and layout | 0.20 / 0.65 | 0.25 / 0.75 | 0.11 / 1.09 | 0.09 / 1.00 |
| play the Attack pill: script / with style and layout | 0.62 / 1.54 | 2.19 / 2.54 | 0.94 / 2.40 | 2.76 / 3.30 |
| the bot's step: script / with style and layout | 0.94 / 2.36 | 1.11 / 2.17 | 1.13 / 3.25 | 1.27 / 2.89 |
| the run after my play: every frame and timer callback until it lands | | 1.77 over 31 frames | | 1.09 over 31 frames |
| the run after the bot's step (its take and the refills) | | 4.42 over 62 frames | | 3.50 over 62 frames |
The play's script now includes layout: the flight's endpoints and the row's glide are measured in the handler (`cd_rect` makes the browser lay out), as React's layout effect does before its paint; the time with style and layout is what the click costs (2.5 against 1.5 ms at x1).
A frame of the run in the air costs nothing measurable: 31 frames for the play's 500 ms come to 1.8 ms, which is the arm and the landing's commit; the frame loop draws only a frame whose answer changed.
The x4 run sums came out lower than x1's in every round; that was not explained, and they are read as small, not as faster.

DOM calls per action (`tools/domcount.mjs`, counted until the action's animations land):
| | before | after: in the handler | after: the frames and timers after it | after: all |
| --- | ---: | ---: | ---: | ---: |
| select 6s | 15 | 15 | 0 | 15 |
| play it (Attack) | 25 | 35 | 22 | 57 |
| the bot's step (a take and the refills) | 77 to 80 | 48 | 142 to 147 | 190 to 195 |
What the extra calls are: a flight is a cloned face and its wrapper (about 4 calls) armed with 5 `setProperty`; the row's glide is 3 a moving cell; a veil 1 each way; the overlay comes and goes; the badge counts as cards leave.

The string table stays flat from game 2 on (`tools/strings.mjs`): 192, 195 and 196 strings, 5.9 to 6.0 KB, at the three games' ends (178 and 4.5 KB before this pass: the flights', the glide's and the coins' literals); every position and transform crosses as a one-off.
`table.wasm` 104,009 bytes, 42,119 brotli, 47,367 gzip (75,330, 31,106 and 34,735 at this pass's start): about 14.1 KB of the code is the kernel now linked (`evwire_serialize` and `evwire_walk`, the push reader with its roster-trailer reader, `anim_build_plan` and `anim_plan_at`), and 11.7 KB the port's own (the run, the flights, the coins, the row's glide; measured on unstripped links, by function), then the ghosts (+3.6 KB).

### Not done, and every way it differs from the React table

- **No revert flight.** React flies a move the server refused home with the red look; here the game is local and nothing refuses a move that flew. A click made on the board on screen while a run plays is judged against the game itself: a move the shown board offered and the game no longer allows is refused (`app_last_reject`) and nothing flies, where React would fly it and fly it back.
- **No sound.** React strikes the plan's cues (`AnimPlan.cues`) on its audio engine (the reference checkout's branch); the port plays none.
- **The ghost's watchdog** (React lands a hand-off that no frame drew) is not ported: the ghosts and the coins step only on frames, at most a 60 Hz frame each, as React's do, so a stalled page stretches them rather than ending them.
- **A coin interrupted mid-turn** restarts from the face it was drawn with; React's RoleCoin has a finer rule for a mark that changes and comes back within a paint ("restore"), which the port takes as "stand it at once" and does not otherwise reproduce.
- **The glide offset** a landing is aimed off is the cell's box against its rest while its glide runs (its end time known); React reads the computed transform. They differ only if the window is resized mid-glide.
- **The plan is built when steps join the run, not every frame**: the same answer (it is a pure function of the run), less work.
- **The held piles** (React's rule for a board that got ahead of its own flight, a server's confirmation beating the card) cannot happen here: every board the port draws is a step's own or the game's after its run.
- **No side-by-side film with React**: the React table needs its server. The look is matched number by number (the table above) and the endpoints are measured in the tests; the films are the port's.
- **Headless timing**: in headless Chrome the screencast and the timers stall now and then (a 500 ms gap with no frame was seen once in a film), which stretches a ghost exactly as a slow device would; the tests judge endpoints and landings, not frame times.
- **The kernel's push reader** links its roster-trailer reader (about 2 KB) though the port's streams carry no roster: an entry that reads a sequence with no identity would not.
- Unchanged from earlier passes and still not ported: the cover arrows (dead in React), the tutorial's hints, chat, translations, live play. Drag and drop and keyboard play came after (their own sections).

## Drag and drop

A card is played by dragging it, as the React table plays it (DragContext.tsx, DragShadow.tsx, TableBattles.tsx, ActionButtons.tsx CardDiv), by mouse and by touch.
The complaint was "no drag and drop": the port took a click on a card and nothing else.

### The gesture

1. **The press** is on a hand card: cdom delivers its `mousedown` or `touchstart` (cdom's delegated events, the card's tag), and the table listens on the document for the moves and the release while it lasts (`cd_listen_window`, removed again with `cd_unlisten_window`), as React adds and removes its document listeners.
   A mouse press stops the browser's own action (`cd_prevent_default`: a card is `draggable`, and the native drag would take the moves); a touch press does not, as React's passive touchstart cannot.
2. **A tap or a drag.** Moved more than 10 px (Euclidean) from the press it is a drag.
   Released within 150 ms, not moved 10 px and not rearranged, it is a tap, which toggles the card in the selection; held longer it does nothing.
   The 150 ms are the two events' own times (`cd_event_rec.ms`, event.timeStamp), as React reads them: a tap whose release waited behind a busy page is still a tap (checked: a release held 250 ms behind a busy loop).
   A card no longer takes a click: the tap is the selection, as React's `onClick={() => true}` leaves it.
3. **Within the hand's area** (the plane and one card's height, 70 px, above it: the plane measured at each move, as React measures it) a drag rearranges: the card under the pointer (`cd_hit`) changes places with the dragged card, live, and the dragged card follows it.
   The dragged card is dimmed (opacity 0.3) meanwhile.
4. **Outside it** the drag is a game action: the card dragged is the one pressed, shown with the selection's red border at full opacity, and the kernel says what a drop here would be.
   The cards the drag carries are the selection when the dragged card is one of it, else the dragged card alone (`client_gesture_cards`); the target is the battle whose attack card is under the pointer, else the open table (`client_drop_target`: a drop cell past the last battle is the table); `client_play` reads them on the board on screen, and is asked again only when the target or the cards change, or the board was drawn again (a bot step mid-drag).
5. **The drop** is where the pointer last moved: a move (`move_type` 0 or more) spends the selection and is played as a pill plays it (`awire_apply`), and its flight is the click's (Animations); anything else does nothing, and the card is simply where it was (React has no snap-back either).
   A rearrange is kept in the game at the drop (`game_rearrange_hand`, the server's own call for ASK_REARRANGE_HAND), so a reload draws it; React sends it 5 s later.
   A touch release stops the browser's mouse events and click after it (`cd_prevent_default` on the touchend), and a touch move stops the page scrolling (the document's touchmove listener is not passive).

### What a drag shows

| | React | port |
| --- | --- | --- |
| the shadow | `.drag-shadow` (cards.css: absolute, translate(-50%, -50%), z-index 2001, pointer-events none) at `left: x, top: y - 40`, the board's first child after its title | the same element, class and place; moved per move with two `setProperty`, rebuilt only when the answer or the cards change |
| its badge | `.drag-shadow__indicator`: `<span>⚔️</span> Attack`, `<span>🛡️</span> Cover`, `🔄 Pass` for the kernel's MOVE_*; none for -1 | the same nodes and text |
| its cards | CardFace each: scale(0.8), transform-origin center, margin-right 2px, box-shadow 0 2px 4px rgba(0, 0, 0, 0.3), opacity 0.9 | the same face template and styles |
| the dragged card | rearranging: opacity 0.3; a game action: border red, opacity 1; `transition: all 0.1s ease` | the same, patched by property |
| a cover's battle | the cell: `border: 3px solid #d29002`, background rgba(210, 144, 2, 0.1) | the same (two `setProperty` each way) |
| an attack or a transfer | one drop cell a card after the battles: the battle cell's style with `3px dashed #d29002`, the same background, radius 8, a 40 x 60 dashed 2px box with a `+` (24 px, opacity 0.7) inside; `data-battle-index` the battle count and up | the same DOM |
| the row making room | at once (the FLIP's 0 ms when only the drop cells change) | at once: every glide cut, the rests measured again |
| the hit test | `document.elementsFromPoint`: every layer under the point | `cd_hit`: the topmost element that takes pointer events, then its nearest listening ancestor (Gaps) |
| a battle's target | the attack card (`data-battle-index` is on it) | the attack card, which listens now (it was the cell) |

### The hand's order

React draws my hand in a sticky order of its own (localHandOrder: clientReconcile.ts `reconcileHandMemory`, `displayedHand`, `reorderHand`, and ServerContext.tsx `setLocalHandOrder`): a card that leaves and comes back keeps its slot, a new card goes to the end, and a drag or Meta+arrow rearranges it.
The port had none (the kernel's order, which appends a card taken back at the end).
These rules were TypeScript; they are the kernel's now (`client_hand_remember`, `client_hand_shown`, `client_hand_swap` in client_table.h), and the port draws its hand through them.
The kernel's `client_rearrange_hand` reorders a view's hand; the game itself is the server here, so the port keeps the order with `game_rearrange_hand` on its game, which is the call the server makes for it, after every rearrange and after every operation (a card taken back keeps the slot it is drawn in).
Found by the reload check, which failed once in a full run: a game whose line took back a card of mine drew it in its remembered slot, and the reload drew it at the end, where the kernel had appended it.
React behaves so too (its server's order changes only by a drag, and a reload resets the remembered order to it); the port keeps what was on screen.

### Kernel rules lifted

`client_gesture_cards`, `client_drop_target`, `client_cursor_play` (Keyboard) and the three `client_hand_*` were TypeScript in the React app (state/view.ts, KeyboardPlayMode.tsx, clientReconcile.ts) and are C in c/src/client_table.c now, with c/tests/tests.c `test_client_drag_and_cursor_gestures` (the cursor's verdict held against the engine over dealt games: 7,048 cards asked, every kind met) and `test_client_hand_order`, and 15 mutants run by hand, all caught (listed in tests.c).
The React app still holds its own copies: they are the same answers, and moving the web onto these is a change of its own.

## Keyboard

Both of React's keyboard systems, on the one document keydown (cdom's `cd_listen_window(CD_EV_KEYDOWN)`, the key's character in the record, `cd_prevent_default` for a handled key), as the live table mounts both.
Neither does anything to a key typed into a field (the record's CD_MOD_EDITING).

**Letters and numbers (KeyboardInputHandler.tsx).** With no Ctrl, Alt or Meta: 1 to 9, 0, - and = toggle the hand's cards 1 to 12 in the order drawn; Space or A attacks with the selection when I do not defend, Space or C covers with it when I do (the Cover button's aim, `client_play` with CLIENT_PLAY_COVER_BUTTON, only when it names a cover); G says Good, P transfers, U takes; each only when its pill shows.
Attack, Cover and Pass spend the selection; Good and Take leave it.

**Arrows (KeyboardPlayMode.tsx).** A cursor on the hand: Left and Right move it, wrapping (the first press puts it on the first card); Shift toggles the card under it; Shift+Left/Right moves and toggles (a sweep); Meta+Left/Right moves the card under it a place (kept in the game, as a drag's is); any other modifier makes an arrow do nothing.
Down is Take or Good, whichever pill shows.
Up with cards selected plays them as the buttons would (Attack; or the Cover button's cover, else Pass); with none it plays the cursor's card as the kernel reads it (`client_cursor_play`): an attacker's attack; a defender's transfer when it covers nothing, its cover when it covers one battle and may not transfer, nothing when it does neither, and otherwise an aim: Left and Right cycle the battles it covers, then the transfer last, Up plays the one shown, Down gives up.
A key's move hides its pill until it is refused, as React's `fire` does; the aim ends after a move, and the cursor stays on its index (on the last card when the hand shrank).

| | React | port |
| --- | --- | --- |
| the state the tests read | `<div data-testid="kbd-state" data-mode="none\|cards\|target" data-sel data-target style="display: none">`, after the vignettes | the same div, in the same place: the golden's first paint now matches it too (one exclusion fewer) |
| the cursor | fixed, `left: x + 0.15 w; top: y + h + 4; width: 0.7 w; height: 3px`, #111, radius 2, `box-shadow: 0 0 5px 1px rgba(255, 255, 255, 0.7)`, z-index 1500, from the card's box polled every frame | the same; the box from the kernel's slot on the measured plane, set when it changes |
| the arrow | a fixed full-window svg, z-index 1500: marker `kbd-arrow` (polygon 0 0, 11 4, 0 8; 11 x 8, ref 9 4, #ff2d2d), a line 3 wide from the card's top centre to the target | the same svg |
| a cover's target | a rect 3 px out round the attack card, rx 8 | the same |
| the transfer's target | the line dashed 7 5 to 46 px right of the table's cells at their middle (the window's centre with none), a 52 x 72 rect dashed 6 4, rx 8, and PASS (13 px, 700) 54 px below | the same |

## Gestures: evidence and numbers

`tools/test.mjs` (real CDP input through `tools/input.mjs`: `Input.dispatchMouseEvent`, `Input.dispatchTouchEvent`, `Input.dispatchKeyEvent`, nothing synthesised in the page), each drag check by mouse and again by touch:
- a card dragged onto the table attacks: let go 40 px above the hand it plays nothing (the hand's area); dragged out, the badge says Attack, the shadow stands 40 px over the pointer, one drop cell opens past the two battles, the dragged card wears the red border; the attack is played and the shadow and the cell are gone;
- board 1 (I defend 7h and Qd holding 9h Kd 6c 6s): 6s onto 7h is no move (no badge, no amber, nothing played, the card back in its place, the Kd selected still); 9h onto 7h covers it, the cell in amber while held, the shadow carrying the 9h alone, the selection spent;
- a 5 px wiggle released at once is a tap, twice toggles back; a 200 ms hold selects nothing; a tap released behind a 250 ms busy page still selects;
- the 6s dragged along the hand ends last, the others moved left one by one, dimmed meanwhile, no tap; the game holds the new order and a reload draws it;
- board 2: the 9h dragged onto the 6h covers it, Down takes: the 9h comes back to its old slot and the 6h goes last, and a reload draws the hand so;
- board 2 (I defend 6h holding 6c 6s 9h Kd): 6s and 6c selected and the 6s dragged out: Pass, both cards in the shadow, two drop cells; both transferred;
- keys: typed into a field nothing happens; 3 toggles the third card; Meta+1 and Ctrl+A do nothing; 1 then A attacks with the six and spends the selection;
- arrows: the cursor wraps, sits at the card's box (x + 0.15 w, y + h + 4, 0.7 w, 3 px, #111) within 0.01 px, Shift toggles, Shift+Right sweeps, Meta+Left moves the card and the cursor with it, kept in the game; the hidden state follows;
- arrows on board 1: Up on the trump 6c aims at the 7h (the line from the card's top centre to the target's centre and the box 3 px out, within 0.01 px), Right cycles to the Qd, Up covers it, the aim and the arrow go;
- arrows on board 2: the 6c aims at the battle, then the transfer: the line dashed 7 5 to 46 px right of the row at its middle, PASS; Down gives up the aim, Down takes; board 3 (all covered): Down says Good;
- a whole game played out by mouse drags, one by touch drags, and one by the arrow keys alone (`tools/play.mjs drag|touch|keys`): each reaches the end screen.

`tools/consist.mjs` (3 games, seed 1) plays mouse and touch drags onto battles and the table, rearranges along the hand, and keys (numbers, letters, arrows, Shift, Up, Down) among its clicks, and judges every step against the kernel and React's model, which now holds the hand's remembered order (the drag's swaps replayed on the kernel's slots, the topmost card under each point) and the keyboard's cursor.
Its choices were rebalanced for this pass (fewer pill presses, more drags and keys); seeds 1, 2 and 3, 3 games each: 995 steps, among them 69 mouse and 58 touch drags onto the table or a battle, 45 rearranges, 80 keys, 110 card taps, 45 battle or table clicks, 179 pill presses and 400 bot steps, and no disagreement.
The test runs seed 1 (321 steps).
The earlier passes' mutants it catches (a drop that is no move spending the selection, the end rows, the "(You)", the veil) are still caught with the new mix, and so is the hand drawn in the game's order.
On the way it found one harness bug of the input's own: `Input.dispatchKeyEvent` with `nativeVirtualKeyCode` 16 (Shift's Windows code, read on macOS as the Mac's own) hid the headless page (`document.hidden`, no more frames), and a run stalled with its animation never landing; `tools/input.mjs` sends no native code.

Shots (`tools/gestureshots.mjs`, textures on, `shots/drag_attack.png`, `drag_cover.png`, `drag_pass.png`, `keys_target.png`, `keys_pass.png`, and each `_390x844`): the 6s held over the table with its Attack badge and drop cell, the 9h held over the 7h in amber, the two sixes held for a transfer over two drop cells, the trump aimed at the Qd, and the 6c aimed at the transfer.
There is no React shot beside them: the React table needs its server to drag on; the look is the table of values above, every one read from the TSX and CSS.

Per action (`tools/timing.mjs 21 1` and `21 4` with `GESTURES=1`, 21 fresh loads each, the test build; script is the handler, C and its DOM calls; medians, 25th-75th percentile).
Load averages 2.2, 1.9 and 1.7 on 8 cores before, between and after the two runs (on top: WindowServer, the wallpaper's extension, MenuBarAgent, at 4 to 22% of a core), below the core count.
| ms | x1 script | x1 with style and layout | x4 script | x4 with style and layout |
| --- | ---: | ---: | ---: | ---: |
| a tap selecting the 6s (press and release) | 0.24 | 0.68 | 0.13 | 1.15 |
| the Attack pill | 2.10 | 2.43 | 2.79 | 3.19 |
| a drag's move out of the hand (the kernel asked, the shadow and the drop cell built) | 0.89 (0.8-1.0) | 1.49 | 1.75 (1.6-2.0) | 2.88 |
| a move with the same answer (the shadow moved) | 0.23 (0.2-0.3) | 0.31 | 0.13 (0.1-0.8) | 0.19 |
| a move onto the 6d (an attacker's card still attacks there) | 0.23 (0.2-0.2) | 0.31 | 0.34 (0.1-0.8) | 0.71 |
| the drop on the table (the attack played, the shadow and cell gone) | 1.45 (1.0-1.7) | 1.71 | 2.66 (2.5-3.0) | 2.88 |
| ArrowRight, the cursor put on | 0.36 (0.3-0.4) | 0.49 | 0.72 (0.2-0.9) | 0.97 |
| 2, a card selected | 0.46 (0.4-0.5) | 0.47 | 0.90 (0.7-1.0) | 0.90 |
A move of a drag costs what a click costs, and most moves (the answer unchanged) are two `setProperty` and no kernel call; the first move out and the drop cost about what the Attack pill does.
These are absolute numbers from one session; there is no React side to pair them with (its table needs its server).
The click rows are not a before and after: the select is now a tap (a press and a release) where it was a click.

DOM calls per action (`tools/domcount.mjs`): the tap selecting the 6s 15 (the click was 15), the drag's move out 38 (the shadow, its badge and card, the drop cell, the dragged card's border), a move with the same answer 2 (left, top), the drop 49 (26 in the handler, 23 in the frames after it; the Attack pill is 57), ArrowRight 8 (the hidden state's two attributes, the cursor made), a number 2.

The string table (`tools/strings.mjs`): 134 strings at the first paint, 198, 199 and 200 at the three games' ends (6.0 KB), flat from game 2 on.
`table.wasm` 117,457 bytes, 46,851 brotli, 52,778 gzip (104,009, 42,119 and 47,367 before this pass): the table's own code grew 12.7 KB in its object, before wasm-opt (render_board 5.7 KB, with the cursor, the arrow, the shadow and the drop cells inlined into it; cd_on_event 3.2 KB, with the key and drag handlers; drag_ask 1.0 KB), the kernel's client_table 1.3 KB (the lifted rules).


### Every way it differs from the React table

- **The hit test** is the topmost element that takes pointer events (`elementFromPoint`), React's every layer (`elementsFromPoint`): where a seat or a flight that takes pointer events lies over a battle, React still finds the battle and the port finds the table. On the golden board and board 1, at 1024x768 and 390x844, no point of an attack card (3,300 points, 3 px apart) has anything over it that takes pointer events.
- **React's state lags one event**: `isActuallyDragging` set in a move is read in the next, so React's first move past 10 px is still in the hand's terms; the port's is a drag at once.
- **The rearrange is kept at the drop**, React's 5 s later (so a React reload within 5 s loses it); and Meta+arrows are kept too, which React's keyboard leaves local (setLocalHandOrder alone, never sent); and the drawn order is kept after every move, where a React reload puts a card taken back where its server appended it.
- **A touch that the browser cancels** (touchcancel) ends nothing, in React or here: the drag waits for the next release.
- **The cursor and the arrow** are placed when the table draws, not every frame: React polls the card's box per frame and so follows the hand's 0.1 s slide and a row's glide; here they jump to where those end.
- **Focus in the pill column**: React's letters and numbers do nothing while the focused element is inside `[data-touch-interactive]` (a pill button just clicked keeps the focus); the port does not track focus, so after a pill click its letters still work.
- **The cover arrows** (CoverArrows.tsx) are not ported: React mounts them, but nothing ever fills the map they draw from, so they never show.
- **The rules lifted into the kernel** still have their TypeScript copies in the React app.

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

When: the store is asked for all four at once as the table starts (`cd_main`), and the page opens the store's database while the wasm loads (cdom's `start` option `openStore`), so a kept texture lands as soon as it is read; making waits for two frames after the first paint and 50 ms more (started on a timer right after the first frame, making held `cdom.start` to 165 ms at x4 against 60), and keeps its order, waiting for a texture the store has not answered for yet.
The store's tokens: 0 to 3 the textures, 100 the game (`STORE_GAME`, where it is declared); `cd_on_store` hands the game's to the game first.
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

### React baseline

The harness page above (`out/ts_tex.html`) runs the app's three texture functions at navigation with nothing else to do.
The real app must load its bundle and its kernel, restore the session, fetch the game from its server and mount the table before its texture hooks show anything, so `tools/reacttime.mjs` times the real thing in the same rig and throttle.
The build: this checkout's app (branch cdom-port at 2ceb588e9), `npm ci`, then `NEXT_PUBLIC_BACKEND_URL=http://127.0.0.1:8731 npm run build`: Next.js 16.2.6, the static export `next build` makes for production (`out/`), served with the rules of the site's CloudFront router (c_base/providers/aws/ops/site/web.yaml), without the cross-origin isolation headers, which production turns on only with a flag.
The server: `c_base/build/cbase serve` on 127.0.0.1:8731, throwaway secrets and SQLite, as `tests/live_test.mjs` starts it.
The route: a fresh user signed up on the app's welcome page (`/`), a game created on `/dashboard`, a bot added and Ready pressed in its lobby, and then every timed load opens `/<game id>` (the game page, `_shell.html`), the table against the bot, from about:blank, so the app's module-level texture variables start empty each time.
Its textures do not wait for that page in the app: the fern starts in FernFractalProvider, which ProtectedRoute puts around every signed-in page, the wool is behind the kernel's gate and the loading screen as well, and the wood is on the welcome page, the dashboard and the lobby.
A player who walks from the dashboard to a table has all three in memory already; these loads are a reload or an opened link, the case where both apps start from nothing but their stores.
Cold: TextureCacheDB (React) and the port's store emptied before the round, and checked: as a React page starts (`indexedDB.databases()`, asked before the app's scripts run) a cold load finds no TextureCacheDB and a warm one finds it, every React load leaves fern_v1, wood_v2 and wool_v1 kept, and every port load finds its keys as the arm says.
The start check went red, by hand, for the right load both ways: React's origin not emptied ("react_cold: the databases at the start: TextureCacheDB") and emptied again before the warm loads ("react_warm: the databases at the start:", and none named).
It was added after the two sessions below; one round with it gave, at x1 warm, all decoded at 206 (React) and 128 (the port), no later than the sessions' medians.
Times are ms after navigation, set by one MutationObserver installed in every page before its scripts: React's `.bg-wool`, card backs and wood surfaces getting a blob background, the port's `--tex-*` set on its root, and the first hand card in the DOM ("board"); "all decoded" is the three pictures on the table decoded.
Two sessions on 2026-10-28, 11 rounds each, arms rotating within a round; the fusion run had ended (`pgrep -x cnitro_eval` empty before and after every run), and the load stayed under the 8 cores:
session 1 x1 6.08 before (top: secd, our Chrome, ctkd, WindowServer), 2.98 after; x4 3.05 before, 2.11 after (our Chrome's GPU helper at 57%); session 2 x1 2.41 and 2.20, x4 2.50 and 2.21.
The table is session 2; session 1 agreed within 7% on every median below except the harness's cold ones (up to 13%: x1 2,146 against 1,893).

All three decoded, median (25th-75th):
| | x1 cold | x1 warm | x4 cold | x4 warm |
| --- | ---: | ---: | ---: | ---: |
| port | 729 (697-731) | 144 (125-154) | 1,723 (1,694-1,735) | 247 (240-252) |
| real React app | 2,101 (1,962-2,286) | 248 (227-251) | 6,870 (6,589-8,465) | 477 (448-486) |
| harness page | 1,893 (1,826-2,206) | 135 (125-136) | 7,788 (6,516-8,092) | 137 (136-138) |
| port / React | 0.35 | 0.58 | 0.25 | 0.52 |
| harness / React | 0.90 | 0.54 | 1.13 | 0.29 |
| session 1: port / React / harness | 705 / 2,171 / 2,146 | 141 / 237 / 136 | 1,713 / 6,782 / 7,223 | 245 / 449 / 137 |

Each texture set, x4:
| | fcp | board | fern | wood | wool |
| --- | ---: | ---: | ---: | ---: | ---: |
| port, warm | 116 (113-120) | 178 (177-183) | 195 (194-200) | 197 (196-203) | 203 (202-209) |
| React, warm | 292 (292-296) | 399 (394-401) | 398 (394-400) | 399 (394-400) | 210 (207-211) |
| harness, warm | | | 39 (39-42) | 40 (40-42) | 41 (40-42) |
| port, cold | 116 (112-118) | 183 (180-184) | 284 (283-288) | 1,688 (1,685-1,705) | 1,035 (1,033-1,043) |
| React, cold | 1,284 (1,272-1,632) | 1,265 (1,253-1,613) | 6,828 (6,545-8,424) | 6,351 (6,147-7,980) | 1,984 (1,925-2,454) |
| harness, cold | | | 7,693 (6,421-8,000) | 7,300 (5,986-7,866) | 1,853 (1,690-1,909) |
At x1: port warm fern, wood, wool 96, 96, 98 and board 90, React 132, 131, 87 and board 131; cold, React's board 358 and first paint 372 against the port's 92 and 68.
The port's fourth picture, the end screen's wood, is set at 206 (204-211) warm and 2,480 (2,474-2,487) cold at x4 (99 and 962 at x1); it is not on the table and not in "all decoded".
What each page fetched (served uncompressed and uncached, as `tools/cdp.mjs` serves everything): the port 191 KiB in 14 requests, the React table 1,015 KiB in 28.

Which bar is right: the real app's.
The harness page is the stricter one, by 3.5 times at x4 warm (137 against 477 ms) and 1.8 times at x1 (135 against 248), because it reads its store at navigation with no app to start; the real app's warm reads wait for its bundle, its kernel, the session and the game's fetch.
Against the real app the port's warm path is not behind at x4: it has all three decoded at 0.52 of React's time (247 against 477), and at x1 at 0.58.
Texture by texture the wool is level (203 against 210 at x4, 98 against 87 at x1, where React's is ahead by 11 ms), and the fern and the wood land at half React's time, because React mounts them with its board (both are set within 1 ms of its first hand card), while the port sets them right after its own.
Cold, the port is 3 to 4 times ahead (0.25 at x4), and React's own texture making also holds up its board: its first hand card is in the DOM at 1,265 ms cold against 399 warm at x4.
Against the harness page the port is still 1.8 times slower warm at x4 (247 against 137); that gap is the port's start-up before it asks its store (Numbers, below), which the real app has too, and more of.

What makes the comparison unfair, and which way:
- CPU throttling slows a page's main thread, not its workers: React's wool weaves on a worker, so its cold wool is 2 times slower at x4, not 4 (1,984 against 1,005), while the port's wool is made on the page's thread; this flatters React's cold wool.
- The work differs cold: React makes its pictures whole (3840x2160, 1920x1080, 1000x1400), the port only what shows; this is the port's design, not the rig.
- React's table needs its server: the session's restore and the game's fetch go to a local c_base over loopback; the port's game is in its own store.
  In production the server is farther and React's warm path would be slower still.
- Both are served with no-store and no compression; production caches and compresses React's 1 MiB of scripts, which would shorten its start (and its warm path) on a repeat visit.
- The port's page is served cross-origin isolated (COOP and COEP, as `tools/textime.mjs` serves it), the React app is not.
- The bot moves on React's table during a load when it is its turn (the server's pace); the port's table is the port's own game, with its bot's pace.

## Network

cdom's network imports were made fit for live play in cdom (port-fixes 9501d6f, PERF_LOG.md "Port fixes"), each with a unit case and a mutant there:
- `cd_http(token, method, mlen, url, ulen, headers, hlen, body, blen, deadline_ms)`: request headers cross as one block of `Name: value\n` lines in C memory (one string, built in C with a memcpy or two; name/value pairs would cross as two pointers each), so `Authorization: Bearer <jwt>` and `Content-Type: application/octet-stream` are sent.
  The answer's status is the response's; `CD_HTTP_FAILED` (0) when it never arrived; `CD_HTTP_TIMEOUT` (1) when `deadline_ms` passed first and the request was aborted (an AbortController).
  Response headers are not exposed: the web client reads only the status and the body, and the headers would need a format of their own across.
- `cd_ws_buffered(id)`: the socket's bufferedAmount.
- A closed socket's id is free before C hears the close, and the next `cd_ws_open` takes it: the shim's table holds the sockets open at once.
- `cd_ws_send` sends a view of C memory (send copies; the `.slice()` is gone; the echo test writes over the message as the send returns).
- `cd_store_del(token, key, klen)`, answered by `cd_on_store_del(token, ok)` as a put is.
  The store also read a put's or a get's key only after the database opened, so a key in a buffer C had reused was read late; it is read during the call now.
- `cd_epoch_ms()` (Date.now()): netclient.h takes epoch ms (a token's expiry is one), and `cd_now()` is the page's own clock.
The shim: 6,623 bytes minified and 2,658 brotli before, 7,221 and 2,861 after (+598, +203).

`tests/live_test.c` is the web host written over those imports: `nc_do(intent)`, then every `nc_next_request` sent as fetch with only the client's `Authorization` value and, on a POST, `Content-Type: application/octet-stream`, its status and body handed to `nc_on_response` (a TIMEOUT handed on as 0, "never arrived"; a 15 s deadline on every request); the socket opened at the client's `nc_req_stream()` path, `nc_stream_opened` on open, every `nc_stream_next` frame sent, every message to `nc_stream_in`; the pump again on `cd_timer` after `nc_due_ms`, with a generation so an older timer does nothing.
`tests/live_test.mjs` builds nothing: it starts `c_base/build/cbase serve` on port 0 with throwaway secrets and a throwaway SQLite file (`CBASE_DEV=1`, `CBASE_ALLOWED_ORIGINS='*'`; never production), takes a port nobody listens on, serves the module on 8720 (cross-origin to c_base, as the web app with its own backend URL is), and reads the module's verdict.
A run (c_base on 127.0.0.1:60608, load 4.6):
```
http POST /v1/auth/signup -> 200
ok   signup: signed in as CDOMLIVE354CA758, access token 172 bytes, why 0
http GET /v1/games -> 200
ok   games: a list of 0 rows, why 0
ok   stream: the first frame sent is HELLO (60), READY (61) back
http POST /v1/create -> 200
ok   create: game 6001cf, envelope 129 bytes, why 0
ok   a wrong Bearer: status 401
ok   a 1 ms deadline: aborted, status 1
ok   a port nobody listens on: never arrived, status 0
http GET /v1/games/6001cf -> 200
ok   sockets: 20 opened and closed, ids 1 to 1
ok   bufferedAmount: 176 right after the HELLO, 0 once drained
stream frames seen: 61 65 51 65
requests: POST /v1/auth/signup GET /v1/games POST /v1/create GET /v1/games/6001cf
done
```
The frames: READY, the REPLY to the dashboard's subscription, the creator's view pushed (NW_WS_VIEW 51), the REPLY to the new game's own subscription (a create subscribes it), whose catch-up is the client's own `GET /v1/games/<gid>`.
The wrong Bearer and the deadline are raw `cd_http` calls (through the client a 401 is refreshed and sent again); the 20 socket cycles open and close a second socket while the stream holds id 0.
Four runs in a row, all passing (the 1 ms deadline always aborted first: the CORS preflight alone takes longer).
The live mutants (tools/mutate.py, `live: ...`): request headers not sent (the client signs up, then `/v1/games` is 401, the refresh is 200 and the retry 401 again: "FAIL games ... why 1"), the status not passed on ("a wrong Bearer: status 200"), no deadline in the shim and the deadline dropped in C ("a 1 ms deadline: aborted, status 200"), an abort reported as a failure ("status 0"), socket ids never reused ("ids 1 to 20"), bufferedAmount always 0 ("0 right after the HELLO").

What a live table still needs (this pass does not wire the table to play):
- Identity: a sign-up and sign-in screen (`NC_DO_SIGN_UP`, `NC_DO_SIGN_IN`; the name upper-cased by the host, ASK_WHY_ACCOUNT codes shown as backend.ts `BackendError.describe` words them), sign-out (`NC_DO_SIGN_OUT`, then `nc_reset`), and the session kept across loads: `nc_session_save` into cdom's store after every `NC_EV_SESSION`, `nc_session_load` at start, `cd_store_del` on sign-out (the web keeps it in localStorage and follows other tabs; the port would need a storage event too, or accept one tab).
- A lobby: `NC_DO_GAMES` and `NC_DO_CREATE`, joining by link, `NC_DO_OPEN_GAME` (flag 1 when no board is held), the bots list (`NC_DO_BOTS`) and the leaderboard if wanted.
- The table's board from the server instead of the local `Game`: an answered or pushed envelope adopted with `client_adopt_envelope` (client_table.h), the seats named through `client_identity_begin` and `client_identity_seat`; the stream held with `nc_stream_sub(NW_SUB_GAME, gid)` while the table shows, and each `NC_EV_VIEW` adopted whole, each `NC_EV_CAST` animated through `client_push_open`, `client_push_next` and `client_push_final` (and `client_push_settle` for an optimistic move); `NC_EV_SEATED` says whether the user sits there.
- A move sent with `NC_DO_PLAY` (the kernel's action wire from `client_move_wire` and the version as the intent), applied optimistically and settled when the cast or the refusal comes (`NcDone.action_status`, `reject_code`); the pressed pills (Consistency, item 8) then show while a move is on its way.
- The bot driven by the server, not `bot_drive` here; the local `game_keep` blob only for offline play.
- Chat (`NC_DO_CHAT_HISTORY`, `NC_DO_SEND_CHAT`, `NC_EV_CHAT`), if the table shows it.
- The reconnect: on `cd_on_ws_close`, reopen after `nc_stream_closed(now)` ms (backend.ts `lost`), and close when `nc_stream_next` hands out an empty frame; a hidden page (`CD_EV_VISIBILITY`) may pause the pump.
- `cd_ws_buffered` read before a large send, if anything large is ever sent (the client's frames are under 4 KB).

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
The real app, with its server and bundle, is measured under React baseline (Textures).

Warm loads after the store is asked at start (the follow-up; `tools/textime.mjs` now waits for the store's answer for the game, as the board does, rather than the human's turn).
Measured under heavy load: load average 29 to 73 on 8 cores around the runs (another agent's `cnitro_eval` processes at 65 to 83% of a core each), above the core count, so every absolute here is an upper bound; the arms ran interleaved under the same load, and the ratios and the first paint within a session are the results.
One x1 run lost its Chrome session after three rounds (a tab gone under the load; one run of `tools/test.mjs` lost it the same way and the next three passed) and is not counted.
| | x1 port | x1 TS | x4 port | x4 TS |
| --- | ---: | ---: | ---: | ---: |
| warm: fern set | 40 (38-78) | 25 (23-38) | 215 (147-268) | 49 (38-61) |
| warm: wool set | 43 (41-83) | 30 (25-43) | 245 (205-286) | 50 (40-64) |
| warm: all decoded | 133 (122-148) | 139 (117-164) | 326 (299-367) | 171 (147-189) |
| cold: all decoded | 941 (898-969) | 1,953 (1,924-2,311) | 2,615 (2,610-2,716) | 8,697 (6,983-8,977) |
| first contentful paint: `?notex` / cold / warm | 81 / 79 / 74 | | 142 / 128 / 173 | |
At x1 a warm port load now has all four decoded with the TS page (0.96 of it; 1.11 before) and sets them before its own first paint (40 against 74), which does not move.
At x4 it is still 1.9 times the TS page: the store is asked at 53 ms (when the wasm has started) and answers about 150 ms later, behind the page's own start-up work at x4 (the game's answer, then the board's first frame); a get of 1.5 MB on an idle page takes 2 ms at x4, so it is the busy thread, not IndexedDB.
The TS page has no start-up of its own (it asks at navigation, with nothing else to do), so this is not a comparison with the app, whose reads wait for its bundle and its mount; asking for the textures before the game moved nothing (230-310 against 210-266), and was taken back.
At x4 the first paint went 142 / 128 / 173 (`?notex` / cold / warm) with spreads of 122-211, 117-146 and 147-218 under a load of 70, which overlap.
Measured again once the load fell to 2-5, paired `?notex` and warm loads alternating, 15 to 41 rounds: x4 119 (114-121) against 120 (118-120) and 118 (111-121) against 115 (111-119); x1 63 (59-70) against 70 (63-77) over 41 rounds.
The x1 warm loads paint 4 to 8 ms later than `?notex` ones (an A/A run, `?notex` against `?notex`, gives 2), and that does not come from the textures' timing: asking the store only after the board's first paint gave 65 against 69, holding a texture read before that paint until after it gave 59 against 68 and 65 against 67, and asking 400 ms after the board 60 against 68; so the store is asked at start and a kept texture is set as soon as it is read.


The string table (`tools/strings.mjs`, three games played out by clicks):
| | before | after |
| --- | ---: | ---: |
| after the first paint | 174 strings, 16 KB | 128 strings, 3.6 KB |
| after game 1 | 358, 65 KB | 178, 4.5 KB |
| after game 2 | 388, 74 KB | 178, 4.5 KB |
| after game 3 | 523, 125 KB | 178, 4.5 KB |
Every computed string (a style, a data-card, an index) now crosses as a one-off (`cd_tmp`) and is never interned; what is left is literals and labels, almost all seen by the end of the first game (in later runs the table ended games at 175 to 181 strings, as the games differ: an end title or a pill can first show in game 2 or 3).
Live node ids at each end screen: 58, 64, 60 (the board on screen differs; nothing accumulates).

Sizes:
- `table.wasm` 104,009 bytes raw, 42,119 brotli and 47,367 gzip after the animation pass (Animations, The numbers); 75,303 bytes raw, 31,092 brotli (q11), 34,723 gzip (-9) after the consistency pass (the blob's writer and reader, `state_canonical`, `bot_wait_ms`, `anim_finish_rows`, the end screen's rows, the save's notice); before it 71,391, 29,539 and 32,872, before the texture pass 71,725, 29,589 and 32,956, before the first port-fixes pass 54,992, 22,122 and 24,870.
  musl's 2/pi table went with jsmath.c; the crop, the rectangles, the store and the keys came in.
  For scale, the React app's kernel alone, web.wasm, is 67,311 raw and 31,219 gzip (c/Makefile), before any JavaScript, and its textures are in its JavaScript.
- The shim: cdom's own, unchanged, written to be read and inlined minified: 8,147 bytes minified, 3,228 brotli after the drag and keys pass (the events, the key's character and time, `cd_prevent_default`, `cd_unlisten_window`, `cd_hit`); 7,351 and 2,919 with `rect` (the animation pass); 7,221 and 2,861 after the network pass (6,623 and 2,658 before it; 6,553 and 2,615 in its golfed form); cdom's PERF_LOG.md, SHIM_LINES, says what each import is for.
  Before the first port-fixes pass the port carried 172 lines of its own patched shim.
- The C the table is written in: 3,343 lines (`table_app.c` 3,142, `anim_run.c` and `.h` 97, with the texture pipeline and the test build's window on the kernel, `game_keep.c` and `.h` 48, `ui.h` 36, `kernel_glue.c` 20), plus the generators (385), trig.c (141) and crtrig.c (139).

## Against the TSX it replaces

The TSX for the same pieces is 1,795 lines: GameBoard 126, CardFace 204, CardBack 56, DeckAndFlipped 80, DiscardPile 44, TableBattles 400, PlayerRing 319, ActionButtons 289, handShape 147, RoleMark 130.
That is not a like-for-like count: the TSX also carries the flights, the row glide, the turning role coin, drag and drop, the tutorial's hints and chat bubbles, and it leans on contexts, `sdk/ts/table/client_table.ts` and generated readers for every kernel struct it touches.
The C needs none of that bridge: a `TableView` is a struct, and `client_play`, `hand_layout` and `bot_drive` are plain calls with no marshalling and no generated layout code.
For what both do, the C is about as long as the TSX, most of it the same inline styles as string literals.

## The tests

`tools/test.mjs` (headless Chrome, real mouse, touch and key events; the test build, its bot held or fast where a check says so):
- the first paint is the golden's DOM, but for the listed differences (and `data-turn="bot"`: the bot may act on that board);
- the seeded deal reads back: the hand, the battles, a badge of 21 over 20 stock layers, the marks, no pills;
- a resize re-lays the hand at the new width through `hand_layout` (the resize arrives through cdom's event path, `cd_listen_window`);
- a click (a tap: the press and the release) selects a card and the kernel's pills follow (7h offers nothing, 6s offers Attack), and a second click deselects;
- Attack plays 6s and spends the selection, the bot answers with a legal move, the surviving hand cards, battles and seats are the same DOM nodes, and Anna's count stays her mini hand's last child;
- the textures land after the first paint: with the store emptied, none is set when the table first shows, then all four and the black back are, a card back, a button and the wool show a `url(blob:...)`, all were made on this load, and the wool's image is the app's 3840x2160;
- the next load in the same profile sets all four from the store and makes none (no generator ran);
- the bot waits as the kernel says: 3000 ms with a human in the game (`app_bot_wait`), and a throw-in of mine a second after the load puts its step 3000 ms after the throw-in (between 2,900 and 4,500 ms), not after the load;
- the consistency check (`tools/consist.mjs`, seed 1, 3 games, textures on): no disagreement at any click or bot step, judged once the animations have landed (the run's clock 4 times as fast, the kernel's plan untouched), and every card in the air hidden wherever it is drawn (164 flights checked mid-air);
- a reload lands in the same game: seed 3's line (my 6s, the bot's answers, my moves) to 6h, 6d and 6s attacking me, the DOM the kernel's, a card selected, a reload: the stored game adopted and the DOM the same but for the selection; then played to its end, a reload shows the end screen, Play again, and a reload shows the new deal;
- a save that fails is not silent: with IndexedDB refusing the game's key, a move shows the notice; the store taking it again, the retry keeps the game, the notice goes, and a reload adopts it;
- a game played out by clicks (`tools/play.mjs`, textures on) reaches the end screen ("done - Two of us"), and Play again deals six new cards;
- after every click of that game and of more games, until every pill has shown, every wood surface on the page (the back button, each pill, the results panel, Play again) lies inside the rectangles its picture was worked out in;
- the animations (Animations), through an in-page recorder of every flight node (its card, its start, when it was armed and with what, when it went), every role ghost and every mark change: a played card leaves from its hand card's centre and lands on the centre of the cell the row made for it, at scale 1.5 then 1.8, armed about 25 ms after it is drawn and gone at its 500 ms landing, hidden at both ends meanwhile, the two cells already down gliding, my count unchanged until the landing; the bot's cover leaves its seat and lands laid across 6d, which turns with it (`transform 500ms ... 25ms`); a take flies the five table cards from where they lay, at their tilts, to the bot's seat; the refill flies from the stock onto the slots my hand made room for; a sweep flies four cards to the pile (3 px apart), the pile's count showing at its landing while the refills still fly, the bot's draw one stack of three backs, my check frozen through the run and the closing beat throwing the shield and the opener's sword as ghosts; a transfer's shield takes off with the card and lands before it, my badge making way and Anna's blanking then turning a sword in; a deal is 12 flights at 350 ms (the kernel's plan: 525 + 375 k), mine face up and the bot's backs, then the trump into its slot; a move made while a run plays joins it (one run, its step at 525 ms); a reload mid-flight lands on the committed board with no overlay; and in the played-out game the trump's first flight leaves its own slot.

`tests/blob_test.c` (native, ASan and UBSan): 300 seeded games with seat 1 the bot and seat 0 the same brain driven alone, the game canonical after every operation as the table keeps it; at every step the blob is written and adopted by `game_keep_load` into a zeroed Game, and the board (`state_put` unmasked, byte for byte), both seats' TableView and ViewRules, and the next drive from the same seeds all agree.
`tests/net_test.mjs`: a wasm module sends 8 binary bytes over a WebSocket and gets them echoed, then POSTs them and gets them back reversed (cdom's network, with no cd_flush anywhere).
`tests/live_test.mjs`: the kernel's network client over cdom's imports against a local c_base (Network).
`tools/strings.mjs`: from game 2 to game 3 the string table may grow only by what a game can show for the first time late (an end title, a pill, a medal: at most 10 strings and 200 bytes); it had been "may not grow", which failed whenever game 3 was the first to show such a literal (seen once in this pass, 180 to 181).
`tools/texparity.mjs`: the PRNG exact; the wood's cos exactly Chrome's and trig.c's sin, cos and tan within 1 ulp; the wool (whole and the page's crop), the wood (floats, the WebGL picture, and six rectangles with the base colour around them) and the fern's points exact; the multisampled fern within 3% of the WebGL picture; and, measured but not judged, the page-size fern against the app's picture shrunk.

- drag and drop by mouse and by touch, the keyboard by letters, numbers and arrows, and a game played out by mouse drags, by touch drags and by the arrow keys alone (Gestures: evidence and numbers).

Mutation check (`tools/mutate.py`), each of 92 mutants caught by the check written for it (a mutant of this pass runs the check it names, `ONLY`; the source's copy is kept outside the checkout):
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
- the wool placed 16 px off in that image: `tools/shots.mjs` "FAIL over the bar" (755,744 pixels with textures);
- the WebSocket bytes flipped in the shim: "ws failed"; the HTTP body not sent: "http wrong".
- the seven live mutants (Network): each fails its own line of `tests/live_test.mjs`.
- the game not canonical between operations: `out/blob_test` ("seed 1 step 11 after the next drive: the board differs");
- a drop that is no move spends the selection: the consistency check ("selection held x152 ... port (none) React 1-6");
- the bot not scheduled at the start: the golden check (`data-turn="human"`) and the pace check;
- the bot's wait 650 ms from the clock: "the kernel's wait with a human in the game" (650 against 3000); the timer stepping without asking again: "the bot's step 3000 ms after my move, not after the load: 1876 ms";
- the end rows reversed, and no "(You)": the consistency check ("end rows");
- the game not kept after a bot step, and the kept game ignored: the reload check ("the DOM after the reload is the DOM before it", "the stored game adopted");
- a failed put not answered, and not tried again: the save check (no notice; the notice never goes).
- the animation pass, the capture (`out/run_test`): the snapshot hook not installed ("the deal is 0 DEAL steps"), the stream written for nobody ("the stream's final board is not the board the table adopts (my hand)"), the closing transition dropped ("the last step's board draws otherwise ... (status)");
- the animation pass, the table: the deal flown at 500 ms ("each dealt card flies 350 ms"), a landed step not committed ("the discard pile shows its four cards while the run still plays"), the bot paced from the click ("3,012 ms"), no veil (the played card's mid-flight check and consist.mjs's "a flying card shows where it is drawn"), flights never armed ("armed: given its landing"), no row glide ("the two cells already down gliding"), the attack not turning under a cover ("the attack under the cover turns"), no room made in my hand ("my hand made room"), a move during a run starting another run ("one run"), no coin ("the marks turned as coins"), the marks not frozen in a run and the shield waiting for the landing ("it takes off with the card": 504 and 514 ms after it), the good not leading the stream ("mid-run: my check"), the trump drawn out of the stock ("its first flight leaves from its own slot"), no ghost ("one mark flies: the shield"), the seat landed on not making way ("the make-way"), the seat left keeping its mark ("Anna's badge blanks");
- the drag and keyboard pass (31, each run with its check, `ONLY`, at load 2.1 to 3.8 on 8 cores): no 10 px slop ("a tap selects"), a long press a tap ("a long press selects nothing"), the tap timed by the handler's clock ("a tap released behind a busy page selects"), no card's height above the hand ("a drop in the hand's area plays nothing"), the hand never its area ("the dragged card is dimmed"), a mousedown left to start the native drag (the attack check), a touch end letting the mouse events through ("a tap selects", touch), a touch move left to scroll (the touch attack check), the badge naming Attack for a cover, no drop cells, no amber ("the cover's cell in amber"), the dragged card looking as any other ("dimmed"), no rearrange ("the six went to the end"), the order not kept in the game ("the game holds the new order"), a drop that is a move keeping the selection ("spends the selection"), the dragged card alone though selected ("the shadow carries both selected cards"), the hand drawn in the game's order (the consistency check), the keys with a modifier held ("Meta+1 is the browser's"), a key typed into a field ("keys typed into a field do nothing"), the numbers one off, A without the Attack pill ("A without the Attack pill leaves the selection"), the cursor not wrapping ("Left from the first card wraps to the last"), the underline off its card (-2.5 px), Meta+arrow not kept ("kept in the game"), the aim not cycling ("Right cycles to the next"), the arrow from the card's middle ({"from":[0,35]}), the pass place 40 px out (-6), Down doing nothing ("Down took"), the hidden state staying "none", the drawn order not kept after a move ("a reload draws the hand as it was drawn"); and the pass-1 mutant of a hand rebuilt on every render, moved onto the hand as now drawn.
Not in the list, being equivalent here: Pickup and Good spending the selection (a selection never offers either), a pressed pill shown (no press outlives its move locally), and a battle's cover patched one way only (no path in play uncovers a battle under the same attack card since the board waits for the store).
Run at load averages 45.9 to 89.7 on 8 cores (the bot league), every mutant caught; the pace check's bounds (2,900 to 4,500 ms) held.
All 62 again after the animation pass, in one run (load 2.4 to 10.3 on 8 cores, a 6-process eval run joining it midway): every one caught, the pace check's new bounds (3,450 to 4,500 ms) held.
After the drag and keyboard pass its 31 ran (all caught), and the five the consistency check catches ran again on its new mix (all caught; the hand drawn in the game's order again after the order was kept after every move); the other 56 were not run again, and every one's text still matches the source once.

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
16c. **No element could be measured** (animation pass).
    Closed: `cd_rect(id, out)`, a node's box on screen (getBoundingClientRect, transforms included) as four doubles in C memory (port-fixes 5885289: unit case, mutants rect-order and rect-untransformed; 130 bytes minified, 58 brotli).
    A flight leaves from the centre of the card it replaces and lands on a battle cell the browser's flex row placed, and the row's glide reads each cell before and after a change; C cannot know those boxes without asking. It was the one cdom change the animations needed: the motion is CSS transitions set with `cd_set_style`, the timing `cd_raf_request`, `cd_timer_start` and `cd_now`.
16d. **No drag and no keys** (drag and keyboard pass).
    Closed (port-fixes 105ddc6 and 2ff8218, cdom's PERF_LOG.md SHIM_LINES): the mouse and touch event types (the per-node listen mask grew to 32 bits for them), a touch event's point from its changed touch, the key's character (`ch`) and a typed-into-a-field bit in the record, the event's own time (`ms`, event.timeStamp, which a tap is timed by), `cd_prevent_default` (no native drag after a mousedown on a `draggable` card, no scroll under a touch drag, no mouse events and click after a touchend, nothing typed after a handled key), document listeners that are not passive and can be removed again (`cd_unlisten_window`), and `cd_hit` (the listening node under a point, through what takes no pointer events).
    Seven unit cases and thirteen mutants there, all caught; the shim 7,352 -> 8,147 bytes minified, 2,921 -> 3,228 brotli.
    The drag's hit test is the one thing the browser has to answer; what it means (the target, the cards, the move) is the kernel's.
17. **`bot_drive` pulls in every brain through the roster.**
    The kernel's, not cdom's, and still open: `kernel_glue.c` answers the two roster calls for the handwritten brain alone.
    A link per app, by what it calls, is the kernel's doctrine; the roster's dispatch table defeats it.

### Open, and new findings

- **Measuring forces layout** (animation pass): `cd_rect` in a click handler makes the browser lay out there and then (the play's script time went from 0.6 to 2.2 ms, its total from 1.5 to 2.5); React pays the same in its layout effect. A batched read (several ids, one crossing) would cut the crossings, not the layout.
- **Per-frame motion that CSS cannot do** (the role ghost's arc) is driven from C every frame (4 `setProperty` a ghost a frame for 400 ms); a cdom import for the Web Animations API with keyframes from C would hand it to the compositor. Not needed for anything a transition can do.

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
- **A host keeping a board must keep it canonical** (consistency pass): the kernel says so in view.h `state_canonical`, but nothing stops a host that applies moves to a resident Game from skipping it, and the board then drifts from its own row (Consistency, item 1).
  A kernel entry that applies a move and canonicalises in one call would make the rule impossible to forget.
- **Whether a put was kept** (consistency pass): the saved game needs the answer the textures can ignore; cdom's store now answers every put (`cd_on_store_put`, vendored by the texture pass), and the table shows a notice and retries on a failure.
- **First paint and the store** (consistency pass): drawing the board before the store answered and the kept game over it cost first paint 66 ms at x1 (Reload); the board now waits for the answer, bounded by the store's deadline.
- **Open for live play**: closed by the network pass (Network): request headers, a deadline and its own answer, bufferedAmount, socket ids reused, a store delete (a failed put was answered already), ws_send without its copy, and the wall clock.
  Still not exposed: response headers (nothing in the contract reads them).
  What a live table needs beyond the imports is listed under Network.

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
