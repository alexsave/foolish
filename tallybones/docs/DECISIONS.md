# Tallybones - decisions

The one decisions doc for the proof of concept.
The owner can veto any row; the working title is the easiest one to veto and is one string in the kernel's word table.
Numbered T1, T2, ... in the order they were taken.
Workers append their own rows at the end of the section they belong to; keep each row short.

## Naming

T1: the working title is **Tallybones**.
"Tally" is the scorecard, "bones" is the oldest English slang for dice, and the compound is not a word anyone else uses for a game as far as the orchestrator knows.
Rejected: "Yahtzee" (Hasbro), "Kniffel" (Schmidt Spiele), "Generala" (ruled out by the owner), "Yacht" (the 1938 public domain ancestor, but so many clones use it that the name is crowded and it reads as a knock-off), "Farkle" (a registered trademark of Legendary Games for a dice game, and a different game besides: push-your-luck, not category scoring), "Handful" (a plain English word that already appears in `shared/` prose, so the shared-is-shared guard could never list it).
A folk name like "Yamb" was considered, since the sibling game took "Chui Niu" that way, but the Balkan Yamb scorecard is itself a close copy of the branded one, which is what we are trying to stand clear of.
Folder `tallybones/`, C prefix `tb_`, Swift module `TallybonesKit`, targets `TallybonesMessages` and `TallybonesMessagesApp`, xcframework `Tallybones.xcframework`, C module `CTallybones`, CI lane `.github/workflows/tallybones.yml`.
The shout word for five matching dice is "Tallybones!", which is the game's own and doubles as the name of that category.
Owner action: a USPTO and App Store search before any store listing (BLOCKED, below).

## Rules

T2: 2 to 8 players, seated in join order; the lobby is Pick 'Em Up's lobby copied (`pk_lobby`), so the same join / leave / start flow and the same seat resolver.

T3: a turn is up to three rolls of five dice.
Roll 1 rolls all five.
Before roll 2 and roll 3 the player marks any subset to keep; the unmarked dice reroll.
A player may stop after roll 1 or roll 2.
After the last roll the player must score the five dice into exactly one open category on their own card.
A turn is one to three bubbles: each keep-and-reroll is its own bubble that must be SENT before the reroll exists (T11), and the category choice is the last bubble of the turn.

T4: the scorecard has 13 categories, in two halves.
The numbers half: Ones, Twos, Threes, Fours, Fives, Sixes, each scoring the sum of the dice showing that number.
A numbers half of 63 or more earns a 35 point bonus (63 is three of each number).
The combinations half: Three Alike (any three matching: sum of all five dice), Four Alike (any four matching: sum of all five), Full House (three of one and two of another: 25), Short Run (four in sequence: 30), Long Run (five in sequence: 40), Tallybones (all five matching: 50), Any (no requirement: sum of all five).
A category may be scored with dice that do not satisfy it, for zero; that is how a bad turn is spent.
No second-Tallybones bonus and no joker rule: they are the branded game's own ornaments and the proof of concept does not need them.
The names are generic dice-poker descriptions, deliberately not the branded card's words ("Small/Large Straight", "Chance", the shout word); the layout in the app must not reproduce the branded card either (see `LEGAL.md`).

T5: the game ends when every seat has filled all 13 categories (13 turns each, in seat order).
Highest total wins; a tie is shared and the caption says so.
A player who leaves mid-game is skipped; their card stands as it is.

T12 (kernel): seat 0 rolls first, whoever pressed Start; the order is the roster's.

T13 (kernel): LEAVE is a move any seat still in may send at any time, as its own bubble.
The turn seat's leave passes the turn at once (the next seat's roll 1 derives from the history through the leave).
When fewer than two seats remain in, the game ends there, and the winner is the highest total among the seats still in; a seat that left never wins.

T14 (kernel): Full House is three of one face and two of another, so five alike is not a full house; a long run also scores as a short run.

T15 (kernel): `turns` counts SCOREs, and it is the turn index the roll hashes; a turn seat's leave is not a turn.

## Randomness

T6: every roll is derived, never rolled by the phone, through `deal_rng` (`shared/c/deal_rng.{c,h}`, the same RNG that deals Foolish's and Pick 'Em Up's decks), seeded with the SHA-256 digest of the sent history that precedes the roll (T11 says exactly which bytes).
The game seed is the 32 bytes the lobby's start draws, exactly as Pick 'Em Up's deal seed, and it is in every bubble.
Roll 1 of a turn has no choice in front of it, so it is derived from the history as it stood when the turn began (the previous bubble), and a player sees it the moment they open the extension on that bubble.
Rolls 2 and 3 are each derived from a keep-and-reroll commit that has already been sent (T11).
Proven by `tb_test`: same history -> same five dice, however many times the derivation is asked for, and a draft never yields any.

### T11: the keep choice is an RNG input, and a reroll cannot be previewed (owner correction, 2026-09-27)

The owner's requirement: which dice a player keeps must itself feed the RNG for the next roll, AND it must be structurally impossible to see what a given keep-subset would produce without having already sent a bubble that commits to that subset.
The exploit this closes: stage "keep A", look, cancel the draft for free, stage "keep B", look, and send whichever came out best.
Deriving from the parent envelope's digest alone (section 1.4 of `docs/IMESSAGE_APP_IDEAS.md`) only stops re-rolling the SAME subset; it does not stop comparing subsets before committing.

The mechanism chosen: **commit-then-reveal in the history, with no reveal bubble.**

- A keep-and-reroll is its own move in the history and its own bubble: the KEEP move carries only the 5-bit keep mask.
  Its bubble picture shows the kept dice and blank slots for the rerolling ones, and its caption says "Alex keeps 3, 3, 5 and rerolls two"; neither can show the reroll, because the sender's kernel has not derived it.
- The reroll's values are derived from SHA-256 over the game seed and the ENCODED HISTORY THROUGH THAT KEEP MOVE (the mixed-radix body of the keep bubble, which is a deterministic function of the seed and every move including this one), plus the seat, the turn index and the roll index.
  So the committed subset is an input, and so is everything sent before it.
- The kernel derives reroll values ONLY while replaying a RESIDENT (sent or received) history.
  A draft is "resident history + one pending move", and when the pending move is a KEEP the replay marks the rerolling dice as unknown (value 0) and derives nothing.
  There is no kernel entry point, and no bridge entry point, that takes a draft and returns dice values for it; the only derivation lives inside the resident replay path.
- The sender continues the turn from their own sent bubble: when Messages reports the send (`didStartSending`) the extension adopts that envelope as resident, the replay derives the reroll from it, and the player sees the new dice and chooses again (another KEEP, or SCORE).
  Until then the tray shows blanks.
  The same holds for every receiver: opening the keep bubble replays the sent history and shows the same dice.
- A turn is therefore 1 to 3 bubbles from the same seat in a row: [KEEP] [KEEP] SCORE.
  The race rule and the seat resolver copied from Pick 'Em Up must allow consecutive bubbles from one seat within a turn.

Why not the two-bubble commit + reveal the coordinator sketched: the reveal bubble would carry values that every phone can already derive from the commit bubble, so it adds a send without adding information; the "no reveal bubble" shape gives the same guarantee with one bubble fewer per reroll.
The guarantee is the same because the thing a player must have SENT before any value exists is the keep bubble itself.

The kernel test that tries to defeat it (`tb_test`, the T11 group, mutation-checked like every other test):
1. Stage a KEEP with subset A on a resident game; read the draft view: the rerolling dice are unknown (0) and the draft caption names no values.
   Cancel; stage KEEP with subset B; same: unknown, and no function in `tb.h` / `tb_api.h` returns values for either draft.
2. Adopt the KEEP-A bubble as resident (the send echo); now the view has values for the rerolled dice, they are stable across repeated reads and repeated decodes of the same bytes, and they differ from the values the KEEP-B history would have produced (the subset is an input).
3. A history that contains a SCORE whose dice do not match the derived dice cannot be constructed, because the wire carries no dice values at all, only keep masks and categories; the test asserts the move alphabet has no value field.
Mutation for 1: make the draft replay derive (drop the pending-move guard) and the test fails on "draft has values".
Mutation for 2: seed the derivation without the history (seed, seat, turn, roll only) and the test fails on "A and B produce the same reroll".

T16 (kernel): the roll's hash input is exactly `seed (32) || u16le body length || body || seat (1) || u16le turns || roll (1)`, the length making the variable-length body unambiguous.
The body is the minimal little-endian mixed-radix number the wire would carry for the history through the move (the one byte 1 for the empty history).
`mixrad` folds backwards, so the replay keeps the same number forwards as S + P (`tb_code.h`); `shared/c/mixrad` has no bignum add, so that sum is a local helper in `tb_code.c` and `shared/` is untouched.
Each roll draws one value per position, kept or not, so die i is draw i whatever else is kept, and two subsets that reroll a position differ there only through their histories.

T17 (kernel): the envelope's own read-back of a link it is writing decodes WITHOUT deriving (a draft is never rolled, not even in scratch), and `tb_msg_text_peek` is that read for comparing chains (prefer, common, same game, "is this my staged bubble").

T18 (bridge): `tb_api_read` refuses my own staged, unsent bubble (`TB_ESTAGED`), found move by move without deriving, so a host cannot adopt its staged link by accident.
The limit, recorded rather than enforced: a link a host kept after a cancel decodes like any link, so T11 also rests on the extension never reading back a link it did not send.

T23 (kernel): roll 1 of turn 0 depends on the seed alone, and the seed is drawn when the lobby is made (Pick 'Em Up's `pk_api_new`), so a modified client could remake lobbies to shop for seat 0's opening roll; accepted for the proof of concept, since every later roll depends on sent moves.

## Wire

T7: the wire is Pick 'Em Up's: every bubble carries the whole game as version + seed + roster + one mixed-radix number that is the move history, base32 in `MSMessage.url`, with the same race rule and seat resolver (copy `pk_code.c` / `pk_msg.c`, rename, and shrink the alphabet of moves).
The move alphabet is: KEEP(mask 0..30; 31 is not a move, since keeping all five is not a reroll), SCORE(category 0..12), plus the lobby moves (join, leave, start) as in Pick 'Em Up.
The wire carries NO dice values ever: every die on every phone is replayed from the seed and the moves (T6, T11).
At most 3 moves per turn, 104 turns for 8 players, well under 200 bytes before base32; the fit is asserted at compile time against `MSMessage.url`'s 5,000 characters, as Pick 'Em Up does.
Everyone always sees everything (grade A), so there is no masked view: `tb_view` is the plain game state.

T19 (wire): magic 0xD7, format 1, flags DM and LEFT only (Pick 'Em Up's TIP_SAID has no meaning here).
The race rule is more turns, then more bubbles, then lobby_rev and seats, then the smaller digest; the sender is the newest move's seat, so KEEP, KEEP, SCORE from one seat is one chain growing.
One digit a bubble, base at most 52 (6 bits): the analytic bound is 1,203 link characters with eight 48-byte names, asserted at compile time; the longest real game (8 seats, 312 bubbles, 48-byte names) is 1,130.

## Words and animation

T8: captions are the kernel's (`tb_say`), in the shape "Alex rolled a full house, 25 points" / "Bo to roll" / "Alex wins with 241"; English only for the proof of concept, through the same `datagen` table shape as Pick 'Em Up so a second language is a file.

T9: the animation plan is the kernel's (`tb_plan`, `tb_beats`) and small: opening a KEEP bubble plays one dice-settle beat for the rerolled dice (the kept ones stay put); opening a SCORE bubble plays the score stamping into the category and the turn passing; opening the bubble that starts a turn plays the five-dice settle of roll 1.
Sender and receiver play the same beats from the same resident history.

T20 (words): a caption is the move, then the bonus and the next roll while the line stays within 36 columns; the game's last bubble says only the result ("Alex wins with 241", "Alex and Bo tie at 200"); the summary (`summaryText`) says every clause.
The five-alike category's name and shout are `{game}`, so "Tallybones" is written once, in GAME_NAME; `tb_say_test` refuses "yahtzee", "kniffel", "generala", "yacht", "straight" and "chance" in every string and every composed sentence.

T21 (animation): the beats are settle 620 ms a die, 70 ms apart, stamp 340, turn 340, fade 220, the result held 1,000; a bubble opened leads by 100 ms, one that lands or is sent (the send echo) by 16.
A tumbling die's face is a blur of time and its landed value, and a settle is only ever laid out for a resident roll, so no frame can show a draft's reroll.

## iOS

T10: SwiftUI extension copied from Pick 'Em Up's shell (`MessagesViewController`, the lobby, felt and textures, seat badges, `BeatPlayer`), a dice tray in the middle of the felt instead of the pile, and the player's own scorecard as a tappable list; other seats show their running total on their badge and open their card on a tap.
Dice faces are drawn (pips as circles on a rounded square), not image assets, so there is no art to license.

T50 (ios): a die on the tray is 52pt, pickemup's widest hand card, with a corner radius of 0.2 of its side, and the five stand 10pt apart, so the tray is 300pt and fits the 304pt board of a 320pt phone.
The face is foolish's card bone (#F4EFE6) and the pips its card ink (#17140F), each pip 0.18 of the side across, on the classic 3 x 3 grid at 0.27 / 0.5 / 0.73.
An unknown die (value 0, T11) is the same square at 55% opacity with a dashed inner outline and no pips, so it never reads as a value.
A kept die wears a 2.5pt brass ring (foolish's win brass, #D8B24A) 1.5pt outside its edge, which leaves 2pt between two kept neighbours.

T51 (ios): the roll animation is a tumble of 600ms per die, dice starting 30ms apart in slot order: 1.5 revolutions on a cubic ease-out (alternate dice turning the other way), a swell to 1.18 at 30% and back, and the face flicking through fixed pip values every 80ms until 70%, then showing the value it landed on.
It plays for the dice that just got a value: the rerolled ones when a KEEP is sent or opened, all five on a turn's first roll.
The flicker is decoration, a fixed function of die and time, never a hint.
It is Swift for now behind a `DiceMotion` protocol so the kernel's dice-settle beat (T9) can drive the same tray.

T52 (ios): the scorecard's two halves stand side by side as two columns on a bone paper panel, numbers on the left with the Sum and the bonus line under them, combinations on the right with the Total under them; 24pt rows, 13pt names, 14pt numbers.
This is deliberately not the branded card's one tall column with a how-to-score column (LEGAL.md).
An open row on my card shows the kernel's preview for the dice on the tray, dimmed, and a tap on it scores there; a staged score shows in brass on a brass wash; the bonus line reads "-" until the kernel decides it.
In the compact drawer the card scrolls under the tray; expanded, it all shows.

T53 (ios): the table is pickemup's board inset (8 / 8 / 14 / 4) on foolish's felt, re-laid for dice from the top down: the other seats in one row across the top (44pt badges, each min(96pt, its share of the row), name over running total, the brass turn bar under the total), 8pt, the tray, 10pt, the status line with pickemup's 96 x 40 wood Roll pill 12pt from the trailing edge, 10pt, my card.
A tap on another seat's badge opens that seat's card read-only in a medium-detent sheet.
My own seat has no badge: my card is on the table.

T54 (ios): the bubble picture is foolish's 300 x 195 on the felt: the five dice at 44pt, 8pt apart, kept ones ringed and rerolling ones blank (T11), over one line of the kernel's caption at 13pt heavy (the same words also go in the MSMessage caption); a lobby bubble is the title and the numbered roster.

T55 (ios): a KEEP and a SCORE both collapse the drawer once staged, as a play does in Pick 'Em Up, because both want Send next.
After a KEEP is sent the extension stays up (no dismiss): the reroll tumbles in right there and the player chooses again.
After a SCORE is sent it dismisses as Pick 'Em Up's does after a turn.
Until the kernel's plan says how long a move rests, the settle before the collapse is 750ms.

T56 (ios): the Swift tests run on an iOS simulator when one is free, and otherwise as a Mac Catalyst test bundle (`DEST='platform=macOS,variant=Mac Catalyst' ios/scripts/mac_tests.sh unit`): TallybonesKit imports no Messages, so it builds for Catalyst with command-line overrides only, and a shared Mac's two simulator slots are no longer a reason for a test to go unrun.

T22 (bridge): one staged move at a time, since every bubble is one move: staging replaces it, cancel drops it, and `tb_api_mark_sent` (from `didStartSending`) makes it resident, which is when its roll first exists.
The lobby's join, leave and start change the resident at once, as Pick 'Em Up's do; in a live game `tb_api_stage_leave` stages a LEAVE bubble instead.

## BLOCKED

- The final name: a USPTO and App Store search for "Tallybones" is the owner's, before any store listing.
- App Store Connect, signing, TestFlight: not started, by design.
