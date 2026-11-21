# Durak for the Game Boy

Two-player Durak (you against one bot) for the original Game Boy (DMG), in C with GBDK-2020.
Four bots on the dashboard: Miami plays a uniformly random legal move, drawn from the one seedable generator; New York is foolish's handwritten_prod bot, ported (below); Seoul is Monte Carlo over sampled worlds with the rollout engine (below); Moscow (the boss) is a cut-down cordite: Monte Carlo behind New York's move, and an exact endgame solver (below).

```
make                # build/durak.gb (GBDK= points at GBDK-2020 4.5.0)
make test           # the RNG ban and the differential test against foolish's engine
make mutants        # break each rule in a copy; the differential test must catch every one
make selfplay       # clang and SDCC must play identical games (the cross-compiler check)
make scripts        # every gbrun script against the ROM: screenshots in shots/, expects checked
make rom-mutants    # break each control in a copy of the ROM; some script must catch every one
make playout        # one random playout timed on the Game Boy
make simbench       # one rollout-engine playout timed on the Game Boy
make seoulcheck     # Seoul: the ROM must decide as the clang build does
make moscowcheck    # Moscow: the same
make solvecheck     # the endgame solver: the same, and its time on the Game Boy
make build/vscordite && build/vscordite cordite 2000   # Moscow against foolish's real cordite
make build/match && build/match seoul newyork 2000   # bots against each other
make romusage       # ROM and RAM per bank
```

On Linux the runner needs `apt-get install libmgba-dev libpng-dev` and `make MGBA_CFLAGS= MGBA_LIBS= ...` (the Homebrew paths are the default); `make scripts PYTHON=python3` needs Pillow for `tests/fwcheck.py`.

## Hypotheses (written before measuring) and measurements

- ROM: about 14 KB of the 32 KB ROM-only cart (code about 10 KB, as SDCC pulls in its 32-bit multiply and divide helpers; the binomial table 2.8 KB; tiles about 1.3 KB).
  No MBC needed.
  Falsifier: over 28 KB, which would force banking.
  - Measured with placeholder tiles: 26.4 KB, so the hypothesis was wrong by almost 2x (SDCC's SM83 code for the rules is about 2.5 times what I guessed), but it fitted 32 KB.
  - Measured with the real art: it does not fit 32 KB, so the cart is MBC1.
  - With New York the code no longer fit the 32 KB the CPU sees at once either, so the code is banked (`src/gd_bank.h`); with Moscow's solver the cart is MBC1 with 32 KB of RAM and a 256 KB ROM (table as measured with Moscow):

| Bank | Holds | Used |
| --- | --- | --- |
| 0 (fixed) | the rules core (`gd_rules.c`), the RNG, the input interrupt, the bots' shared scratch (`gd_botlib.c`), `_HOME` and all of GBDK's library | 11.1 KB |
| 1 | the UI: `main.c`, `gfx.c` | 13.3 KB |
| 2 | the table's tile banks, the fonts, the dashboards, the lose picture | 12.6 KB |
| 3 | the four win pictures | 13.2 KB |
| 4 | New York (`gd_bot_hw.c`) | 7.0 KB |
| 5 | the menu count and unrank (`gd_menu.c`, the random bot's, and the lite menu), and the gesture rules (`gd_play.c`, moved out of bank 1 for the card flights) | 11.2 KB |
| 6 | the rollout engine (`gd_sim.c` on the board rules of `gd_simboard.h`, with its 2.5 KB rank-set table) | 11.3 KB |
| 7 | Seoul and Moscow (`gd_bot_seoul.c`, `gd_bot_moscow.c`) | 6.9 KB |
| 8 | the endgame solver (`gd_solve.c`, on its own copy of the board rules and tables) | 12.2 KB |

  - The rule that makes it work: anything a banked brain calls must be in bank 0, so the core and the library are linked first and nothing else is unbanked (the link fails on any overflow); a new brain gets a bank of its own and one `GD_BANKED` entry point.
- RAM: about 4.6 KB of the 8 KB work RAM, most of it the cover-counting table (3.8 KB).
  Measured: 5.5 KB (3.9 KB + 1.6 KB).
  With Moscow: 7.1 KB (3.9 KB + 3.3 KB), 0.7 KB left above the stack; Seoul and Moscow share one world and one candidate list (`gd_botlib.h`), and Moscow ranks candidates by menu index, as a move is 38 bytes.
  With the sprites built in a staging copy (160 bytes, below): 541 bytes are left between the data and the stack's top at 0xE000.
  The stack's deepest reach, read from the bytes it left in that gap: 320 bytes over a whole game against Moscow, 400 in `moscowcheck` (100 decisions, 6 solved), so about 80 bytes to spare once the game's own frames are counted.
  Now measured by a canary (`src/gd_stack.c`, the section below): 665 bytes are free between the data and the stack (the character table went to ROM), 346 untouched after a whole game against Moscow, and `game_moscow.txt`, `game_moscow_endgame.txt` and `moscowcheck` fail if fewer than 48 are.
  With the live screen during a think (below): 14 bytes more data, so 527 bytes are left; the deepest reach is unchanged at 400 in `moscowcheck`, so about 66 bytes to spare.
  The cart's 32 KB of RAM is the solver's: bank 0 its stack (49 states and frames, 640 moves, the hash's table at 0xBF00), banks 1 to 3 its transposition table (3 x 1024 entries of a 32-bit tag and a value).
- Bot decision time (count the menu, draw, unrank, apply): 20 to 60 scanlines (2 to 7 ms) for a typical position, under 3 frames (50 ms) at worst.
  Falsifier: a typical move over one frame (154 lines, 16.7 ms).
  - Measured over two whole scripted games: 251 and 261 scanlines on average (27 to 28 ms, 1.7 frames), worst 1186 scanlines (129 ms, 7.7 frames).
    So the hypothesis is falsified: typical is 5 times my guess.
  - Profiled (scanline clock around each phase, one game): counting the menu, unranking the move and applying it took about a third each; applying was the largest because the hand-order code scanned 36 x 36; that is a mask now.
  - It does not show: the bot waits half a second (30 frames) after the board last changed before it moves, by design.
- Table redraw: the slowest whole-board redraw in those games was 582 scanlines (3.8 frames); moving the cursor redraws only the foot rail and the sprites.
  Buttons are latched by an interrupt, so a slow redraw never loses a press.

### Win-screen fireworks from the art's export, and the stack guard (gb-fireworks-stack)

Predictions written before the change was measured, each marked right or wrong after (the fireworks prediction was written after the old table's first screenshot but before the new code ran).

Fireworks:
- The game drew bursts from a hand-copied table (`fw_x`, `fw_y` in `main.c`). Prediction: it matches the art in at most a few of the 16 frames (4 cities x 4 frames) and fails the art comparison in the burst boxes in the rest; with `ART_WIN_BURSTS_<CITY>` in its place every frame matches the art's own render pixel for pixel, in the boxes and on the whole screen.
  Falsifier: the old table already matching in most frames, or any pixel differing after the change.
  Right: the old table failed 16 of 16 frames (4 to 217 differing pixels per frame in the boxes), the export passes 16 of 16 with 0 differing pixels, in the boxes and over the whole 160 x 144 screen (New York's train deck rows, the one sprite the game does not draw, are skipped).
- Where the 8 px came from: not the drawing code. `gfx_spr` adds the OAM's (8, 16) once and the four quarters and the shell use the art's formula, so a burst centred on the same point lands on the same pixels (the comparison proves it). The table was a hand copy of an older art revision (`wins.BURSTS` as of the Moscow commit: New York (22, 32), (88, 34), (136, 36)), and the art moved its bursts afterwards: New York's right burst went from (136, 36) to (144, 36) (8 px right) and then (146, 36), the middle one went, and Miami's, Seoul's and Moscow's moved too. Nothing tied the two together; `art_tiles.h` is the only source now.
- Cost: RAM none (`fw_t` stays one byte; it lost `static` so `scripts/win_fireworks.txt` can set the loop frame with `poke _fw_t`).
- `scripts/win_fireworks.txt` sets the loop frame, saves each city's four frames to `build/fw/`, and `tests/fwcheck.py` (run by `tests/scripts.sh`; `PYTHON=` picks the interpreter, it needs Pillow) renders the same frames with the art's own code and compares. Mutation: the burst centre x shifted 8 px left in the game fails it on all 16 frames (first differing pixel reported).

Stack guard (`src/gd_stack.c`: a canary painted over the gap between the end of the data, the linker's `s__HEAP_END`, and the stack pointer at boot; read back at the end screen as `gd_stack_free`, the untouched run from the bottom up):
- The gap at boot is `0xE000 - s__HEAP_END`, 539 bytes before the guard and 537 with its 2-byte result. Right: `s__HEAP_END` was 0xDDE5, then 0xDDE7.
- Untouched bytes after a whole game against Moscow (`game_moscow.txt`): predicted 130 to 200. Wrong, a little high: 215 (the stack's reach was 322 of 537, the same as the 320 read by hand; I had guessed the interrupt handler's pushes would add more).
- After `moscowcheck` (100 decisions, 6 solved): predicted 100 to 170 in the game's gap. Right: the stack reaches 400 bytes there, so 537 - 400 = 137 in the game's gap (that ROM's own data ends lower, so the check compares against the game's gap: `tests/moscowcheck.sh` asks for 48 plus the difference of the two `s__HEAP_END`s). The same reach with no solves at all (`MOSCOW_SOLVE_CARDS=0`) is also 400: the deepest path is Moscow's sampling and playouts, not the solver, whose stack is in the cart's RAM.
- In the game with the solver running (`game_moscow_endgame.txt`, a game poked into a 4 v 4 endgame, 3 solves): reach 345 of 665 (after the cheap win below), 320 untouched.
- Measured minimum headroom: 137 bytes in the game's gap before the cheap win (the moscowcheck reach; that ROM's call chain is shorter than the game's by a few frames I could not separate, so the game's real worst case is a little under that), 265 after it. The game's own runs left 215 (Moscow, no solve), 193 (endgame, 3 solves) before, 346 and 320 after.
- Cost: predicted 2 bytes of RAM (right), under 90 bytes of bank 0 ROM (right: 57 bytes of code), painting under 0.1 frame at boot and the read-back under 0.15 frame once per game (both wrong: measured with the scanline clock, 70 scanlines (0.45 frame) to paint and 75 (0.49 frame) to read back, about 36 and 84 T-cycles a byte; one half frame at boot and at the end screen, nothing in play).
- The check fails when fewer than 48 bytes are untouched. Mutation checked: a 330-byte local (written) in `gd_moscow_choose` fails `game_moscow.txt` (13 untouched, 346 before) and `moscowcheck` (1791 untouched, 1905 wanted); the threshold raised to 400 fails `game_moscow.txt` (346 untouched); `game_moscow_endgame.txt` with its poke removed fails on `_moscow_solves` (0, not at least 1).
- The cheap win: `gfx.c`'s `char_off[128]`, a character-to-tile table built at boot, is a function now (`icon_off`, arithmetic for a space, A..Z and 0..9, a search of `ART_FONT_CHARS` for the rest; `tests/fontorder.c`, in `make test`, fails the build if the font stops being in that order). Prediction: RAM down by exactly 128 bytes, ROM up by under 120 bytes, every screenshot of every script pixel-identical, the slowest redraw under 0.25 frame longer. Right on RAM (`s__HEAP_END` 0xDDE7 to 0xDD67), ROM (wrong direction: bank 1 shrank by 83 bytes, the table builder is gone), the screenshots (all 55 identical; `take.txt`'s second shot moved one frame later, because the 8 scanlines it added to the slowest redraw (662 to 670, 0.05 frame) pushed a row-by-row flush across a frame boundary, and the old shot caught the tear) and the redraw (right, +0.05 frame).
  I chose it over overlaying `bot_board`/`bot_slowest` (216 bytes of profiling copies of the board in `main.c`) because those two are live across a decision, when the bots' scratch is in use, so they cannot share it; and over a table in ROM because a 128-byte table would duplicate `ART_FONT_CHARS`.

## New York: foolish's handwritten_prod

`src/gd_bot_hw.c` ports `foolish/c/src/handwritten_prod_strategy.c` to the Game Boy with integers in place of its doubles, branch for branch:
- continue attacking with the most non-trump cards (least summed card score); with trumps only, attack with probability p (0.02 while the trump card is under the deck, else 0.65 + 0.35 x discard / cards, held to [0.5, 0.95]), else say done;
- transfer with one card, a non-trump first;
- cover only when every attack can be covered, with the cover of least product of card scores;
- otherwise done (one draw, as foolish makes it), the forced attack, or pick up.

Where foolish breaks ties by its menu's enumeration order, the port reproduces that order analytically for a hand in card id order (the difftest sorts foolish's hands the same way).
The doubles are replaced exactly:
- the trump gate is an integer threshold on a 16-bit draw (ceil of 65536 p, from p = (13t + 7d) / 20t), checked against `r / 65536.0 < p` for every draw, discard and card count (92 million cases, all equal);
- foolish's cover choice multiplies card scores in doubles, which round past 2^53; the port keeps the product exact in 32 bits while it fits and switches to a soft double (53-bit mantissa, round half to even after every multiply) past that, checked against real doubles (1.9 million products and 200000 comparisons, all equal);
- the cover search walks foolish's order with a bound (a first cover found by augmenting paths, and an exact lower bound: the product so far times each remaining attack's cheapest cover; rounding never decreases), so it returns foolish's first minimum while visiting at most 172 nodes (4.7 on average).

foolish's three random draws (the trump gate, the pick among "other" moves, the final fallback) take this engine's draws: the difftest compiles foolish's bot with `random_strategy_random` redirected to a queue of the port's own 16-bit draws, `u = r / 65536`, and checks both make the same number of draws.
Two pieces of foolish's code are unreachable in this game and are not ported, with the reason in the code: the early-deck "first non-trump attack, no tie-break" fallback (that point is only reached when no non-trump attack exists) and the final random fallback (a seat with a menu always returns earlier).

Result: 16811546 decisions compared over 90000 games (New York playing one seat in 30000 of them, and asked for both seats' moves on every board of all of them), 16811546 agree: exact agreement, not a rate.
Its 9 mutants are caught (`tests/mutants.txt` 17 to 25), and `make selfplay` folds its choice for both seats on every board, so SDCC computes what clang does.

Time per decision on the Game Boy, 1044 decisions over 40 scripted games (`scripts/game_new_york.txt`): 262 scanlines on average (28.5 ms, the same as Miami's, most of it applying the move), 1867 at worst (203 ms, a three-attack all-trump cover).
Before measuring I had not written a hypothesis for New York; the first measurement was 1.28 s at worst, which was the cover search walking every cover with no bound (fixed by the bounds) and then a 32-bit division per multiply (fixed by a table of limits).

## One random playout (sizing Seoul and Moscow)

`make playout` times what a Monte Carlo bot repeats: a fresh deal, then both seats playing uniformly random moves off the LITE menu (foolish's `calculate_legal_moves_lite`, its rollouts' menu: the covers are one greedy full cover) to the end, with no repetition rule, as foolish's rollout games carry none.
The lite menu is in `gd_menu.c` and the difftest compares it with foolish's as sets on every board (691928 boards in 1000 games each way, all equal; its mutant is caught).
The same `tests/playout.c` runs built by clang and inside a test ROM, and the ROM must play the same moves (the move total is compared).

| 64 playouts, seed 0x2468 | |
| --- | --- |
| moves a playout | 101.5 on average, 210 at most, none cut at 1000 |
| one playout on the Game Boy | 19544 scanlines on average (2.12 s), 37642 at worst (4.09 s) |
| one move | 20.9 ms |
| a move's time, by phase (a second, marked pass) | the two lite menu counts 41%, the draw 11%, the unrank 29%, the apply 19% |

I wrote no hypothesis before the first playout measurement (a slip against the rule).
Before the phase split I predicted the counts at about 60% and the unrank at about 10%: the unrank is much dearer than I thought, and the draw (11%) is not free either.
So a bot that runs even 10 playouts a decision would think for 20 seconds on this engine as it stands; Seoul and Moscow need a rollout engine of their own (a bitboard simulation without the menu, as foolish's `cordite_sim.c` is for cordite), sized from this number.

## The rollout engine (gd_sim): hypothesis, written before any code

What it ports: foolish's two-player rollout, `cd_sim_playout` in `foolish/c/src/cordite_sim.c`, whose policy is the arena handwritten bot computed directly on bitboards (`sim_handwritten_move`; foolish's own comment explains espresso is never reached at two players).
It plays no menu: each ply asks the policy for its one move, the lowest eligible seat first, and applies it; the stock is drawn at a random index; there is no repetition rule.
Design: a 32-byte state (two hands, the stock, the uncovered attacks and every table card as 5-byte masks; the ranks on the table; a few counters and a flag byte), table-driven bit operations (no variable shifts), in a bank of its own.

Predictions for the Game Boy, against the lite-menu playout's 2.12 s (20.9 ms a move):
- one move (policy, apply and draws): about 1.2 ms;
- one playout: about 120 ms on average (about 100 moves), about 300 ms at worst (about 210 moves);
- phase split of a move: the policy about 50%, apply and refill about 35%, the deck draws about 15%.

Falsifiers: an average playout over 150 ms or a worst over 400 ms (the targets) means the masks or the policy are too slow as written; a policy share under 30% would mean the state handling, not the decisions, is the cost.

### Measured (marking the hypothesis)

`make simbench`: 64 fresh deals played out, the ROM checked against the clang build (the plies and a checksum of every final state; mutation checked by breaking only the ROM's assembly AND).

| | predicted | measured |
| --- | --- | --- |
| plies a playout | about 100 | 48.4 (78 at most): the rollout policy plays whole groups, so a game is about half as many plies as random play |
| one ply | about 1.2 ms | 3.33 ms: wrong by almost 3x |
| one playout, average | about 120 ms | 160.8 ms (target 150: missed by 7%) |
| one playout, worst | about 300 ms | 245.3 ms (target 400: met) |
| phase split | policy 50%, apply 35%, draws 15% | policy 48%, apply and refill 35%, draws 18% (a second, marked build) |

So the split was right and the cost per ply was not: the first version took 720 ms a playout, and the way down was measured step by step on the Game Boy, each step kept only when the sim test still agreed exactly:
- the bots' 16-bit draw without a 32-bit multiply and modulo (720 to 431 ms; the same values, from `(b - a) mod 65536`);
- the cards of the table's ranks as one lookup (a 512-row table), loops over set bits only, unrolled mask copies (431 to 225 ms);
- the two hottest mask operations in SM83 assembly, with the C kept for the Mac (225 to 202 ms);
- one fixed-address working state instead of a state passed by pointer, which SDCC reaches far faster (197 to 165 ms);
- straight to "good" when the attacker has nothing to add (165 to 161 ms).
Two tries made it slower and were undone: inlining the 5-byte count and test at every call site (231 to 259 ms), and finding the draw index by repeated addition instead of two small multiplies (161 to 179 ms).

### The proof

`tests/simtest.c` deals with both rule engines, plays a random stretch of uniformly random moves, then turns the board into both rollout states (foolish's stock array put in card order, which is no part of a determinized world) and steps them one ply at a time with `gd_sim_step` and foolish's `cd_sim_one_step`, foolish's `game_random` handed this engine's draws: the same seat must act, the two must draw the same numbers, and every field of the states must agree after every ply, to the end of the game; then whole playouts (`gd_sim_playout`, `cd_sim_playout`) from the same boards must finish the same way.
Result over 60000 seeds: 47570 boards, 1409978 plies stepped and compared, 95140 playouts compared, all equal (37 of them unfinished at 1000 plies in both, a cycle with no repetition rule).
The one structural difference: foolish covers the uncovered attacks in table order, this state keeps no order and covers them in card order; the cards used come out the same for these nested cover sets, and the test checks it on every cover.
9 sim mutants, all caught (`tests/mutants.txt`).

## Seoul: the robusta idea

`src/gd_bot_seoul.c`, after foolish's robusta (`foolish/c/src/robusta_strategy.c`): for each candidate move it samples worlds consistent with what it can see, plays the move on each, plays the game out with the rollout engine, and keeps the candidate that won most.
- What it can see: its hand, the table, the face-up trump, the discarded cards, and the cards the opponent is known to hold because it picked them up and has not played them since (the rules state keeps these two, `gone` and `known`, as foolish's BotMemory does; no rule reads them, and the difftest checks they stay true).
- A world: the cards it cannot place, shuffled (an exactly uniform draw, checked over every 16-bit value) and dealt to the opponent's unknown cards and the stock.
- The candidates, at most 6: New York's move first (the tie-break), then done, pick up, a one-card transfer with each card that can, and a one-card attack or throw-in with each card, cheapest first. Robusta weighs every move of the full menu, 12 worlds each; on the Game Boy that is far beyond the time, so this is the cut.
- The same worlds, and the same draws in their playouts, for every candidate (common random numbers), so candidates differ only by the move.
- The budget: 20 playouts a decision, shared by the candidates (two to eight worlds each); a playout past 400 plies counts as lost, as robusta counts an unfinished one.

Sizing, measured, for a budget of about 3 s a decision on the Game Boy:

| budget (playouts) | Seoul's wins against New York (2000 games) | playouts a decision | modeled Game Boy time a decision |
| --- | --- | --- | --- |
| 12 | 78.3% | 8.4 | 1.0 s |
| 16 | 77.8% | 10.6 | 1.3 s |
| 20 | 81.2% | 11.9 | 1.5 s |
| 24 | 77.2% | 13.4 | 1.6 s |

Strength plateaus at about 78 to 81% (2000 games is about 2 points either way): more worlds do not help, and neither did 4 or 8 candidates instead of 6 (78.5%, 78.9%), so the cut, not the sample count, is what limits it.
The model is the per-playout cost the profiling ROM measured (sampling 17.3 ms, apply and convert 6.8 ms, 3.30 ms a ply) times Seoul's own playouts and plies in the match.

On the Game Boy itself (`make build/durak_prof.gb`, three scripted games, budget 20): 2.66 s a decision on average and 4.1 s at worst, 17.4 playouts a decision; its time is 82% playouts, 12% sampling worlds, 5% applying and converting, 1% the candidates.
That is more than the model's 1.5 s because the scripted player keeps taking, so Seoul faces big hands and its budget fills.
The rail says SEOUL THINKS while it searches, with dots that step while the hand stays live ("The screen while a bot thinks", below).

Strength, final (budget 20, seeds 9001 on):
- Seoul against New York: 77.7% of 2000 games;
- Seoul against random: 98.5%;
- New York against random: 95.8%.

`make seoulcheck`: a banked ROM laid out like the game makes Seoul's 146 decisions over two random games and must match the clang build's (mutation checked: building only the ROM's Seoul with a different budget fails it).

## Moscow: a cut-down cordite

### Cordite at two players, and what Moscow keeps (written before any code)

Read from `foolish/c/src/cordite_strategy.c`, `cordite_sim.c` and `foolish/c/CORDITE.md` (read-only).
At two players cordite is these parts:

| Part | Cordite at 2 players | Moscow |
| --- | --- | --- |
| Belief | pinned cards (picked up, not played since), the discard, voids (a pickup facing exactly one uncovered attack says the defender held nothing to beat it; cleared when it draws), rank floors | pinned cards and the discard (`known`, `gone`, already in the rules state) and voids, kept: they are a 5-byte mask a seat. Floors dropped: cordite only sets one when more than two players are in (`m->in_now > 2`), so at two players they never fire |
| Worlds | the unseen cards shuffled to the opponent and the stock; voids applied in 3 worlds of 4 by swapping a forbidden card with a stock card | the same |
| Candidates | from the full menu: up to 12 attacks (most cards, then cheapest), 10 covers (cheapest product of card scores), 3 transfers, done, pick up | the same ranking, cut to at most 6 |
| Monte Carlo | common random numbers; 3 stages on 32, 56, 56 worlds: all candidates, then the best third (at least 3), then the best 2; ties drop the last candidate (the cheap-first fix) | the same stages cut to 2: all candidates on a few worlds, then the best 2 on more; ties drop the last |
| Root endgame solve | when the stock and the face-up trump are gone (both hands known): pass 1 hunts a win (a window from 0 up, one shared budget of 20000 nodes), pass 2 marks proven losses with a null window (15000 nodes) and drops them from Monte Carlo when some move is proven not to lose | kept whole, with node caps sized for the Game Boy |
| Endgame solver | exact alpha-beta over the full move set, values plus or minus (1000 - depth), a transposition table of 65536 exact entries (fail-soft results stored only when strictly inside the window), depth cap 48, abort on a node with more than 96 moves | kept: the same search on the 32-byte rollout state, the table in cartridge RAM |
| Exact leaves in rollouts | off by default (`CD_LEAF`): measured both 10 times slower and weaker | dropped, for cordite's own measured reason |
| Race early stop | off by default (`CD_RACE=0`) | dropped: cordite does not use it, and two stages leave little to stop early |
| Rollout policy | the handwritten policy on the bitboard engine | the rollout engine (`gd_sim`), already exact against it |

The solver's move set: foolish lists a cover once for each way of pairing cards to attacks; those pairings lead to the same position, so Moscow lists each (attacks covered, cards used) once.
That leaves every game value the same (a duplicate child cannot change a minimum or a maximum), and only removes repeated work.

The cartridge RAM: MBC1 with 32 KB of RAM (four 8 KB banks), no battery.
- Why RAM at all: the solver's stack (a 32-byte state and a move list per ply, 48 plies deep) and its table cannot fit the 1.3 KB of work RAM that is left.
- Why 32 KB and not 8 KB: the table should hold every exact value one capped solve stores, so that the solver's result does not depend on what the table evicted; with a cap of a few thousand nodes at 4 bytes an entry that is 16 KB, which 8 KB cannot hold beside the stack.
- Why no battery: the table is a cache rebuilt for every decision, so nothing in it is worth keeping, and a battery adds a save file and nothing else.

### Hypothesis (predictions before measuring)

- Solver speed on the Game Boy: about 4 ms a node (a 32-byte copy in and out, an apply, a hash, a probe, move generation), so 5 s buys about 1200 nodes.
  Falsifier: over 8 ms a node, which would leave too few nodes to resolve anything but the last few plies.
- The node caps: about 1500 for the win hunt and 1000 for loss avoidance (cordite's 20000 and 15000 scaled down), which at 4 ms is 10 s at worst for the solve.
- Resolution: with foolish's caps the root solve resolves about 85% of the positions where the stock is gone; with Moscow's about 50%, most of the misses in the first few plies after the stock runs out, when hands are biggest.
- The proof: on thousands of positions with the stock gone, whenever both solvers finish within their caps, the values are identical (including the distance to the end); any difference is a bug.
- Time a decision on the Game Boy: 3.5 s on average and under 13 s at worst (falsifier: over 5 s average or 15 s worst).
- Strength (2000 games each): Moscow beats Seoul about 62% (falsifier: under 55%, which is not clearly), New York about 85%, random about 99%; against foolish's real cordite (8 to 10 times the playouts, 15 times the solver budget) about 40%.

### Measured (marking the hypothesis)

The proof, `tests/solvetest.c` (`make test` runs `solvetest 60 1 20000`):
- It solves boards against foolish's `cd_sim_solve` at every ply once the stock is gone, in games whose moves are uniformly random or New York's (half each), and in random small endgames built field by field for both solvers.
- Each board is solved from each seat's side in the full window and in Moscow's window (0, 2000).
- 100 games at 200000 nodes: 1412 full-window values where both finished, all identical including the distance to the end; 3793 windowed verdicts the same.
- The final run (60 games and 1200 random endgames, 20000 nodes): 2020 identical values, 5221 identical windowed verdicts.
- The hypothesis held: no difference ever.
- The random endgames were added because games alone never make a transfer of several cards decisive: a mutant that allowed only one-card transfers survived 400 games, and is caught now.
- 10 solver mutants (`solve:` in `tests/mutants.txt`), all caught.

Speed on the Game Boy, `make solvecheck` (86 solves at a 1500-node cap, the scanline clock): falsified.
- First 11.3 ms a node. Timing the parts showed the hash at 3.4 ms (5 Pearson lanes in C) and move generation at 5.4 ms.
- The hash in assembly, its table copied to an aligned address in cart RAM so a lookup is one `ld l,a; ld c,(hl)`: 0.97 ms. A table of the cards that beat each card: generation 4.6 ms.
- Now 8.2 ms a node (84564 nodes in 6.41 million scanlines): twice my guess, just over the falsifier. SDCC's code for the move generation is most of what is left (about 45 cycles to list each card of a hand).

Resolution: falsified, worse than guessed. Within 200000 nodes the root win hunt finished:

| Cards in both hands | 4 | 6 | 8 | 10 | 12 | 14 | 16 | 18 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Solves finished | 99% | 96% | 82% | 62% | 32% | 15% | 12% | 4% |
| Nodes when finished (mean) | 284 | 2848 | 10284 | 19945 | 21111 | 10378 | 21245 | 50057 |

At about 400 nodes the Game Boy reliably finishes only endgames of 6 cards or fewer.

Strength, and why (2000 games each unless marked): falsified first, then found.
- The first cut (cordite's candidates, 4 and 8 worlds, both solver passes at 1500 and 1000 nodes) beat Seoul only 53.9%: under the falsifier.
- Ablations against Seoul: solver off 52.6%; solver at 200000 nodes 56.1%; 16 and 32 worlds 61.2%. So at two players the solver adds a few points at most and the worlds add the most, and on the Game Boy the worlds are what the time buys.
- Cordite's second pass (drop the moves proven to lose, when some move is proven not to) never fired in about 10,000 solves. At two players a draw needs both seats out at once, so a move proven not to lose is a proven win, which pass 1 already took. It is gone, and its node cap with it.
- New York's move as the first candidate (Seoul's anchor, and the tie-break): 60.0% against 53.9% at the same time. Cordite needs no anchor with 144 worlds a candidate; Moscow has 6 to 18.
- With the anchor: solver off 58.0%, the solver only for endgames of at most 8 cards 59.8%, for a fifth of the solver's time.
- The time bound: a budget of plies a decision, a solver node charged as 2.5 plies (8.2 ms against 3.3 ms), checked before each world. 2500 plies 59 to 60%, 1500 57.7%, 1200 56.4%. 1500 holds the hard scripted game below to 5 s a decision; at 1000 solver nodes a big solve then left no worlds, so the cap is 400 (59.0%).

The final Moscow: up to 6 candidates (New York's move, then cordite's ranking: 5 attacks, 3 covers, 2 transfers, done, pick up), stage 1 on 6 worlds, the best two on 12 more, voids in 3 worlds of 4, the win hunt for endgames of at most 8 cards with 400 nodes, 1500 plies a decision.

| Moscow against | Games | Moscow wins | Hypothesis |
| --- | --- | --- | --- |
| Seoul | 8000 (seeds 9001 and 30001) | 59.7% (60.1%, 59.4%) | 62%: lower, but clearly ahead (17 standard errors from even) |
| New York | 2000 | 84.4% | 85% |
| random | 2000 | 98.5% | 99% |
| foolish's cordite | 2000 | 42.8% | 40% |
| foolish's octogen | 400 | 23.2% | (not guessed) |

For scale, Seoul against cordite: 34.6% (2000 games); New York against cordite 9.0% (400 games), which checks the harness against foolish's published 88% for cordite over handwritten.
`make build/vscordite && build/vscordite cordite 2000 9001` runs both rule engines in lockstep and feeds cordite foolish's own game, logs and legal-move list (foolish's sources read, built into `build/` with foolish's defaults).

The void belief (`nocover` in the rules state) matched foolish's BotMemory after every move of those 2000 cordite games (159518 moves, 128824 seat-moves holding a void); 4 mutants (`void:`) caught.

Time a decision on the Game Boy:
- The scripted game (`scripts/game_moscow.txt`, the input loop that keeps taking, so Moscow faces big hands and long playouts): 26 decisions, 4.84 s on average, 5.99 s at worst; 964 playouts, 36 plies each.
- `make moscowcheck`, 100 decisions over games of random and New York moves: 2.3 s on average.
- The solver at its cap: 400 nodes, 3.3 s. The Mac's model over 8000 games against Seoul: 3.3 s on average, 8.4 s at most; against random, 9.1 s at most.
- The hypothesis (3.5 s, under 13 s) held; the budget (5 s, 15 s) holds.
- The rail says MOSCOW THINKS while it searches, with stepping dots (`shots/moscow_02_thinks.png`); `shots/moscow_03_end.png` is the finished game.

The cart's RAM in practice: the 400-node cap stores at most 400 values, which 8 KB would have held; the 32 KB keep the stack (2.8 KB of states and frames, 5 KB of moves) in a bank of its own and leave room for the 1500-node solves of `make solvecheck`.

The cross-compiler checks: `make moscowcheck` (100 decisions, 6 of them through the solver) and `make solvecheck` (86 solves) match the clang build.
Mutation checked: a ROM-only Moscow with 5 worlds in stage 1, or 900 solver nodes, fails `moscowcheck`.
`solvecheck` also caught a real bug no Mac test can see: the probe read the frame (in RAM bank 0) after switching to the table's bank.

## The screen while a bot thinks

Seoul and Moscow search for seconds in the game loop, and the screen used to hold still meanwhile, with presses queued until the move.
Now the input timer's interrupt (`src/input.c` `in_tick`, 60.2 a second, which already reads the buttons) calls `live_tick` (`src/live.c`, bank 0), and while a think runs that runs the game's handler (`src/main.c` `live_think`, bank 1) once a tick:
- the rail says MOSCOW THINKS (or SEOUL THINKS) with a wave of dots after it, a step every quarter second: "", ".", "..", "...", " ..", "  .", again (the font's own `.`, no new tiles). One tile changes a step, so no frame can show a step half written (a version that cleared three dots at once could, for the frame the row was sent in);
- LEFT, RIGHT, A, B, A held + LEFT / RIGHT and SELECT held + LEFT / RIGHT move the cursor, select, sweep-select and rearrange the hand, drawn as at any other time (the sprites through the staging copy, the hand by the usual board redraw);
- UP and DOWN wait for the bot's move, and a press of either during the think is dropped, so nothing plays itself after it; TARGET mode waits too (in TARGET mode every key is dropped); START is kept and takes effect after the move.

What makes it safe:
- The search is stopped at any instruction, in any ROM bank and any cart RAM bank. `live_tick` saves the ROM bank (`_current_bank`), maps the handler's bank, maps cart RAM bank 3, moves the stack there (the free top of the bank, 0xB400 to 0xBFFF, above the solver's transposition table: `gd_solve.c` fails the build if the table ever grows into it), lets interrupts in again (the VBlank must still copy the sprites and count frames; a tick that comes meanwhile sees the handler busy and only latches the buttons), runs the handler, then puts the stack, the RAM bank and the ROM bank back.
- Both banks are put back from what the search last asked for, not from a register (MBC1 registers read back nothing): GBDK's banked calls write `_current_bank` before the MBC register (read from GBDK 4.5's `___sdcc_bcall_ehl` in the built ROM, both ways: `ldh (cur),a; ld (0x2000),a` on the call and on the return), and the solver writes a shadow, `gd_ram_bank`, before its register, so a tick that lands between the two still restores the bank meant.
  Every bank switch in the ROM, checked: GBDK's banked-call trampoline (shadow first), `SWITCH_ROM` (`_current_bank = b, rROMB0 = b`, used only by `gfx.c` `from_bank`, which also runs with interrupts off and never while a bot thinks), the solver's `RAM_BANK` (below), and `live_tick` itself (interrupts off). Nothing else writes 0x2000 or 0x4000.
- The solver's switch is a call (`gd_solve.c` `ram_bank`, two adjacent instructions: the shadow, then the register), not a macro. As the macro `(gd_ram_bank = b, *(0x4000) = gd_ram_bank)` SDCC moved the shadow's store above the reads of the search frame that came before the switch (C lets non-volatile reads move across a volatile store), so a tick in that gap put the table's bank back while those frame reads were still to come from bank 0. `make livestress` found it (below): with the handler on every tick at the game's own 60 Hz, `solvecheck` gave a different sum, and at 256 Hz the ROM ran off the rails. A call is a fence SDCC does not move memory accesses across, either way, which also rules out table reads hoisted above a switch.
- On the search's stack (work RAM, about 80 bytes to spare) the handler costs only `in_tick`'s call to `live_tick`, 2 bytes; everything else runs on the cart RAM stack.
- `in_tick` calls `live_tick` only while a think runs, and then only when a key wants it (a press, a step, SELECT going down or up) or every third tick (the dots); outside a think it adds one flag test a tick, so play outside a think keeps its timing.
- The search reads the rules state `G` (through a `const` pointer) and its own memory. The handler only reads `G` and changes what is the screen's: the hand's order (`mem`, `disp`), the cursor, the selection, the map and the sprites. No bot reads any of them, and the hand order is presentation (the rules keep hands as sets).
- During a think the foot rail is the think rail, so the handler never asks the rules for the pills (`action_text`); the rest of the board redraw reads `G` only.
- SDCC's `CRITICAL` on this CPU is a plain `di` / `ei` (seen in its output), so inside an interrupt it lets interrupts in anyway; the handler is written for that from the start (the busy flag), rather than relying on running with interrupts off.

### Hypothesis (written before measuring)

- Moscow's think time, on the same decisions: about 3% longer with no key pressed (a tick of the handler is about 600 machine cycles of 17556 a frame: the bank and stack switch, taking the buttons, the dot counter, and a rail row flushed every 15 ticks).
  Falsifier: over 6% or under 1%.
  A press costs more while it is drawn (a cursor move redraws the sprites, about 30 scanlines; a selection redraws the board, up to about 580), so the scripted game, which presses a key about every 12 frames, about 4%.
- The handler's own stack: under 256 bytes of the 3 KB (the deepest call is the board redraw); falsifier over 1 KB.
- Work RAM: 12 bytes more data (8 in `live.c`, the RAM bank shadow, 3 for the dots and SELECT), 2 bytes more of the search's stack.
- ROM: about 100 bytes in bank 0, about 250 in bank 1.
- A LEFT press during a think shows on the screen within 2 frames (falsifier: more than 4).
- Moscow decides exactly as before: `moscowcheck` (now run with the handler ticking) matches clang.

### Measured (marking the hypothesis)

Think time, on the same decisions. `scripts/think_time.txt` plays Moscow with every press in the 20 frames after a move (Moscow waits 30 frames after the board last changed), never during a think, so the old ROM and the new one play the same game (25 decisions; the scripts expect the same playouts, plies and moves). `scripts/think_time_keys.txt` is the same game with keys pressed during each think that leave the hand as it was (RIGHT then LEFT, A then A: two cursor moves and two board redraws a think).

| 25 decisions, scanline clock | before | after, no key | after, keys mid-think |
| --- | --- | --- | --- |
| average a decision | 44275 lines (4.81 s) | 45622 (4.96 s), +3.0% | 46558 (5.06 s), +5.2% |
| worst | 54294 lines (5.90 s) | 55950 (6.08 s), +3.0% | 56926 (6.19 s), +4.8% |

- 3% with no key: right, but only for the second version. The first one cost 7.4%, past the falsifier, so it was taken apart by building variants and timing the same game: the bank and stack switch alone 0.7%, taking the keys 1.7%, and the dots 5.0% (they rebuilt the whole rail and compared the whole map every quarter second). Now `in_tick` runs the handler only when a key wants it or every third tick (0.5%), and the dots step on a `sys_time` deadline (read with interrupts off, as they are on in the handler) instead of a 16-bit division and send only the foot row (`gfx_flush_row`). A flush of only the changed span of the row measured the same, so it was not kept.
- About 4% with keys: 5.2%, so about right; it depends on how many keys are pressed (here four a think, two of them board redraws).
- `scripts/game_moscow.txt` presses UP and DOWN during thinks, which are now dropped instead of played after the move, so it is a different game than before: 34 decisions, 4.66 s on average, 7.40 s at worst (before: 26 decisions, 4.84 s and 6.00 s). Those are not comparable decision for decision; the table above is.
- The handler's stack: 166 bytes at most (cart RAM bank 3 filled with a pattern through gbrun, which writes the MBC register like the CPU does, then a Moscow game of about 300 rounds of keys pressed through the thinks, including selections and moves of cards). Under 256: right.
- Work RAM: 14 bytes, not 12. `live.c` 8, the RAM bank shadow 1, `in_tick`'s idle count 1, and 4 in `main.c` (the dots' step, SELECT as drawn, and a 16-bit deadline where I had planned an 8-bit counter). The search's stack: same game, 321 bytes before and 323 after (`think_time.txt`): the 2 bytes predicted. `moscowcheck`, which now ticks the handler all the while, still reaches 400 bytes, as before. (`game_moscow.txt` reaches 356 now, in a different game; it is the search's own frames, written during a think.)
- ROM: bank 0 +187 bytes, bank 1 +461 (bank 0: `live.c`, `in_give_back` and the tick's test; bank 1: the think screen in `main.c` and `gfx_flush_row`): wrong by about 2x both, as SDCC's code usually is against my guesses. Bank 1 has 2.3 KB left.
- A LEFT during a think shows within 2 frames: right, 1 or 2 frames (the input timer's 60.2 ticks a second drift against the 59.7 frames).
- `moscowcheck` matches clang with the handler ticking (about 5000 runs of a stand-in handler that works a 64-byte buffer on its stack during the 100 decisions); `seoulcheck`, `solvecheck` and `selfplay` match too.

The proof, `scripts/live_think.txt` (45 expects), during Moscow's first think: the rail's dots are checked pixel by pixel at each of the six steps, mid-step; a LEFT puts the cursor on the first card and its underline is on the screen two frames later, while `_bot_thinking` is still 1 and `_bot_moves` 0; a second LEFT wraps to the last card; A raises it; SELECT held + LEFT moves it a place left; B clears the selection; RIGHT twice wraps the cursor to the ten of spades, a trump that beats the jack of hearts Moscow is about to lead; UP is pressed. After the move: six cards, no cover, not in TARGET mode, the cursor where it was (`shots/live_01_dot.png` to `live_05_after.png`).

Mutation checked:
- the handler's input path off (`hand_input` not called): `live_think.txt` fails on `expect _cur 00` (the cursor is still `ff` two frames after LEFT) and on the underline's pixel;
- UP given back with START instead of dropped: `live_think.txt` fails on `expect _disp_n 06` (5: the ten covered the jack after the move) and on the cover's slot;
- the dots frozen at no dot: `live_think.txt` fails on the first dot's pixel;
- the RAM bank put back as left by the handler (bank 3) instead of `gd_ram_bank`: `moscowcheck` fails on the sum (1b b1 for e2 38: Moscow decided differently);
- the ROM bank put back as the handler's instead of the search's: `moscowcheck` never finishes (the search runs on in the wrong bank).

Not covered by a script: TARGET mode during a think (every key dropped) and START during a think (kept for after the move); both are one line each in `live_think`.

The stress test, `make livestress` (`tests/livestress.sh`): `moscowcheck` and `solvecheck` built with `LIVE_STRESS`, which runs the handler's stand-in (a 64-byte buffer worked on its stack in cart RAM bank 3) on every tick of a timer at 1024 Hz, 17 times the game's rate, while Moscow and the solver switch ROM and RAM banks under it; both sums must stay clang's (solvecheck on its first game: under the stress it runs 11 times slower).
- Measured on the Mac: 530883 handler runs through `moscowcheck` and 854680 through `solvecheck`'s 46 solves, both sums clang's.
- Mutation checked: the shadow's store taken out of `ram_bank` fails it (`solvecheck` sums a5 e0 for 0c e6); the hoistable macro the PR first had fails it (24 15), and with an empty handler at 60 Hz on every tick, sums 37 c1; at 256 Hz the ROM crashed. `moscowcheck` alone (6 solves) does not see a solver race; `solvecheck` (46 solves) does.

On the Mac, the same 25 decisions (`think_time.txt`): 4.81 s on average and 5.90 s at worst before the live screen, 5.00 s and 6.13 s after (+4%); with keys mid-think (`think_time_keys.txt`) 5.10 s and 6.24 s.

### What the sound merge must respect

Sound will also hook the input timer and share the bots' memory, so:
- `in_tick` runs `live_tick` last, and during a think the handler runs inside the timer interrupt with interrupts on, on a stack at 0xB400 to 0xBFFF of cart RAM bank 3. Any code the sound adds to `in_tick` runs on that stack when it nests in a think (keep it small), and must not depend on interrupts being off in `in_tick`.
- A ROM bank switch in sound code must write `_current_bank` before 0x2000 and put the bank back the same way (or run with interrupts off throughout): `live_tick` restores the ROM bank from `_current_bank`.
- Sound must never switch the cart RAM bank (0x4000). If it ever has to, it goes through `gd_ram_bank` the way `ram_bank` does, and puts back `gd_ram_bank` after.
- `gd_bot_cand`, `gd_bot_world` and `gd_bot_sim` (`gd_botlib.h`) are the search's own memory for the whole of a think: an overlay of them is safe only while no bot thinks (`bot_thinking` is 0, `live_on` is 0), never from the timer interrupt while a think runs.
- Cart RAM bank 3 above 0xB400 is the handler's stack; banks 1 to 3 below it are the solver's table, and bank 0 its stack and hash table: no sound data in cart RAM.

## Rules: foolish's variant, two seats

Read from `foolish/c/src/game.c`, `legal.c` and `card.h` and ported to `src/gd_rules.c`.
Where foolish has an option, its default is used.

- 36 cards (6 to A), 6 each; seat 0 draws its six, then seat 1 (foolish deals seat-major).
- The next card is turned for trump; an Ace is not turned while a non-Ace is left in the stock: it goes to the bottom and the next card is turned (foolish's house rule "the Ace does not lead the deck").
- The lowest trump leads; if nobody was dealt a trump, a random seat leads (one draw from the RNG on every deal, used or not, so the number of draws never depends on the deal).
- Perevodnoy (transfer): the defender may pass an uncovered table of one rank on with cards of that rank, if the next player then holds at least as many cards as the table has attacks.
  This is foolish's default (`GAME_RULE_NO_PASS` clear); podkidnoy is the option.
- Attacks: the leader opens with one or more cards of one rank; the attacker may then throw in any cards whose rank is on the table.
  The only limit is the defender's hand: never more uncovered attacks than the defender holds cards.
  There is no 6-card or "first bout 5" limit in foolish.
- A move may be several cards at once (an attack of a pair, a cover of several attacks, a transfer of two cards), exactly as foolish's legal-move menu lists them.
- The defender may pick up whenever the table holds cards (foolish's handler accepts a pickup over a fully covered table, though its menu lists pickup only while an attack is uncovered).
- An attacker ends its part of the bout with "good" (DONE).
  A bot may say it over an uncovered table, which only means it stops throwing in (a silent good, never shown); a human may say it only over a covered table (foolish `play_can_say_good`).
- The bout ends on the discard when every attacker has said good and every attack is covered; the defender leads next.
  A defender who covers with their last card ends the bout at once.
  After a pickup the same attacker leads again.
- Draws: the bout's first attacker (the non-defender) draws up to six first, the defender last.
- Out: an attacker or transferer who empties their hand when the stock is gone is out at once; anyone left with no cards after the draw is out.
  The one left in is the fool.
- The repetition draw (foolish `REPETITION.md`): the third occurrence of the same position right after a pickup that drew nobody a card ends the game as a draw, both players fools.
  The ring keeps the last 16 such positions, as foolish's does; this engine compares the positions exactly (seat 0's hand and who leads are the whole position within one history) where foolish compares 64-bit fingerprints.
- Not used: the fool's penalty on a rematch (`game_open_at_seat`), which only foolish's iMessage game applies.

### What was ambiguous

- foolish's Cover button resolves a several-card cover to the first matching entry of its menu, whose order follows the kernel's hand order.
  This engine keeps hands as sets, so its hand order is card id order; the differential test sorts foolish's hands the same way before comparing, which is legal (hand order is no part of the rules).
- foolish truncates a menu past 65536 moves (its production cap); this engine has no cap.
  In 60000 games the largest menu was 10326 moves, so no truncation was ever met.

## The rules core

- `src/gd_rules.c`: portable C, no GBDK headers, no malloc, no float; correct with a 16-bit int.
  The game is 82 bytes (hands as 36-bit masks, the stock in draw order, the table in battle order), so a search bot can clone it with one struct copy; the repetition ring (85 bytes) is separate and a clone leaves it out, as foolish does.
- The legal-move menu is a count and an unrank, never a list: the bot draws one number below the count and unranks it.
  Covers are counted with a table over (row, trumps used, own-suit cards used): the uncovered attacks are ordered so that each one's covers of each kind contain every earlier one's, which makes the count of free covers depend only on how many earlier rows took that kind.
- `src/gd_play.c`: what a gesture means on a board, foolish's `play_*` rules (pills, coverable battles, the Cover button's aim and the move it resolves to).
- `src/gd_rng.c`: the one random source, a 16-bit xorshift; the deal and the bot draw only from it.
  On the Game Boy the seed is the frame count at the first button press of the session, as the input interrupt saw it (`src/main.c` `GD_SEED_FIXED` makes it a constant with one line).
  `tests/rng_ban.sh` fails on any other random or time source (mutation checked: a planted `rand()` fails it).

## The proof: the differential test

`tests/difftest.c` links foolish's production engine (`../foolish/c/src/game.c`, `legal.c`, `card.c` and `../shared/c/deal_rng.c`, read-only, compiled into `build/` with foolish's production caps `MAX_LEGAL_MOVES=65536 MAX_MOVE_CARDS=40`).
It deals the same deck to both engines (`start_game_with_deck`, with `game_force_first_attacker` pinning the no-trump roll to this engine's draw) and plays: both seats choose uniformly random legal moves from the same draw sequence, and every move goes to both engines.
The card encodings are mapped explicitly (`f_of`, `gd_of`).
After every move it checks both seats' legal move sets (this engine's whole unranked menu against foolish's enumerated menu, as sorted canonical keys, with no duplicates), the hands, the stock in draw order, the trump card, the table in battle order, the leader, the defender, the goods, the outs, the discard count, the status and the result.
On random probes it also checks the handlers' accept and reject (illegal moves too), menu membership, and the gesture rules the d-pad is built on.
A second pass of the same games steers play toward the repetition draw (transfer when possible, else take), which uniform play almost never reaches.

`make test` runs 3000 games each way; the full run below is 30000 each way.

| Run | Games | Moves | Menus compared | Handler probes | Gesture probes | Draws |
| --- | --- | --- | --- | --- | --- | --- |
| 30000 seeds, both passes | 60000 | 8431694 | 16983388 | 33726776 | 50590164 | 6019 |

Result: PASS, no mismatch.

### Mutants (`tests/mutants.txt`, `make mutants`)

Each mutant breaks one rule in a copy of `src/`; the test must fail.

| # | Mutant | Caught by |
| --- | --- | --- |
| 1 | a lower card of the suit beats | handler probe on the first move |
| 2 | no throw-in limit (defender's hand) | menu count |
| 3 | the defender draws first | stock count |
| 4 | an Ace may be turned for trump | trump suit |
| 5 | no transfer (podkidnoy) | menu |
| 6 | the lowest trump defends instead of leads | first attacker |
| 7 | an attacker is out while the stock still has cards | state |
| 8 | the repetition draw on the second occurrence | status |
| 9 | the cover menu forgets trumps beat other suits | menu count |
| 10 | pickup on the menu over a covered table | menu membership probe |
| 11 | good ends the bout over an uncovered attack | stock count |
| 12 | the discard counts battles, not cards | discard count |
| 13 | covering with the last card does not end the bout | stock count |
| 14 | Cover aims at the rightmost of equal attacks | best cover probe |
| 15 | Take only while an attack is uncovered | pills probe |
| 16 | the Cover button pairs in selection order, not hand order | resolved cover probe |

16 caught, 0 missed.

### The cross-compiler check (`make selfplay`)

The difftest proves the clang build.
`tests/selfplay.c` plays 12 seeded games folding every menu count, move, pill and gesture answer into a CRC-16, once built by clang and once inside a test ROM built by SDCC (read back by gbrun); the two must be equal (1249 moves, sum 0x9b77).
It caught an SDCC miscompile (Quirks), and it fails again when that code is put back.

## Controls

The d-pad behaves like the arrow keys of the website's keyboard play mode (foolish `src/components/GameDisplay/KeyboardPlayMode.tsx`); its modifier keys map to buttons.

| Button | What it does |
| --- | --- |
| LEFT / RIGHT | No cursor yet: the cursor appears on the leftmost card. Otherwise move the cursor, wrapping at the ends. Held: repeats. |
| UP (attacker) | Attack or throw in with the cursor card, if that is legal. |
| UP (defender) | The cursor card covers the one attack it can beat, if it cannot pass; passes, if it can only pass; if it can cover several attacks, or cover and pass, TARGET mode. |
| UP (cards selected) | Play the selection as a set, like the website's buttons: attacker, an attack; defender, the cover the Cover button aims at (the highest attack the set beats, trumps above all, ties to the leftmost), else a transfer; an ambiguous or illegal set does nothing and stays selected. |
| DOWN | PICK UP (defender) or DONE (attacker), exactly when the foot rail offers it; neither while cards are selected. |
| A | Toggle the cursor card in or out of the selection (the website's Shift press); a selected card is raised. |
| A held + LEFT / RIGHT | Sweep-select: move the cursor and toggle each card it lands on (the website's Shift+arrows). |
| B | Clear the selection; in TARGET mode, back out. |
| SELECT held + LEFT / RIGHT | Move the cursor card along the hand, clamped at the ends; never plays (the website's Cmd+arrows). |
| START | Back to the dashboard at once, as the website's back button (no confirmation); the game stays open and the dashboard offers CONTINUE. |
| TARGET: LEFT / RIGHT | Move the arrow between the attacks the card can cover; one step right of the last is the pass space, when the card can also pass. |
| TARGET: UP | Cover the aimed attack, or pass. |
| TARGET: DOWN or B | Back out to the cards. |

Edge cases, as the website does them:

- With A or SELECT held, UP and DOWN do nothing, so a held modifier never plays.
- Up or down with no cursor does nothing, except DOWN, which takes or says done without a cursor.
- After my move the cursor keeps its index (pulled back to the last card if it fell off the end, gone when the hand is empty) and TARGET mode ends.
- A bot move ends TARGET mode only if it changed the table (a silent good, the bot no longer adding, leaves the aim alone).
- The hand keeps the website's sticky order: a card that comes back to the hand (a pickup) returns to its old place; new cards join at the end in the order they arrive (from the table in battle order, then from the stock in draw order).
- A selection loses cards that leave the hand.
- With 20 or more cards, 19 show and the hand scrolls with the cursor, a card early so the cursor never sits on an end card next to a tab; tabs count the hidden cards each side.

Input timing lives in `src/input.h`, for tuning by hand: `IN_REPEAT_DELAY` (20 ticks before a held LEFT or RIGHT repeats), `IN_REPEAT_RATE` (a repeat every 6 ticks), `IN_A_HELD_FRAMES` and `IN_SELECT_HELD_FRAMES` (0: held from the first tick, like a keyboard modifier).
A tick is the input timer's, 60.2 a second.

## The screens

The art is gameboy/art (its README has every layout); this code follows it: the table (top rail, the bot's backs, the trump under the deck, battles centred, the hand, the foot rail with the status and the move UP or DOWN makes), the traveller dashboard (the boarding pass for the open game, the departures board, the plane gliding to the selected city), a win picture per city with the bot's name and fireworks, and the lose picture.
Card corners are chosen by the colour actually under each round corner (read from the tile below, cached at start), which is the art's rule.
Beyond six battles (the art draws up to six) the table packs six to a row, three columns apart, and a third row past twelve overlaps; the differential test never saw more than seven uncovered attacks.

## Cards in flight

A move that puts cards on the table (attack, throw-in, cover, transfer) flies them from where they were (my hand, or the middle of the bot's) to their places, and a pickup flies the table's cards to the taker: 12 frames on the frame clock (`sys_time`), so a slow redraw does not slow a flight.
The board leaves a flying card's place empty until it lands.
At most four cards fly at once: a card is two sprites wide, and four of them with the cursor's two fill the ten sprites a line can show; the rest land at once.

A flying card is six sprites built from the card's own tiles (`gfx_spr_card`).
Clubs are the exception the art README asks for: light grey is index 0, which a sprite draws as transparent, so the three club tiles with seams have sprite copies (`T_SP_CLUB_SUIT`, `T_SP_CLUB_PIPL`, `T_SP_CLUB_PIPR`) whose seams are index 2, and every sprite of a flying club card is on OBP1 (`ART_OBP1`, set at boot), where index 2 is light grey.
Every other sprite stays on OBP0.
`scripts/flight_club.txt` checks the seams pixel by pixel in flight; with the OBP1 bit dropped every one of its eight seam checks fails (they read dark grey).

The sprites of a frame are built in a staging copy and handed to GBDK's shadow OAM in one copy with interrupts off.
Building them in the shadow OAM itself let the VBlank copy a half-built set: moving the aim in TARGET mode showed the new arrow's first dots beside the old arrowhead for a frame.
`scripts/target_cover.txt` watches that move frame by frame; with the old in-place build it fails on the torn frame.

## The discard pile and the sweep

The pile is the art's (`art/README.md` "The discard pile"): nine tiles (`pile`, bank 2, loaded with the table's banks to tiles 160..168) drawn at columns 17-19, rows 1-3 once the discard is not empty, and nothing while it is.
The bot's backs keep to columns 0-16 (`ART_BOT_HAND_LAST_COL`, 16 shown at most, centred there; the top rail has the true count).
Nothing else is in rows 1-3 right of column 16: battles start at row 4 in every layout (up to six, past six, past twelve), as do the TARGET arrow and brackets.

When a move ends the bout on the discard (the discard count grew: a last cover, or DONE over a covered table) the table's cards fly to the pile, landing with their top left at (143, 8), by the one flight code: 12 frames on `sys_time`, the staging buffer, at most four in flight.
Choice: fly the top cards and drop the rest, rather than fly in batches.
A batch of four takes 12 frames, so twelve cards (six battles, the most the art lays out; more are rare) would be three flights, 36 frames, with the bot's next move and my input waiting or racing the flight's state (`fly_*` is one flight's state); the top cards are what the eye sees go, the rest are covered by them, and a pickup already does exactly this (the first four fly, the rest land at once).
Top is the cards of my own last cover first (they fly from my hand, or the bot's), then the table from its last battle backwards (the cover, then its attack).
No new RAM: the sweep fills the flight arrays that were there, and the one new local is a byte on the stack.

### Hypothesis (written for what I had not measured; a slip, I had already looked at the RAM and bank sizes)

I built before writing these, so the RAM line and the bank-size lines are not predictions; the rest were made before the numbers.
- RAM: no change (no new variables). Measured: `_DATA` 7487 bytes before and after, the stack top still at 0xE000 with 539 bytes above the data: right (a bank-1 stack frame grows by a byte or two).
- Bank 1 (`main`, `gfx`): about 250 bytes more, falsifier over 600. Measured 13579 to 13957: +378, wrong against my 250 but under the falsifier (the pile loop, the sweep list and the 16-column centring). Bank 2 +144 (the pile's 9 tiles): right, it is the tile size.
- The worst board redraw (`draw_lines_max`, `game_win.txt`): 661 scanlines before; 9 `gfx_tile` calls and the row diff should add about 10, falsifier over 60. Measured 698: +37, right.
- A sweep flight is 12 frames, 2 cards for a one-battle table, the first card at t = 6 half way between its start and (143, 8), at t = 11 within 6 px of it. Measured exactly that: `_fly_pos` is (107, 32) at t = 6 and (137, 12) at t = 11 from (72, 56): right.
- Falsifier for the layout: a bot hand of 32 cards drawn with 16 backs reaches pixel 135 at most; right (the test below).
Timings on this VM are indicative only; the Mac re-measures.

### The proof

- `scripts/sweep.txt`: the pile is absent at the start (three pixels read wool) and while the bout is open; DOWN says DONE, both cards of the battle fly (`_fly_n` 2), pinned at frames 6 and 11 of the flight (`_fly_pos` equals the interpolation toward (143, 8), so it fails if they head anywhere else, or land elsewhere); after the landing the table is empty (`_G+74` 0, the attack card's black edge at (65, 48) is wool again) and the pile's black reads at (141, 20) and (156, 20).
  Mutants, each failing on its own assertion: the sweep landing at the old left position (0, 64) fails both `_fly_pos` expects; the pile drawn when empty fails the first pixel check (the pile's black on an empty discard, frame 119); the sweep never flying (`swept` always 0) fails `until _fly_n 02`, which runs to its frame limit (no flight ever starts).
- `scripts/pile_busy.txt`: 32 cards in the bot's hand, a card in the discard: 16 backs, the last one's dark edge at (135, 20), wool at (137, 20), the pile whole beside it.
  Mutant: the old centring and cap (18 backs from column 0 of 20) draws the last back's edge elsewhere: pixel 135 reads white and the script fails.

## The deal

The opening deal and every refill from the stock fly, one card at a time, through the same flight as a played card (art README "The deal", `mockups/05_deal.png`).
- The opening: twelve cards leave the deck at (1, 4), alternating bot and you, the bot first; then the trump is turned up last: the top card flips on its way to (0, 4) and the stock is set down on it as it lands.
- A card to the bot flies as a back (6 sprites); a card to you flips in flight: back (6 sprites), `SP_NARROW_*` (3), `SP_EDGE_*` (3), then its face (6), each a quarter of the flight, and lands in its place in the hand. A club's face uses the club's sprite copies on OBP1, as any flying club does.
- A refill after a bout uses the same flight, in the order the rules draw: the attacker's cards, then the defender's; the face-up trump, when drawn, flies face up from (0, 4) as the last card.
- `DEAL_FRAMES` (`src/main.c`) is the frames a card: the one pacing constant.
- The board shows the deal as it goes: a card not yet arrived leaves its place empty (the hand and the bot's backs are laid out for their final count, so every card lands where the settled board draws it), the deck count and the bot's count on the top rail count the cards as they leave and land, the trump suit icon appears with the trump, and the foot rail says DEALING during the opening.
- While cards are dealt the bot waits (its half second starts when the last card lands) and your presses are kept, not dropped: they act once the deal has landed; START still leaves at once.
- The state is 5 bytes (`dl_n[2]`, `dl_first`, `dl_flags`, `dl_seat`) beside the flight's.
  It is not overlaid on the bots' scratch: the phases are exclusive today (the bot never thinks during a deal), but another branch makes the screen live while a bot thinks, and a refill then would run beside the search.

### Hypothesis (written before measuring)

- Pacing: with `DEAL_FRAMES` 8, the opening's 13 flights (12 cards and the trump) take about 125 frames (2.1 s) from the first card's first frame to the last landing: 8 frames a card plus about 1.5 frames of board redraw at each landing (the next card's flight starts when its first frame is drawn).
  Falsifier: under 110 or over 140 frames.
- A landing's board redraw (`draw_lines_last`): 150 to 300 scanlines (1 to 2 frames).
  Falsifier: over 460 scanlines (3 frames), which would show as a stall between cards.
- ROM: bank 1 grows by about 600 bytes. Falsifier: over 1.2 KB.
- RAM: 5 bytes of static state, nothing on the stack beyond a few bytes of locals in the main loop's own calls. Falsifier: the gap between the data and the stack's top shrinks by more than 8 bytes.

## gbrun, the headless runner

`tools/gbrun.c` runs the ROM on libmgba: it holds buttons, writes PNG screenshots, reads, checks and writes memory, waits for memory to reach a value, and checks screen pixels and counts (`atleast SYM LEN MIN`: a little-endian number at least MIN, the stack guard's headroom; `pixel X Y SHADE`; `pixelspr N DX DY SHADE`, relative to sprite N's place in the hardware OAM, which is where the frame drew it; `notboth FRAMES ...`, two pixel states that must never share a frame, watched frame by frame), from a plain text script (the format is at the top of the file).
Every script in `scripts/` sets the seed by poking `sys_time` just before its first press, so its deal does not depend on how long the ROM takes to start, and waits for the bot with `until` rather than counting frames.

| Script | What it proves | Shots |
| --- | --- | --- |
| `deal.txt` | the dashboard, a deal, the cursor appearing, an attack, the bot's answer | `01` to `05` |
| `target_cover.txt` | TARGET mode over two attacks, moving the aim (watched frame by frame: never the old arrowhead beside the new arrow's dots), DOWN backing out, the cover | `10` to `13` |
| `cover_or_pass.txt` | TARGET mode with the pass space, passing | `20` to `22` |
| `single_moves.txt` | UP passes at once with a pass-only card | `30`, `31` |
| `cover_now.txt` | UP covers at once with a one-way cover | `32`, `33` |
| `take.txt` | DOWN picks up (the shot once the cards land: at a fixed frame it caught a redraw half sent) | `40`, `41` |
| `selection.txt` | A toggles, B clears, A held sweeps (and deselects), UP plays three sixes, the bot takes | `50` to `52`, `55` |
| `done.txt` | DOWN says DONE over a covered table, the bout ends | `53`, `54` |
| `rearrange.txt` | SELECT held + RIGHT moves a card twice, UP with SELECT held does nothing | `60`, `61` |
| `game_lose.txt` | a whole game by an input loop, a 27-card hand with tabs, the lose picture, home | `70`, `72`, `73` |
| `game_win.txt` | a whole game won by an input loop, the Miami win picture with fireworks, a rematch | `80`, `81` |
| `game_new_york.txt` | a whole game against New York, and its time per decision | `nyc_01`, `nyc_02` |
| `win_screens.txt` | each city's win picture with its name and fireworks | `win_*` |
| `game_seoul.txt` | a whole game against Seoul | `seoul_01`, `seoul_02` |
| `game_moscow.txt` | a whole game against Moscow: MOSCOW THINKS on the rail while it searches, the finished game, its times | `moscow_01` to `moscow_03` |
| `game_moscow_endgame.txt` | Moscow's solver inside the real game (a poked 4 v 4 endgame, 3 solves) and the stack guard's 48 bytes | |
| `win_fireworks.txt` | the four firework frames of each city, saved for `tests/fwcheck.py`, which compares them with the art's own render pixel by pixel | `build/fw/` |
| `live_think.txt` | while Moscow thinks: the dots step, frame-checked; LEFT, A, SELECT + LEFT and B work before the move lands; UP pressed meanwhile is dropped | `live_01` to `live_05` |
| `think_time.txt` | Moscow's think time on 25 decisions that are the same in any ROM (no key during a think) | |
| `think_time_keys.txt` | the same game with keys pressed during each think that leave the hand as it was | |
| `flight_club.txt` | a club card in flight over another card: its seams read light grey, pixel by pixel (the eight seam pixels of the flying nine of clubs, two of them over the eight of clubs' black edge) | `flight_club`, `flight_club_landed` |
| `sweep.txt` | the discard pile absent at first, then the bout's cards flying to it, pinned mid-flight and landing, the table empty, the pile drawn | `sweep_01` to `sweep_05` |
| `pile_busy.txt` | 32 cards in the bot's hand beside the pile: 16 backs in columns 0-16, the pile whole | `pile_busy` |
| `dashboard.txt` | START to the dashboard, CONTINUE, a new game against New York, a tap during a screen load kept | `90` to `93` |

SameBoy 1.0.5 (GUI) runs the same ROM: `shots/sameboy_dashboard.png` and `shots/sameboy_deal.png` (macOS `screencapture` of its window), identical to mGBA's apart from SameBoy's green palette.

`make rom-mutants` breaks one control at a time in a copy of the ROM (`tests/rom_mutants.txt`) and requires some script to fail, after an unmutated control run passes.

## Quirks hit

- SDCC 4.5 (SM83) miscompiled a variable-shift bit test, `(byte >> (c & 7)) & 1` through a mask pointer: the generated code computed the byte's address and then shifted the bit index instead of the byte, so a card in hand read as absent and no move was legal (seen in the assembly).
  `-Wf--nolospre` hid that one case but not the next; the bit tests now go through a table (`gd_bit`), and `make selfplay` is the check that keeps SDCC honest.
- With an MBC the game code and GBDK's library live in 0x4000-0x7FFF too, which an art bank replaces when switched in, so `_HOME` is linked first (in bank 0, `tests/home_in_bank0.sh` checks it) and only its loop copies run while an art bank is mapped, with interrupts off, in 256-byte pieces.
- A Makefile flag change did not rebuild the objects, so a stale object overflowed bank 1 and the ROM crashed (every read became 0x39); the link now fails on any overflow warning.
- With the LCD off (a screen load) there are no VBlank interrupts, so buttons are read on the timer interrupt instead; a press latched in the same pass as START is given back for the dashboard.
- `sys_time` ticks at line 144 and LY wraps at 154, so a timer built from the two went negative; the line clock counts from line 144.
- mGBA logs that GBDK's start-up writes an MBC1 RAM bank on a cart with no RAM; gbrun sends the emulator's log to stderr.
- mGBA's video buffer is sized for a Super Game Boy border (256x224); gbrun crops to 160x144.
- The solver's frames are in bank 0 of the cart's RAM and its table in banks 1 to 3, and its first ROM read the frame's slot index after switching to the table's bank: a read of the wrong RAM, so the ROM searched more nodes than the clang build. `make solvecheck` caught it; the code now copies what it needs out of the frame before every switch.
- The rollout state is 33 bytes, not the 32 the plan said; the solver's stack layout uses `sizeof`, and a compile-time check fails the build if the stack outgrows its bank.
- `sys_time` is 16 bits, so a long bench wraps it: the clocks take differences across the wrap, and a decision is timed in 32 bits (Moscow can think past the 7 s that 16 bits of scanlines hold).

## Not done

- Moscow's solver runs at 8.2 ms a node, most of it SDCC's code for move generation; assembly there (as for the hash) would buy it bigger endgames, though the measurements above say the worlds matter more at two players.
- Seoul's sampling (17 ms a world) and the rollout engine (3.3 ms a ply) could both go further; the engine missed its 150 ms average target by 7%.
- The deal is not animated (the art has the frames); played and picked-up cards fly (below).
- A board redraw takes a few frames, and the sprites (the cursor, the aim, a flight) update after it: moving the aim shows the old arrow for about four frames, then the new one, whole.
- The row-by-row flush can tear for one frame: picking up shows a frame whose rail and hand row are half old and half new (frame 5 after DOWN in `take.txt`, whose shot waits a frame for it).
  The fix is double buffering: write the changed rows into the second background map (0x9C00) and flip LCDC bit 3 in the VBlank, diffing against what that map last held.
- A draw (the repetition rule) has no picture; the table says DRAW and A deals again.
- The lose picture shows seven card backs whatever is left in your hand: the backs' top row shares its tiles with the bottom of the fool's cap above it (row 11, a different tile in every column), so another count needs new composed tiles from the art side, not a cheap change here.
