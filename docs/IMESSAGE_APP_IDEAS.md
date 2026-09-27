# What else to build as a bubble

This is research and ideation, not a plan.
It asks one question: what other games and small utilities have a core loop that is naturally "one party acts, sends a bubble, everyone else sees it and reacts", with the bubble's payload alone enough to carry the whole thing, and no server anywhere.
Nothing here was checked against the App Store.
Every "crowded or open" call below is a best guess from general knowledge, and the owner's own searches are the real answer.

Written 2026-09-27, after reading the repo index, `docs/ARCHITECTURE_AS_A_PATTERN.md`, `docs/MOTION_BEFORE_FLOW.md`, `foolish/docs/IMESSAGE_GAME_DESIGN.md`, the UTTT and Werewolf READMEs, `werewolf/COMMON.md`, `shared/README.md` and `pickemup/LEGAL.md`.

---

## 1. The shape, stated precisely

The earlier products taught four things about what fits, and the ideas below are judged against them.

### 1.1 The payload budget

The repo's measured guardrail is about 1,000 base32 characters per URL, which is roughly 600 bytes of body.
UTTT fits its whole game in about 25 bytes, and Foolish fits a seed plus a 3-byte-per-action log in 224 to 314 bytes for a long game.
Nobody here has measured where Messages actually starts to hurt, so treat 600 bytes as the budget until someone does.
Anything that carries free text (item names, labels, phrases) or drawings spends that budget fast, and each idea below says so when it matters.

### 1.2 Who may write, and when

Strict turn order (UTTT, trick-taking, dice games) has no conflict channel at all.
Several people acting at once (Durak's throw-ins, a group answering a poll) needs Rule P, which picks one chain deterministically and loses the other's delta.
Rebase was retired on iOS, so in a game a racing move is simply dropped.

For utilities there is a better answer than for games, and it is worth writing down now.
When every writer only ever touches their own row (my availability, my vote, my expense), the rows commute, so a device that opens a bubble missing its own row can re-assert that row from its App Group cache without any risk of an illegal state.
That is the Rule M merge that `IMESSAGE_GAME_DESIGN.md` §7.7 rejected for games, and the reason it was rejected there (merged game moves are not independent) does not apply to per-person rows.
It still only converges as people keep opening bubbles, because an extension cannot read thread history, only the bubble it is shown.

### 1.3 Hidden information comes in four grades

| Grade | What it means | Examples below |
| --- | --- | --- |
| A - public | Nothing is secret; the payload can be read by anyone and it does not matter. | Yacht dice, snake draft, scheduling, ledger, Go/Hex |
| B - honor system | Secrets are derivable from the payload (a seed), and a player would need to decode base32 by hand to peek. Durak already ships this way. | Liar's dice, trick-taking, rummy, Secret Santa, word duel |
| C - commit and reveal | A secret is kept on the device (App Group) and only its SHA-256 goes in the bubble, revealed later. Costs an extra bubble per reveal, and the secret is lost if the player switches to their iPad or Mac mid-game, because the App Group does not sync. | Sealed bids, Goofspiel, a cheat-proof fleet hunt |
| D - wants a server | Live data, real time, a global leaderboard, fresh content, or a notification at a clock time. | Listed in section 4 as weak fits |

### 1.4 Randomness that cannot be re-rolled by cancelling

Dice and draws have one exploit specific to this transport: roll, dislike the result, cancel the staged bubble, reopen, roll again.
The fix is already in the kernel's toolbox: derive each roll from `deal_rng` seeded by the parent envelope's digest plus the seat and the roll index, so the same position always produces the same dice.
Cancelling then changes nothing, and the only way to influence a roll is to choose a different legal move before it, which is a cheat only a player with a decoder can attempt.

---

## 2. The ranked shortlist

Complexity is relative to what exists: UTTT is S, Pick 'Em Up is M, Werewolf is L, Foolish is XL.
"Reuse" names what already exists in `shared/` or a sibling product.

| # | Idea | Kind | Fit | Rights risk | Crowding (guess) | Size |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Liar's dice | Bluffing dice, 2-6 | Excellent | Low | Fairly open | S-M |
| 2 | Secret Santa draw | Utility, 3-12 | Excellent | Low | Open in Messages | S |
| 3 | Five-dice scoring game | Dice, 1-6 | Excellent | Medium (Yahtzee) | Medium | S |
| 4 | Trip ledger | Utility, 2-8 | Very good | Low | Medium | M |
| 5 | Trick-taking (Oh Hell first, then Spades/Hearts) | Cards, 3-7 | Very good | Low | Medium-crowded | M |
| 6 | Snake draft of anything | Party utility, 2-8 | Very good | Low | Open | S |
| 7 | Draw and guess, with stroke replay | Party, 2-8 | Good, size-bound | High on names | Crowded outside Messages | M-L |
| 8 | Word duel (secret word, counted hits) | Word, 2 | Very good | Medium (Wordle trade dress) | Crowded | S-M |
| 9 | Group availability grid | Utility, 2-8 | Very good | Low | Medium | S-M |
| 10 | Daily group puzzle with in-thread standings | Puzzle, 1-8 | Good | Low if named carefully | Medium | M |
| 11 | Gin rummy and cribbage | Cards, 2 | Very good | Low | Medium | M |
| 12 | Sealed-bid duel (Goofspiel) | Strategy, 2-4 | Good, showcases commit-reveal | Low | Open | S |
| 13 | Who's bringing what | Utility, 3-20 | Good | Low | Medium | S |
| 14 | Friendly wager book | Utility, 2-8 | Good | Medium (App Review wording) | Open | S |
| 15 | Pencil-and-paper abstracts (Hex, Go 9x9, Nine Men's Morris) | Abstract, 2 | Excellent | Low | Mixed | S each |

The rest of this section takes each in turn.

### 2.1 Liar's dice

Each player has five hidden dice, and players take turns raising a bid ("at least seven 4s on the table") until someone calls it and the loser drops a die.
It is strictly sequential, so there is no conflict channel, and the whole state is a round seed plus a short bid log, far under budget.
Hidden dice are grade B, exactly Durak's model, and the dice for each round can be derived from the chain (section 1.4) so a round cannot be re-rolled.
Reuse is high: `deal_rng`, the lobby and seat identity from Werewolf, per-seat masking from `view.c`/`ww_view.c`, and the felt and wood textures.
The caption writes itself, like UTTT's does: "Sam bid eight 5s - your call".
Rights: the traditional game is public domain (Dudo, Perudo, Liar's dice); "Perudo" is a registered mark and must be avoided, as must anything pirate-themed that echoes the Disney films.
A USPTO check on "Liar's Dice" as a name is still worthwhile.
Crowding guess: several standalone apps exist, but I do not recall a strong one inside Messages, and I do not believe GamePigeon has it.

### 2.2 Secret Santa draw

Everyone in a family or office thread joins, the organizer sets a budget and optional exclusions (couples should not draw each other), and the kernel draws a derangement from a seed.
Each device shows only its own assignee, which is Werewolf's role-hiding problem with the stakes lowered from life and death to socks.
Grade B is acceptable for almost every group, and the fair-deal commit (`IMESSAGE_GAME_DESIGN.md` §15) stops the organizer from grinding the seed to pick their own recipient.
Wish lists as short free text are the one size risk, so keep each to a few words, or keep them out of the payload and let people send them as ordinary messages.
It is seasonal and deeply viral: one person installs it and a whole family group has to.
Crowding guess: Elfster and DrawNames own this outside Messages, and inside Messages I suspect it is thin.

### 2.3 Five-dice scoring game

Roll five dice up to three times, keep what you like, score a category, pass the scorecard on.
All state is public (grade A) and turn-ordered, a full game is 13 turns per player, and the whole scorecard for six players is well under 100 bytes.
Section 1.4 matters most here, because the three rolls are exactly what a player would try to re-roll.
This is the lowest-risk fun idea on the list and a natural companion to Liar's dice, sharing the dice renderer and its motion grid.
Rights: the mechanic is the public domain Yacht family (also Generala and Poker Dice), but "Yahtzee" is Hasbro's, "Kniffel" is Schmidt Spiele's, and the scorecard's layout and the shout word are the trade dress to stay away from, in the spirit of `pickemup/LEGAL.md`.
"Full house" and "straight" are poker terms and generic.
Farkle is a second variant worth a trademark check before using the name.
Crowding guess: medium; Scopely's buddies-branded dice apps are big outside Messages, and there are probably small ones inside.

### 2.4 Trip ledger

A shared log of who paid what for whom on a trip or in a household, with the kernel computing balances and the fewest payments that settle everyone up.
It is the purest utility fit on the list: per-person entries commute (section 1.2), all state is public, and the settlement algorithm is real deterministic logic that earns a C kernel.
The size risk is the log, since 100 expenses with labels do not fit in 600 bytes.
The answer is compaction: the bubble carries the running balances (8 seats at 4 bytes each) plus only the last dozen entries, and a "settled" bubble resets the log.
No money moves, which keeps App Review simple.
Crowding guess: medium to high; Splitwise and Tricount own the category as full apps, and Apple has been adding Apple Cash requests to group threads, so check what the OS already does before building.
The angle that is still open is "no account, lives in the trip's thread, gone when the trip is".

### 2.5 Trick-taking: Oh Hell first, then Spades and Hearts

The trick-taking family is enormous and public domain, and it reuses almost everything Foolish built for cards: dealing from a seed, per-seat hands, the card renderer, drag to play, and the lobby.
Oh Hell (also sold as Up and Down the River, Blackout or Nomination Whist) comes first because it plays with 3 to 7, its early hands are one or two cards, and bidding exactly makes every trick matter.
Spades and Euchre are culturally huge in parts of the US, but they need exactly four players in partnerships, which is harder to gather in a thread.
The real risk is pacing, not fit: 4 players times 13 tricks is 52 bubbles per hand, and a game to 500 is hundreds of bubbles.
Messages' session replacement collapses those into one live bubble, which is exactly what Foolish relies on, but motion design has to make a trick completing feel like an event and not a chore.
Rights: all low, but avoid Wizard's special cards (a trademarked commercial game) and any branded name.
Crowding guess: Spades and Hearts are crowded as standalone apps and likely present in Messages; Oh Hell in Messages feels open.

### 2.6 Snake draft of anything

Someone lists 20 to 40 items ("90s movies", "pizza toppings", "best Bond"), players draft in snake order, and the group argues about who drafted best.
The "draft anything" format is a popular podcast and group-chat bit, and it is perfectly turn-ordered with public state.
It doubles as a real utility: dividing chores, picking fantasy league order, or splitting shared books.
Size is the only concern because items are free text, so cap item length and count and measure the worst case against the budget.
An optional end-of-draft vote on the best roster uses per-person rows, which commute.
Crowding guess: open; I cannot name an iMessage app that does this.

### 2.7 Draw and guess, with stroke replay

One player draws a secret word, the others watch the strokes replay and guess.
The magic is the replay: the drawing unfolds line by line in the order it was made, which is what made the genre take off, and the kernel pen UTTT already uses for rough strokes is a head start.
The size is the problem, not the fit: quantized, delta-coded strokes are perhaps 300 to 800 bytes for a sketch, which is at or over budget, so this needs a codec measured against real drawings before anything else.
A telephone variant (write a phrase, the next person draws it, the next describes the drawing) is the funniest possible group-thread game, but its final reveal needs the whole chain of drawings in one payload, which is far over budget unless each device caches the entries it has seen.
Rights: high on names and presentation only; Pictionary is Mattel's, Draw Something is Zynga's, Telestrations and Gartic Phone are marks, and the public domain versions are the parlor game "telephone pictionary".
Crowding guess: crowded as standalone apps; inside Messages I suspect a few exist, and the stroke replay is where a new one could stand out.

### 2.8 Word duel

Each player picks a secret five-letter word for the other; each guess is answered with how many letters are right and how many are in the right place.
Two words and a few dozen guesses are well under budget, the secret is grade B (the setter is your friend), and the game can run over days like correspondence chess.
It needs a dictionary compiled into the kernel, which is the only real cost.
Rights: the mechanic is the public domain Bulls and Cows, also played as Jotto; Wordle is a New York Times mark, and the NYT has sent takedowns to clones, so the green, yellow and grey tile grid is the thing to avoid.
Answering with counts (Bulls and Cows) rather than per-letter colours is both the classic game and the safe distance from that trade dress.
Mastermind is also a mark, so a colour-peg variant needs its own name.
Crowding guess: crowded; Wordle-like games are everywhere, and I believe GamePigeon has word games.

### 2.9 Group availability grid

A week of time slots; each person taps the ones they can make, and the bubble shows the heat map and the best slot.
Each person's answer is a 100-bit row, so eight people fit in about 100 bytes, and rows commute (section 1.2), which makes concurrent answers safe to merge.
It is exactly the shape of the problem: an async group decision that already happens in the thread, badly, by text.
Crowding guess: medium; Doodle and When2meet own it outside Messages, and Apple's own polls in Messages answer single-choice questions, not a grid.

### 2.10 Daily group puzzle with in-thread standings

A seed derived from the date gives every device the same puzzle each day with no server, the kernel generates it (sudoku, nonogram, a word puzzle), and each player's bubble carries their solve time and the group's running streaks.
It is the Wordle share-grid habit, except that the standings live in the thread's latest bubble and every result row commutes.
Grade B cheating is possible and does not matter among friends.
Rights: avoid the NYT puzzle names (Connections, Spelling Bee, Strands), Nintendo's "Picross" (say nonogram), and KenKen (a mark); sudoku is generic outside Japan.
Crowding guess: medium; lots of daily-puzzle apps, very few that keep score inside the group thread.

### 2.11 Gin rummy and cribbage

Two classic two-player card games with long histories of being played by mail and over the kitchen table for years at a time.
Both reuse Foolish's card stack directly, both are strictly turn-ordered, and both are grade B.
Cribbage's pegging board is a rich motion subject, and its audience skews older, which is a group that already lives in iMessage.
Rights: public domain.
Crowding guess: medium; strong standalone apps exist for both, and GamePigeon has no cribbage as far as I recall.

### 2.12 Sealed-bid duel (Goofspiel)

Each player holds cards 1 to 13; a prize card is flipped, both bid a card secretly and simultaneously, and the higher bid takes the prize.
It is the smallest possible showcase of commit and reveal (grade C): each player's bid goes into the bubble as a hash, and the reveal follows in the next bubble.
Getting that pattern right once, in a tiny game, makes every later simultaneous-choice design cheaper, including a cheat-proof fleet hunt and a true rock-paper-scissors.
Rights: public domain, from the 1950s game-theory literature.
Crowding guess: open; it is obscure, which is both the opportunity and the risk.

### 2.13 Who's bringing what

The host lists what a party needs, and people claim items by tapping them, with the bubble showing what is still missing.
Claims on different items commute, and two people racing for the same item is exactly one Rule P decision.
Free-text item names are the size budget again, so cap them.
Crowding guess: medium; SignUpGenius and Partiful live outside Messages, and there are probably small potluck apps inside it.

### 2.14 Friendly wager book

"Loser buys dinner if the Mets make the playoffs": both sides accept in a bubble, and later either side marks it won and the other confirms.
A running tally of wagers between two friends lives in the latest bubble, public and tiny.
It needs no live data because the people resolve it themselves, which is also what keeps it honest to build without a server.
Rights: none on the mechanic, but the store copy must never suggest real-money gambling (App Store guideline 5.3); words like "dare", "promise" or "stakes" are safer than "bet".
Crowding guess: open.

### 2.15 Pencil-and-paper abstracts: Hex, Go 9x9, Nine Men's Morris

UTTT proved the pencil-and-paper look, and these three are public domain, two-player, strictly alternating, and each fits in a few dozen bytes.
Hex is the elegant choice: no draws, one rule, and a board that looks beautiful in the rough pen.
Go has a deep correspondence-play tradition, and a 9x9 or 13x13 board is a natural fit, though it needs scoring logic that is more work than it looks.
Nine Men's Morris is thousands of years old and has a satisfying motion grid (place, then slide, then fly).
Built as one app with a picker, they share one renderer.
Crowding guess: mixed; GamePigeon covers the famous abstracts (chess, checkers, four in a row, gomoku, reversi, dots and boxes, mancala, as far as I recall), but Hex, Go and Morris feel open inside Messages.

---

## 3. What already exists, and what that means for the choice

GamePigeon is the incumbent iMessage game app and has been for years, and from memory its catalog includes pool, sea battle, word games (anagrams, word hunt), cup pong, darts, basketball, mini golf, archery, tanks, four in a row, gomoku, chess, checkers, reversi, dots and boxes, mancala, and a crazy-eights game.
Please verify that list; it is the single most important thing to check, because anything it already does well is crowded by default.
If its crazy-eights game is still there, Pick 'Em Up's competition is GamePigeon, not only Mattel.
The ideas above were chosen partly to avoid its catalog: dice bluffing, dice scoring, trick-taking, rummy, drafts and utilities are all outside it as far as I remember.

---

## 4. Weak fits, and why

| Idea | Why it is weak here |
| --- | --- |
| Simple polls | Apple added polls to Messages itself (iOS 26, to verify); a single-choice poll is now an OS feature. |
| Countdowns to an event | A bubble is a static picture; the live-layout route is a dead end in this repo already (memory: MSMessageLiveLayout breaks tap-to-open). |
| Shared grocery or to-do lists | They need to live outside the thread (on the lock screen, in Reminders), and shared Reminders lists already do this. |
| Trivia | Good questions go stale and need a content pipeline; a static bank is fine for a while and then feels old. Open Trivia DB is CC BY-SA if it is ever tried. |
| Sea battle / fleet hunt | Perfect shape (and commit-reveal would make it cheat-proof, which GamePigeon's is not), but GamePigeon owns it, and "Battleship" is Hasbro's. |
| Crossword tile game | Perfect correspondence shape, but Scrabble's board layout and tile values are defended trade dress (Hasbro and Mattel sued Scrabulous), and Words With Friends is huge. |
| One-word-clue team games | Codenames and Just One are marks from active publishers, and the genre's recognisable mechanics sit too close to them. |
| Sports pick'em | Needs live scores and schedules, which means a server. |
| Anything real time or dexterity-based | The extension runs only while someone is looking at it; there is no "now". |

---

## 5. Search terms for the owner

These are for the App Store inside Messages (the store reached from the apps drawer), and also for the main App Store with "iMessage" appended, since many extensions are listed there.
Example app names are prior art to look at, not to copy, and each is from memory, so a name may have changed.

**Incumbent check, before any of the below:** GamePigeon, Game Pigeon, iMessage games, games for iMessage, multiplayer iMessage.

1. **Liar's dice:** liar's dice, liars dice, bluff dice, dice bluff, Perudo, Dudo, pirate dice. Prior art: Perudo-branded apps, any "Liar's Dice" app.
2. **Secret Santa:** secret santa, gift exchange, name draw, draw names, Kris Kringle, white elephant, Secret Santa iMessage. Prior art: Elfster, DrawNames, Secret Santa Organizer apps.
3. **Five-dice scoring:** yahtzee, yacht dice, dice poker, five dice, Generala, Kniffel, Farkle, dice with friends. Prior art: Scopely's Yahtzee With Buddies and Dice With Buddies.
4. **Trip ledger:** split bill, split expenses, IOU, who owes who, trip expenses, group expenses, settle up. Prior art: Splitwise, Tricount, Settle Up; also check Apple Cash in group threads.
5. **Trick-taking:** spades, hearts, euchre, oh hell, up and down the river, whist, bid whist, trick taking. Prior art: Spades Plus, Spades Royale, Hearts card game apps.
6. **Snake draft:** draft, snake draft, draft anything, pick order, fantasy draft, draft game. Prior art: none I can name, which is itself the signal to check.
7. **Draw and guess:** draw and guess, drawing game, doodle, pictionary, draw something, telephone game, telestrations, gartic. Prior art: Draw Something, Gartic Phone.
8. **Word duel:** wordle, word guess, five letter word, word duel, jotto, bulls and cows, mastermind, code breaker, hangman. Prior art: the many Wordle-style multiplayer apps, GamePigeon's word games.
9. **Availability grid:** schedule, find a time, availability, when2meet, doodle poll, meeting poll, group calendar, plan. Prior art: Doodle, When2meet, Howbout.
10. **Daily group puzzle:** daily puzzle, sudoku, nonogram, daily challenge, puzzle with friends, leaderboard friends. Prior art: Sudoku.com, the NYT Games app.
11. **Gin rummy and cribbage:** gin rummy, rummy, rummy 500, cribbage, crib, card games two player. Prior art: Cribbage Pro, Gin Rummy Plus.
12. **Sealed-bid duel:** goofspiel, bidding game, sealed bid, rock paper scissors, simultaneous, mind game. Prior art: rock-paper-scissors apps.
13. **Who's bringing what:** potluck, sign up sheet, who's bringing, party planner, RSVP, bring list. Prior art: SignUpGenius, Partiful, Perfect Potluck.
14. **Friendly wager book:** bet with friends, friendly bet, wager, dare, prediction, bet tracker, IOU bet. Prior art: none I can name confidently.
15. **Pencil-and-paper abstracts:** go, baduk, weiqi, hex, hex board game, nine men's morris, mills, pencil and paper games, connection game. Prior art: GamePigeon's abstracts, standalone Go apps.

---

## 6. If only one gets built next

Liar's dice, with the five-dice scoring game as the second mode in the same app.
It is the best ratio of fun to effort on this list, it reuses the lobby, seats, masking and `deal_rng` that already exist, it sits outside GamePigeon's catalog as far as I know, its rights story is the cleanest of the dice games, and it fixes the re-roll exploit (section 1.4) once for every dice game after it.
The Secret Santa draw is the best utility and the most viral, but it is seasonal, so it is worth timing to ship in November.

## 7. Owner's search results, checked against GamePigeon's real catalog (2026-09-27)

The owner searched the store himself and then pulled GamePigeon's own in-store game list directly, rather than relying on memory.
GamePigeon's full catalog, as listed in its own App Store page: 8-Ball, Mini Golf, Basketball, Cup Pong, Archery, Darts, Tanks, Sea Battle, Anagrams, Mancala, Knockout, Shuffleboard, Chess, Checkers, Four in a Row, Gomoku, Reversi, 20 Questions, Dots and Boxes, 9-Ball, Word Hunt, Word Bites, Filler, Crazy 8!.

What that confirms:

- **Liar's dice:** not in GamePigeon, and the owner's own search found nothing for "liar's dice," "perudo" or "dudo," only an unrelated app called "Bluffs."
  Open.
- **Five-dice scoring:** not in GamePigeon, and nothing turned up for "yahtzee" in the owner's search either.
  Open.
- **Trick-taking / Spades:** not in GamePigeon, and the owner's search turned up little for "spades."
  Open, and confirmed as the strongest still-unbuilt option after the two dice games.
- **Crazy 8!** is in GamePigeon by that exact name, which reads as plain Crazy Eights (no wilds, no reverse, no skip, no draw stacking implied by the name) rather than a UNO-style game.
  This does not appear to compete with Pick 'Em Up's fuller ruleset, but the real Crazy 8! rules are worth a direct check before relying on that read.
- **Sea Battle** confirms GamePigeon already owns that space, as flagged in section 4.
- Chess, Checkers, Four in a Row, Gomoku, Reversi, Mancala and Dots and Boxes cover much of the pencil-and-paper abstracts space (section 2.15, "Pencil-and-paper abstracts").
  Hex and Nine Men's Morris specifically are still not there, but that section is more crowded than it looked from memory alone.

**Decided, 2026-09-27:** Liar's dice is being built now, under the working name **Chui Niu** (owner's choice; this is the game's own generic Chinese name, 吹牛, a folk bluffing dice game, not a trademark).
The five-dice scoring game is being built now too, copyright-free (not named or scored as Yahtzee), under its own working title to be chosen during the build.
Both skip the full design-phase ceremony this document and Pick 'Em Up's docs otherwise call for; the goal for both is a running proof of concept, not a finished product.

**Spades / trick-taking is queued, not started.**
It is confirmed open against GamePigeon's real catalog and is the next thing to design in detail once Chui Niu and the dice-scoring proof of concept are further along.
