# Durak for the Game Boy

Two-player Durak (you against one bot) for the original Game Boy (DMG), in C with GBDK-2020.
Four bots on the dashboard: Miami plays a uniformly random legal move, drawn from the one seedable generator; New York is foolish's handwritten_prod bot, ported (below); Seoul and Moscow (the boss) play Miami's random brain until theirs land.

```
make                # build/durak.gb (GBDK= points at GBDK-2020 4.5.0)
make test           # the RNG ban and the differential test against foolish's engine
make mutants        # break each rule in a copy; the differential test must catch every one
make selfplay       # clang and SDCC must play identical games (the cross-compiler check)
make scripts        # every gbrun script against the ROM: screenshots in shots/, expects checked
make rom-mutants    # break each control in a copy of the ROM; some script must catch every one
make playout        # one random playout timed on the Game Boy
make romusage       # ROM and RAM per bank
```

## Hypotheses (written before measuring) and measurements

- ROM: about 14 KB of the 32 KB ROM-only cart (code about 10 KB, as SDCC pulls in its 32-bit multiply and divide helpers; the binomial table 2.8 KB; tiles about 1.3 KB).
  No MBC needed.
  Falsifier: over 28 KB, which would force banking.
  - Measured with placeholder tiles: 26.4 KB, so the hypothesis was wrong by almost 2x (SDCC's SM83 code for the rules is about 2.5 times what I guessed), but it fitted 32 KB.
  - Measured with the real art: it does not fit 32 KB, so the cart is MBC1.
  - With New York the code no longer fit the 32 KB the CPU sees at once either, so the code is banked (128 KB cart, `src/gd_bank.h`):

| Bank | Holds | Used |
| --- | --- | --- |
| 0 (fixed) | the rules core (`gd_rules.c`), the RNG, the input interrupt, `_HOME` and all of GBDK's library | 10.0 KB |
| 1 | the UI: `main.c`, `gfx.c`, `gd_play.c` | 13.3 KB |
| 2 | the table's tile banks, the fonts, the dashboards, the lose picture | 12.6 KB |
| 3 | the four win pictures | 13.2 KB |
| 4 | New York (`gd_bot_hw.c`) | 7.0 KB |
| 5 | the menu count and unrank (`gd_menu.c`, the random bot's, and the lite menu) | 8.4 KB |

  - The rule that makes it work: anything a banked brain calls must be in bank 0, so the core and the library are linked first and nothing else is unbanked (the link fails on any overflow); a new brain gets a bank of its own and one `GD_BANKED` entry point.
- RAM: about 4.6 KB of the 8 KB work RAM, most of it the cover-counting table (3.8 KB).
  Measured: 5.5 KB (3.9 KB + 1.6 KB).
- Bot decision time (count the menu, draw, unrank, apply): 20 to 60 scanlines (2 to 7 ms) for a typical position, under 3 frames (50 ms) at worst.
  Falsifier: a typical move over one frame (154 lines, 16.7 ms).
  - Measured over two whole scripted games: 251 and 261 scanlines on average (27 to 28 ms, 1.7 frames), worst 1186 scanlines (129 ms, 7.7 frames).
    So the hypothesis is falsified: typical is 5 times my guess.
  - Profiled (scanline clock around each phase, one game): counting the menu, unranking the move and applying it took about a third each; applying was the largest because the hand-order code scanned 36 x 36; that is a mask now.
  - It does not show: the bot waits half a second (30 frames) after the board last changed before it moves, by design.
- Table redraw: the slowest whole-board redraw in those games was 582 scanlines (3.8 frames); moving the cursor redraws only the foot rail and the sprites.
  Buttons are latched by an interrupt, so a slow redraw never loses a press.

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

## gbrun, the headless runner

`tools/gbrun.c` runs the ROM on libmgba: it holds buttons, writes PNG screenshots, reads, checks and writes memory, and waits for memory to reach a value, from a plain text script (the format is at the top of the file).
Every script in `scripts/` sets the seed by poking `sys_time` just before its first press, so its deal does not depend on how long the ROM takes to start, and waits for the bot with `until` rather than counting frames.

| Script | What it proves | Shots |
| --- | --- | --- |
| `deal.txt` | the dashboard, a deal, the cursor appearing, an attack, the bot's answer | `01` to `05` |
| `target_cover.txt` | TARGET mode over two attacks, moving the aim, DOWN backing out, the cover | `10` to `13` |
| `cover_or_pass.txt` | TARGET mode with the pass space, passing | `20` to `22` |
| `single_moves.txt` | UP passes at once with a pass-only card | `30`, `31` |
| `cover_now.txt` | UP covers at once with a one-way cover | `32`, `33` |
| `take.txt` | DOWN picks up | `40`, `41` |
| `selection.txt` | A toggles, B clears, A held sweeps (and deselects), UP plays three sixes, the bot takes | `50` to `52`, `55` |
| `done.txt` | DOWN says DONE over a covered table, the bout ends | `53`, `54` |
| `rearrange.txt` | SELECT held + RIGHT moves a card twice, UP with SELECT held does nothing | `60`, `61` |
| `game_lose.txt` | a whole game by an input loop, a 27-card hand with tabs, the lose picture, home | `70`, `72`, `73` |
| `game_win.txt` | a whole game won by an input loop, the Miami win picture with fireworks, a rematch | `80`, `81` |
| `game_new_york.txt` | a whole game against New York, and its time per decision | `nyc_01`, `nyc_02` |
| `win_screens.txt` | each city's win picture with its name and fireworks | `win_*` |
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

## Not done

- Seoul and Moscow play the random brain (the plan: a cut-down robusta and a cut-down cordite).
- Motion: cards do not fly, the deal is not animated (the art has the frames).
- A draw (the repetition rule) has no picture; the table says DRAW and A deals again.
- The lose picture shows seven card backs whatever is left in your hand: the backs' top row shares its tiles with the bottom of the fool's cap above it (row 11, a different tile in every column), so another count needs new composed tiles from the art side, not a cheap change here.
