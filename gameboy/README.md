# Durak for the Game Boy

Two-player Durak (you against one bot) for the original Game Boy (DMG), in C with GBDK-2020.
The bot is RANDOM: it plays a uniformly random legal move, drawn from the one seedable generator.
The four bots on the dashboard (Miami, New York, Seoul and Moscow, the boss) all play that random brain for now; the city is the name the board shows.

```
make                # build/durak.gb (GBDK= points at GBDK-2020 4.5.0)
make test           # the RNG ban and the differential test against foolish's engine
make mutants        # break each rule in a copy; the differential test must catch every one
make selfplay       # clang and SDCC must play identical games (the cross-compiler check)
make scripts        # every gbrun script against the ROM: screenshots in shots/, expects checked
make rom-mutants    # break each control in a copy of the ROM; some script must catch every one
make romusage       # ROM and RAM per bank
```

## Hypotheses (written before measuring) and measurements

- ROM: about 14 KB of the 32 KB ROM-only cart (code about 10 KB, as SDCC pulls in its 32-bit multiply and divide helpers; the binomial table 2.8 KB; tiles about 1.3 KB).
  No MBC needed.
  Falsifier: over 28 KB, which would force banking.
  - Measured with placeholder tiles: 26.4 KB, so the hypothesis was wrong by almost 2x (SDCC's SM83 code for the rules is about 2.5 times what I guessed), but it fitted 32 KB.
  - Measured with the real art: it does not fit 32 KB, so the cart is MBC1, 64 KB: bank 0 15.9 KB, bank 1 14.9 KB (the code and the library, which the CPU always sees), bank 2 12.5 KB (the table's tile banks, the fonts, the dashboards, the lose picture), bank 3 10.7 KB (the four win pictures).
  - Code alone is 30.7 KB of the 32 KB the CPU sees without switching banks, so the next big piece of code (a search bot) needs banked code.
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

- The other bots: New York, Seoul and Moscow play the random brain (the plan: handwritten, a cut-down robusta, a cut-down cordite).
- Moscow's art: the code says MOSCOW, but until the art branch's rename lands the board and the fourth win picture are still Warsaw's (`win_warsaw`).
- Motion: cards do not fly, the deal is not animated (the art has the frames).
- A draw (the repetition rule) has no picture; the table says DRAW and A deals again.
- The lose picture shows seven card backs whatever is left in your hand (they are baked into the art).
