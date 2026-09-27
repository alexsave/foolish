# Pick 'Em Up - rules and kernel design

Status: design only, nothing is built.
Written 2026-09-26 against `pickemup/README.md`, `pickemup/LEGAL.md` and `pickemup/docs/UI.html` (the surface study of 20 September 2026).
Every rule in this file is a kernel rule: it lives in C, in `pickemup/c/`, and every host (the iMessage extension, a replay page) asks the kernel instead of re-deciding it.

The name is a placeholder (README), so every user-facing string below says "Pick 'Em Up" only where it must, and the Words section keeps the game name in one key.

The trademark rules from `LEGAL.md` bind this document too.
The Mattel mark is never written in any shipped string, the call-out word is our own, suits are shape AND colour, and nothing here describes the game as "like" anything.

Where this file says "the sister kernels" it means foolish (`c/`, the Durak kernel at the repo root) and UTTT (`uttt/c/`).
Every convention borrowed from one of them is cited, so a reader can check the original.

---

## 1. Rules of play

### 1.1 The cards

The deck has 104 cards in four suits and one wild group.

A suit is a shape AND a colour, always both, never one alone:

| Suit | Shape | Colour |
|---|---|---|
| 0 | circle | teal |
| 1 | triangle | amber |
| 2 | square | violet |
| 3 | diamond | slate |

Each suit has 24 cards:

- two each of the numbers 1 to 9 (18 cards),
- two Skips,
- two Reverses,
- two +2s.

There are 8 wild cards with no suit:

- four Wilds,
- four Wild +4s.

4 x 24 + 8 = 104.
There is no 0 card (DECISION D1).

A card's **rank** is its number, or its symbol for an action card (Skip, Reverse, +2).
Two cards "match" when they share a suit or share a rank.

### 1.2 Players and seats

2 to 8 players, one per seat.
Seats are numbered 0 to n-1 in the order people joined the lobby (section 4.6).
Seat 0 is the **dealer**.
**Clockwise** means increasing seat number, wrapping from n-1 to 0; anticlockwise is the reverse.
Play starts clockwise.

### 1.3 The table

- The **stack** is the face-up pile in the centre of the table.
  Its top card is the one you match.
- The **deck** is the face-down pile to the left of the stack.
  You draw from it.
- There is no other pile.
  Cards leave play only by being played onto the stack, and come back only through a reshuffle (1.9).

### 1.4 The deal

1. The deck is shuffled from the game's seed (3.5).
2. Seven cards are dealt to every player, **one card at a time, round-robin**: one to the first player (seat 1), one to seat 2, and so on round to the dealer (seat 0), then again from seat 1, seven times round.
   Never a pack of seven at once.
3. The top card of the deck is turned face up to start the stack.
   If it is a number card, it is the start card.
   If it is anything else (Skip, Reverse, +2, Wild, Wild +4) it is put face up at the **bottom** of the deck, where it stays in the deck's order, and the next card is turned.
   Repeat until a number card is turned (DECISION D14).
4. The live suit is the start card's suit.
5. The first player is seat 1, the player to the dealer's left (DECISION D19).
   At 2 players that is the player who is not the dealer.

### 1.5 Your turn

On your turn you do any number of **draws**, then exactly one **ending**, which is either a **play** or a **pass**.

- **Draw.** Take the top card of the deck into your hand.
  You may draw whenever it is your turn, whether or not you already hold a card you could play (DECISION D6), as many times as there are cards to draw.
  If the deck is empty and the stack holds more than its top card, the stack is reshuffled into the deck first (1.9).
  Drawing is never automatic: the game never draws for you, except for the penalties in 1.6 and 1.8.
- **Play.** Put one card from your hand on the stack.
  It must match the top card by suit (the live suit, when the top card is a wild) or by rank, or be a Wild or a Wild +4, which may be played on anything at any time (DECISION D11).
  When you play a Wild or Wild +4 you choose the new live suit as part of the same play; a wild with no suit chosen is not a play (DECISION D17).
  Your turn ends.
- **Pass.** End your turn without playing.
  You may pass only if you drew at least one card this turn, or if there is nothing left to draw and nothing in your hand you could play (DECISION D10).

A player never has to play: after one draw they may keep everything and pass.

### 1.6 What the action cards do

After a play, the turn moves one seat in the current direction, except:

- **Skip.** The next player loses their turn; play moves two seats.
- **Reverse.** The direction flips, and the turn moves one seat in the new direction.
  **At 2 players a Reverse is a Skip** (DECISION D13): the direction does not change and the player who played it goes again.
- **+2.** The next player draws two cards (automatically, from the deck) and loses their turn.
- **Wild.** Choose the live suit.
  Play moves one seat.
- **Wild +4.** Choose the live suit.
  The next player draws four cards and loses their turn.

There is no stacking: a +2 or +4 cannot be answered with another +2 or +4 to pass the penalty on (DECISION D12).
There is no challenge of a Wild +4 (DECISION D11).

At 2 players, a Skip, a +2, a Wild +4 and a Reverse all give the player who played it another turn straight away.

A penalty draws as many cards as it names, reshuffling the stack in if the deck runs out; if even that cannot supply them all, the player draws what there is and the rest is forgiven (DECISION D15).

### 1.7 Going out

The first player to play the last card from their hand wins, and the game is over at once.
A last card's action does nothing (nobody draws, nobody is skipped), and a last-card wild needs no suit.
One hand is one game; there is no scoring across hands (DECISION D20).

### 1.8 The last card, and getting caught

The call-out word is **"Last card!"** (DECISION D2).

- A player who plays a card that leaves them holding exactly one card is **exposed**.
- They may **not** say "Last card!" in the same message as that play.
  They must send the play first; the others then get a chance to notice (DECISION D3).
- From the moment that message is sent, the exposed player may say "Last card!" in any message they send, including a message that contains nothing else, sent when it is not their turn.
  Once said, they are safe: their seat shows the LAST stamp for as long as they hold that one card.
- While a player is exposed, **any other player** may catch them by tapping that player's card fan, which puts "Caught you!" into the message they are composing, in or out of turn (DECISION D5).
- **The window** closes at the end of the next message that completes a turn (anyone's turn, including the exposed player's own).
  A message that only says "Last card!" or only catches somebody does not close it.
  An exposed player nobody caught before the window closed got away with it and is safe (DECISION D4).
- **Caught.** If the player called was exposed when the catcher's message began, they draw **two** cards.
  If they were not exposed (they had said it, or they did not hold exactly one card), the catcher called wrong and draws **one** card (DECISION D5b).
- One message may catch at most one player, and nobody may catch a player showing the LAST stamp (DECISION D5c).
- A player who draws, for any reason, stops being exposed and loses the LAST stamp, because they no longer hold one card.

Card counts are never shown on anyone's hand (DECISION D22).
Noticing that somebody is down to one card is a matter of watching what they played and drew.

### 1.9 The reshuffle

When a draw (a player's own draw, or a penalty) finds the deck empty and the stack holds two or more cards, every stack card except the top one is shuffled to become the new deck, and the draw goes on.
The top card stays where it is, with its live suit.
The order of a reshuffle is fixed by the seed and the number of reshuffles so far (3.5), so every phone agrees without anything extra on the wire.

### 1.10 When nothing can move

If the deck is empty and the stack is only its top card, nobody can draw.
A player who then has nothing to play must pass.
If every player passes in a row without drawing (n bare passes in a row), the table is stuck and the game ends: the player holding the fewest cards wins, and a tie goes to whichever tied player is next to move from the seat that would have moved next (DECISION D16).

### 1.11 The long-game stop

A game that reaches 1,500 turn actions (draws, plays and passes) or 750 messages ends at once, the same way as a stuck table: fewest cards wins (DECISION D23).
This exists only so that the whole game always fits in one message (section 4); a real game should never see it.

### 1.12 What a message may carry

Everything one player does between two sends is one message (a bubble).
A message has one sender and holds, in this order:

1. optionally "Last card!", if the sender is exposed;
2. optionally one "Caught you!", naming one other player;
3. optionally the sender's turn, if it is their turn: any number of draws, then a play or a pass.
   If that play gives the sender the next turn again (only possible at 2 players), the message may go on with that next turn too, and so on (DECISION D7).
   A play that leaves the sender holding one card ends the message.

A message must hold at least one of the three.
A message that holds turn actions ends with a play or a pass; nobody sends half a turn.

---

## 2. DECISION log

Every choice made on the owner's behalf.
Each can be vetoed on its own; where one depends on another, it says so.

**DECISION D1: the deck is 104 cards: per suit two each of 1-9, two Skip, two Reverse, two +2; plus four Wild and four Wild +4.**
Alternative: the familiar 108 (one 0 and two each of 1-9 per suit, the same actions and wilds).
Why: `UI.html` only ever draws 1-9, the 0 has no rule of its own, and every deliberate difference from the protected product's card list is cheap evidence of independent design (`LEGAL.md` "Always" item 10). 104 also keeps every card id under 128.
Recommendation confidence: medium.

**DECISION D2: the call-out word is "Last card!".
Proposed: "Last card!", "Down to one!", "Final card!".**
Alternative: "Down to one!" (warmer, more of a boast) or "Final card!".
Why: it is plain English, it is what the public-domain shedding game family has long called out, it shares nothing with the mark, it matches the LAST stamp `UI.html` already draws, and it is short enough for a stamp and a button.
Recommendation confidence: medium.

**DECISION D3: a player may say "Last card!" only in a message after the one whose play left them on one card, never in that message.**
Alternative: allow it in the same message (removes the mechanic) or only on the player's next turn (makes saying it impossible before the next player acts).
Why: the owner's rule, and the only version where the others get a window.
Recommendation confidence: high.

**DECISION D4: the window closes at the end of the next message that completes a turn; "Last card!"-only and "Caught you!"-only messages do not close it.**
Alternative: `README.md`'s "one bubble is the window" - the very next message, whatever it holds.
Why: with one-bubble, a wrong "Caught you!" on one player (or anyone's say-it) silently closes the window on a different exposed player, which is a rule nobody could explain.
Tying the window to the next completed turn keeps README's intent (it needs no clock, it cannot be lost to a slow phone, it is usually exactly one bubble) and makes the answer the same whoever happens to send first.
Inside that turn's message a catch is always in time, because the play or pass is always its last action.
Recommendation confidence: medium.

**DECISION D5: any other seated player may catch an exposed player, in or out of turn, by a message of their own; the exposed player may also say it out of turn.**
Alternative: only the next player may catch, and only inside their turn.
Why: the owner's rule (tap the fan), and at 3+ players the most attentive player should win the moment, not the one who happens to be next.
Recommendation confidence: high.

**DECISION D5b: a right catch costs the caught player 2 cards; a wrong catch costs the catcher 1 card.**
Alternative: symmetric 2 and 2, or 4 for a right catch.
Why: counts are hidden (D22), so a catch is often a guess, and the call-out is the best moment in the game; a one-card miss makes guessing a real gamble without making it pointless.
A wrong catch MUST cost something, because catching is legal on anyone at any time (so that the button itself reveals nothing, 3.7) and a free catch would be spammed.
Recommendation confidence: medium.

**DECISION D5c: at most one "Caught you!" per message, and never on a seat showing the LAST stamp.**
Alternative: several catches per message; allow catching a stamped seat (always a miss).
Why: one catch keeps a message's story to one line, and the LAST stamp is public, so refusing a catch there reveals nothing and saves a pointless penalty.
Recommendation confidence: high.

**DECISION D5d: a catch is judged against the table as it stood when the catcher's message began, and its penalty is dealt at the end of that message, after everything else in it.**
Alternative: judge and deal it at the point in the message where the fan was tapped.
Why: this makes a catch a fact about the whole message rather than a moment inside it, so it can be tapped at any point while composing and undone freely, and nothing the catcher does later in the same message can depend on (or reveal) whether it hit.
It also makes its place on the wire canonical (4.4).
Without it, a miss would put a card in the catcher's hand at stage time and tell them the answer before they send.
Recommendation confidence: high.

**DECISION D6: a player may draw on their turn even when they hold a playable card, as often as there are cards to draw.**
Alternative: draw only when nothing plays (the tournament rule), or draw exactly one.
Why: the owner wants deck-tapping to be free, many times, up to a reshuffle; the kernel forbidding it would take away the comic "fishing" turn and gains nothing, because drawing only ever hurts the drawer.
Recommendation confidence: high.

**DECISION D7: at 2 players a message may carry several consecutive turns by its sender when a play hands the turn straight back (Skip, Reverse, +2, Wild +4); a play that leaves the sender on one card ends the message regardless.**
Alternative: exactly one turn per message, always.
Why: about 90% of games in the sister product are 2 players, and making the player send a bubble just to be told "your turn again" doubles the round trips of every action card.
The one-card clause keeps D3 intact: without it, a player could play to one card and straight out in one message, and nobody would ever get a window.
Recommendation confidence: medium.

**DECISION D8: a draw is committed the moment it is drawn; plays, "Last card!" and "Caught you!" can be un-staged, draws cannot.
The draft has a floor at its last draw, and undo never goes below it.**
Alternative: everything undoable until Send.
Why: a draw shows the drawer the next card of a deck that is the same on every phone, so drawing, looking and undoing is a free peek at the future (foolish's held-settlement argument, `docs/IMESSAGE_GAME_DESIGN.md` section 11.5).
A play above the floor reveals nothing and stays undoable ("card flies home", `UI.html` Undo row).
Recommendation confidence: high.

**DECISION D9: when the sender deletes a staged bubble with Messages' x, the host rebuilds the draft back to its floor, not to the parent; the drawn cards stay in hand.**
Alternative: the x throws the whole draft away.
Why: otherwise the x is a one-tap version of the peek D8 forbids.
The floor lives in the host's memory only (foolish retired every durable pending ledger, `IMESSAGE_GAME_DESIGN.md` section 7.4); a killed extension forgets it, which is accepted casual trust, the same as foolish's seed-in-the-payload.
Recommendation confidence: medium.

**DECISION D10: PASS exists as an explicit action, legal after at least one draw this turn, or when nothing can be drawn and nothing in hand plays.**
Alternative: no pass (draw until you can play); or pass freely.
Why: the owner asked for an explicit pass, drawing is manual so the kernel cannot "draw until playable" for you, and a free pass would let a player sit still without cost.
Recommendation confidence: high.

**DECISION D11: a Wild +4 may be played at any time, on anything; there is no challenge.**
Alternative: legal only when the player holds no card of the live suit (the kernel could enforce this exactly, since it sees the hand).
Why: the owner recommended no challenge, and without a challenge the restriction is only a rule the kernel would enforce silently, with a dimmed card the player cannot understand.
Simpler to say both wilds are always playable.
Recommendation confidence: medium.

**DECISION D12: no stacking of +2 or +4.**
Alternative: stacking as a house rule.
Why: the owner's recommendation, and structurally it is free here: a penalty is dealt as the settlement of the play that caused it (D15), so there is no moment at which the victim could answer.
Recommendation confidence: high.

**DECISION D13: at 2 players a Reverse acts as a Skip and does not flip the direction; the direction indicator is not drawn at 2 players.**
Alternative: flip the direction as well (no effect on play at 2).
Why: at 2 players the direction means nothing, and a word that turns for no reason is a lie on the board.
`UI.html` bubble 02 already says the caption should state the outcome and the kernel owns the rule.
Recommendation confidence: high.

**DECISION D14: the start card is always a number; an action or wild turned at the start goes face up to the bottom of the deck and the next card is turned.**
Alternative: the classic start-card effects (first player skipped, draws two, dealer picks the suit, and so on), or bury only the Wild +4.
Why: no player takes a penalty before anyone has played, no wild colour has to be picked by somebody at the start, and the deal needs no special first turn.
The buried cards are public (they were face up), which is harmless and gives the deal a comedic beat.
Recommendation confidence: high.

**DECISION D15: penalty draws (+2, +4, a right catch, a wrong catch) are automatic, not tapped; if the supply runs out, the rest is forgiven.**
Alternative: the victim must tap the deck N times as a forced turn.
Why: a forced-draw turn is a round trip with no decision in it, and `UI.html` already draws the +2 cards landing on the next seat at Send (channel B).
The owner's "drawing is manual" is about choosing to draw.
Recommendation confidence: high.

**DECISION D16: deck and stack both exhausted (nothing drawable): a player with nothing to play must pass; n bare passes in a row end the game; fewest cards wins, ties to the tied player next to move.**
Alternative: a stuck table is a draw with no winner.
Why: iMessage games want a winner on the last bubble; with nothing drawable and every player passing, the position can never change again, so this is exact deadlock detection, not a timer.
Recommendation confidence: medium.

**DECISION D17: a wild has no default suit.
Dismissing the suit picker cancels the play and the card stays in the hand; the only wild with no suit is a last card, which needs none.**
Alternative: default to the suit the player holds most of, or keep the live suit.
Why: `UI.html`'s own rule, "the move is not a move until the suit exists"; a default is a decision the player did not make, and the wire needs a suit either way.
Recommendation confidence: high.

**DECISION D18: the direction indicator is the word "clockwise" or "anticlockwise" in the freed top-right corner, as `UI.html` draws it; the kernel exposes `dir` and the indicator is hidden at 2 players (D13).**
Alternative: an arrow.
Why: `UI.html` decided "direction is a word, not an arrow", and the Reverse glyph (two opposed solid triangles) already carries the shape.
Recommendation confidence: high.

**DECISION D19: seat 0 is the dealer; seat 1 plays first; the deal starts at seat 1 and the dealer is dealt last in each round.**
Alternative: the first player chosen from the seed.
Why: it is the physical convention ("left of the dealer"), there is nothing to grind, and the person who sent the invitation does not also get the first move.
A rematch is a new lobby, so whoever asks for it deals.
Recommendation confidence: high.

**DECISION D20: one hand is one game; the first player out wins; no points, no rounds.**
Alternative: play to 500 points across hands.
Why: the owner's recommendation, and the same shape as foolish and UTTT: one bubble chain is one game, and the end card is the payoff.
Recommendation confidence: high.

**DECISION D21: the seed is 32 bytes from the phone's secure random at New game; the game's identity is the first 8 bytes of SHA-256("pickemup.game.1|" || seed); the initial shuffle reads ChaCha keystream from block 0, and reshuffle r (r = 1, 2, ...) reads from block r * 2^32.**
Alternative: UTTT's 4-byte send-time seed; a separate 8-byte game id like foolish's.
Why: the whole future of the deck comes from the seed, so it gets foolish's full 256-bit ChaCha key (`shared/c/deal_rng.h`), not 2^32 deals.
Deriving the id from the seed saves 8 bytes and removes a field that could disagree with it.
`deal_rng_seed_at` makes each reshuffle its own disjoint stretch of the one keystream in O(1), with no extra hashing and no state to carry.
Recommendation confidence: high.

**DECISION D22: no card counts anywhere a player can see them during play: not on the seat badges, not in captions, not in the masked view.
The deck count ("27 left") IS shown.
All hands are shown face up once the game is over.**
Alternative: hide the deck count too.
Why: the owner's rule for hands.
The deck is public in the physical game, and its count does not reveal any one player's hand.
Leaving other seats' counts out of the masked view (3.7) means no renderer can show one by accident.
Recommendation confidence: high.

**DECISION D23: wire caps: at most 1,500 turn actions and 750 messages per game, enforced as a rule (the long-game stop, 1.11); names at most 16 characters and 48 UTF-8 bytes; no hand-size cap below the physical 102.**
Alternative: a hand cap of, say, 30 cards; or no stop at all.
Why: game length is unbounded (a play-and-draw cycle can go on forever, the same finding as foolish's `IMESSAGE_BODY_CODEC.md` section 4), so a cap is the only way to promise that every game fits the 5,000-character `MSMessage.url` limit; section 4.5 shows the capped worst case fits.
A hand cap would stop players tapping the deck many times, which the owner wants.
The caps are provisional: they are to be reset to at least 3x the measured p99.9 once the bot test exists (7.4), and must still fit 4.5's bound.
Recommendation confidence: medium.

**DECISION D24: hand order is acquisition order, owned by the kernel: new cards go on the right, a played card leaves a gap that closes; the player cannot rearrange.**
Alternative: let the player sort or drag cards within the hand.
Why: `UI.html` "It landed on you" ("sorting a hand on arrival is the fastest way to make a player lose their place"), and a play is coded as a position in the hand (4.4), so a hand order that only one phone knew would be a second derivation of it (the foolish hand-order divergence of September 2026).
Recommendation confidence: medium.

**DECISION D25: history-as-code: every bubble carries the whole game as seed + roster + a mixed-radix code of every choice ever made, not a delta.**
Alternative: per-turn deltas.
Why: an extension sees only the tapped message (`IMESSAGE_GAME_DESIGN.md` section 7.1, "chains, not diffs", and `uttt/c/src/uttt_msg.h`), so any bubble must rebuild the game on its own.
Recommendation confidence: high.

**DECISION D26: races between two bubbles answering the same parent are settled by a total order in C (section 4.8): more completed turns, then more bubbles, then a "Last card!" tip beats a tip that is not, then the lobby rules, then the digest.
No merge.**
Alternative: foolish's Rule P unchanged; or merging.
Why: foolish's merge was built and rejected because a device can only hold the chains it was awake for (`IMESSAGE_GAME_DESIGN.md` section 7.7).
The say-it clause gives the benefit of the doubt to the player who remembered, when their "Last card!" and somebody's "Caught you!" answer the same bubble.
Recommendation confidence: medium.

**DECISION D27: the lobby is foolish's lobby with the rules checkbox deleted: open roster, lowest-free-first seating, capacity 2 in a DM and 8 in a group, any seated player may start once 2 are seated except the newest joiner while there is still room, join-and-start in one bubble when the join fills the table.**
Alternative: UTTT's "the joiner takes the seat and moves first" (2 seats only).
Why: the owner's instruction ("like foolish's but simpler"), and foolish's lobby is where the racing-Start and ghost-seat bugs were already paid for (`docs/IMESSAGE_LOBBY_V3.md`).
Recommendation confidence: high.

**DECISION D28: the starter may continue straight into their first turn in the start bubble when they are the first player (seat 1).**
Alternative: the start bubble carries the deal only.
Why: in a DM the joiner is seat 1, so "join, start and play" becomes one text instead of two.
There is no reroll hole: the deal depends only on the seed and the seat count, which are fixed before the starter sees anything.
Recommendation confidence: medium.

---

## 3. Kernel design

### 3.1 Where it lives

Mirroring `uttt/c/` file for file, so the build, the tests and the iOS bridge follow a template that already ships:

| File | What | Sister |
|---|---|---|
| `pickemup/c/src/pk.h`, `pk.c` | the rules: state, deal, legality, apply, seal, undo by replay | `uttt/c/src/uttt.{h,c}` |
| `pickemup/c/src/pk_deck.c` | card ids, the shuffle and the reshuffle | foolish `c/src/game.c` deal, `shared/c/deal_rng` |
| `pickemup/c/src/pk_code.{h,c}` | the mixed-radix coder | `uttt/c/src/uttt_code.{h,c}` |
| `pickemup/c/src/pk_msg.{h,c}` | the envelope, seats, lobby verdicts, the preference order | `uttt/c/src/uttt_msg.{h,c}`, foolish `c/src/msg_wire.h` |
| `pickemup/c/src/pk_view.{h,c}` | the masked per-seat view | foolish `c/src/view.c` |
| `pickemup/c/src/pk_plan.{h,c}` | animation plan events | `uttt/c/src/uttt_anim.h`, foolish `c/src/evwire.h`, `c/src/anim_plan.h` |
| `pickemup/c/src/pk_say.{h,c}` | which sentence a position says | `uttt/c/src/uttt_say.{h,c}` |
| `pickemup/c/i18n/keys.h`, `strings_en.c` | the words, one key list | `uttt/c/i18n/keys.h`, `strings_en.c` |
| `pickemup/c/ios/pk_api.c` | the flat entry points Swift calls | `uttt/c/ios/uttt_api.c` |
| `pickemup/c/tests/*.c` | section 7 | `uttt/c/tests/` |
| `pickemup/c/Makefile` | `run`, `asan`, `ios-lib`, `ios-smoke` | `uttt/c/Makefile` |

Shared, used unchanged: `shared/c/deal_rng.{c,h}` (ChaCha), `shared/c/sha256.{c,h}`, `shared/c/b32.{c,h}` (the link text), `shared/c/msg_stage/` (when an insert may go, and whether a received bubble is my own echo), `shared/c/i18n/languages.h`.
The Swift side reaches the kernel through generated bindings: `shared/tools/structgen` over `pk_api.h` and the fixed-layout structs (`PkView`, `PkEvent`, `PkSince`), and `shared/tools/datagen` for any string table a host needs outside the kernel.
No JSON anywhere in a shipped path.

The mixed-radix bignum in `uttt_code.c` should move to `shared/c/mixrad.{c,h}` when this kernel is built, since two products would then carry it; that lift is part of the build, not of this design.

### 3.2 Cards

A card is a `uint8_t` id 0..103, fixed forever (the id order is part of the shuffle, so it is part of the format):

```
id  0..95   suited: suit = id / 24, k = id % 24
              k  0..17  number (k / 2) + 1, copy k % 2
              k 18..19  Skip
              k 20..21  Reverse
              k 22..23  +2
id 96..99   Wild
id 100..103 Wild +4
```

```c
#define PK_DECK        104
#define PK_SUITS       4
#define PK_NO_SUIT     4            /* a wild's own suit */
#define PK_R_SKIP      10
#define PK_R_REVERSE   11
#define PK_R_PLUS2     12
#define PK_R_WILD      13
#define PK_R_WILD4     14
#define PK_CARD_HIDDEN 0xFE         /* a masked card in a view or event */
#define PK_CARD_NONE   0xFF

static inline int pk_suit(uint8_t c) { return c < 96 ? c / 24 : PK_NO_SUIT; }
int pk_rank(uint8_t c);             /* 1..9, or PK_R_* */
```

Two cards match when `pk_suit(a) == live_suit` or `pk_rank(a) == pk_rank(top)`, or `a` is a wild.
The two copies of a card are different ids with identical rules; which copy a player holds matters only for where it sits in their hand.

### 3.3 State

```c
#define PK_MAX_SEATS     8
#define PK_HAND_CAP      PK_DECK       /* physical limit, never a rule (D23) */
#define PK_MAX_ACTIONS   1500          /* long-game stop (1.11, D23) */
#define PK_MAX_BUBBLES   750
#define PK_SEAT_NONE     0xFF

enum { PK_OVER_NO = 0, PK_OVER_OUT, PK_OVER_STUCK, PK_OVER_LONG };

typedef struct {
    /* the table */
    uint8_t  n;                          /* seats, 2..8                              */
    uint8_t  turn;                       /* whose turn, PK_SEAT_NONE when over       */
    int8_t   dir;                        /* +1 clockwise, -1 anticlockwise           */
    uint8_t  live_suit;                  /* 0..3                                     */
    uint8_t  deck[PK_DECK];  uint8_t deck_n;    /* deck[deck_n-1] is the top         */
    uint8_t  stack[PK_DECK]; uint8_t stack_n;   /* stack[stack_n-1] is the top       */
    uint8_t  hand[PK_MAX_SEATS][PK_HAND_CAP];   /* acquisition order (D24)           */
    uint8_t  hand_n[PK_MAX_SEATS];

    /* the last card */
    uint8_t  exposed;                    /* bit s: on one card, not said, window open */
    uint8_t  said;                       /* bit s: said it, still on one card (LAST)  */

    /* the end */
    uint8_t  over;                       /* PK_OVER_*                                 */
    uint8_t  winner;                     /* seat, or PK_SEAT_NONE                     */
    uint8_t  idle_passes;                /* bare passes in a row (1.10)               */

    /* counters the header repeats (4.2) and the stop reads */
    uint16_t reshuffles;                 /* r of the last reshuffle, 0 = none yet     */
    uint16_t actions;                    /* turn actions: draws, plays, passes        */
    uint16_t turns;                      /* completed turns (plays + passes)          */
    uint16_t bubbles;                    /* sealed bubbles                            */

    /* the bubble being composed (all zero between bubbles) */
    uint8_t  b_open;                     /* 1 while a bubble is open                  */
    uint8_t  b_sender;
    uint8_t  b_said;                     /* "Last card!" in this bubble               */
    uint8_t  b_call;                     /* caught seat, or PK_SEAT_NONE              */
    uint8_t  b_exposed_at_open;          /* `exposed` when the bubble opened (D5d)    */
    uint8_t  b_had_terminal;             /* a play or pass happened in this bubble    */
    uint8_t  b_created;                  /* exposures this bubble's plays created     */
    uint8_t  t_drew;                     /* the current turn has drawn (D10)          */
    uint16_t b_floor;                    /* history index undo may not go below (D8)  */

    /* the history the coder writes, the replay rebuilds, and undo truncates */
    PkAct    hist[PK_MAX_ACTIONS + 3 * PK_MAX_BUBBLES];
    uint16_t hist_n;

    uint8_t  seed[32];
} PkGame;
```

`hand` is 8 x 104 bytes and `hist` is 3,750 records of 4 bytes, so the whole struct is about 16 KB, most of it `hist`; one resident game plus one scratch game for Rule P and undo is far under any extension budget.

There is exactly one representation of each truth.
Counts are `hand_n`, never a second field; `exposed` and `said` are cleared by the one helper every hand change goes through (`pk_hand_changed(g, s)`: if `hand_n[s] != 1`, clear both bits for `s`).

### 3.4 Action vocabulary

```c
enum {
    PK_A_DRAW = 1,      /* take the top of the deck (reshuffling first if it must) */
    PK_A_PLAY,          /* a = hand position, b = suit for a wild (0..3), else PK_NO_SUIT */
    PK_A_PASS,
    PK_A_SAY_IT,        /* "Last card!" - bubble-level                              */
    PK_A_CALL_OUT,      /* a = target seat - bubble-level                           */
    PK_A_BUBBLE,        /* history only: a = sender, b = bit0 said, bit1 call; c = call target */
    PK_A_CONTINUE,      /* history only: the sender went on to their next turn (D7) */
};
typedef struct { uint8_t kind, a, b, c; } PkAct;
```

`DRAW`, `PLAY`, `PASS` are **turn actions**.
`SAY_IT` and `CALL_OUT` are **bubble-level**: they belong to the bubble, not to a moment in it (D5d), so the host may apply them at any point while composing, and the history records them in the bubble's `PK_A_BUBBLE` record regardless of when they were tapped.
`BUBBLE` and `CONTINUE` never come from a host; they are how `hist` stores bubble boundaries.

A PLAY names a **hand position**, not a card id, because that is what the player touched and what the coder can index (4.4).
The kernel turns it into the id.

### 3.5 Deal, shuffle and reshuffle

One RNG, one keystream: `shared/c/deal_rng`, keyed by the 32-byte seed.

**The shuffle** (a Fisher-Yates over an array `a` of `m` cards):

```
for i = m-1 down to 1:
    j = deal_rng_bounded(&rng, i + 1)
    swap a[i], a[j]
```

**The deck at the start:** `a[k] = k` for k = 0..103, `deal_rng_seed_at(&rng, seed, 0)`, shuffle, then `deck = a`, `deck_n = 104`, so the top is `a[103]`.

**The deal** (1.4): for round 0..6, for each seat in turn order starting at seat 1 (`1, 2, ..., n-1, 0`), pop the top of the deck onto the end of that hand.

**The start card:** pop the top; while it is not a number card, insert it at `deck[0]` (the bottom, shifting the rest up) and pop again; push the number card onto the stack; `live_suit` = its suit; `turn = 1` (seat 1, D19); `dir = +1`.
It terminates because there are 72 number cards.

**A reshuffle** happens inside a draw (own or penalty) that finds `deck_n == 0` with `stack_n >= 2`:

```
r = ++reshuffles                               (the reshuffle counter, 1-based)
a = stack[0 .. stack_n-2]                      (every stack card except the top, bottom first)
m = stack_n - 1
deal_rng_seed_at(&rng, seed, (uint64_t)r << 32)
shuffle(a, m)
deck = a, deck_n = m                            (top is a[m-1])
stack[0] = old top, stack_n = 1
```

Every input is in the replayed history: the stack order is the order cards were played, and `r` is a count.
Nothing about a reshuffle is on the wire, and a phone that has never computed reshuffle r-1 can compute reshuffle r (the reason `deal_rng_seed_at` exists).
The initial shuffle uses block 0 onwards; it consumes a handful of blocks, far below block 2^32, so the stretches never overlap.

A chosen wild suit belongs to the stack top, not to the card: a reshuffled wild is just a wild.

### 3.6 Legality

One function answers every legality question, and `pk_legal` is written in terms of it so there is one definition, not two (the `uttt_legal` pattern):

```c
/* Everything `seat` may do now, in the canonical order (4.4). Returns the count.
 * The order is part of the format. */
int pk_legal(const PkGame *g, int seat, PkAct *out, int cap);
int pk_is_legal(const PkGame *g, int seat, PkAct a);
```

With `g->over == PK_OVER_NO`, and a bubble either not open or open by `seat` (while one seat has a bubble open, no other seat has anything legal on that draft):

| Action | Legal when |
|---|---|
| `SAY_IT` | `seat` is exposed at bubble open (`exposed` if not open, `b_exposed_at_open` if open), and `b_said == 0` |
| `CALL_OUT(t)` | `t != seat`, `t < n`, `said` bit of `t` clear, and `b_call == PK_SEAT_NONE` |
| `DRAW` | `seat == turn`, the bubble has not ended its turn (below), and `deck_n > 0` or `stack_n >= 2` |
| `PLAY(p, s)` | `seat == turn`, turn not ended, `p < hand_n[seat]`, the card matches or is wild; for a wild that is not the player's last card `s` is 0..3, otherwise `s == PK_NO_SUIT` |
| `PASS` | `seat == turn`, turn not ended, and `t_drew`, or (nothing drawable and no `PLAY` legal) |

"The bubble has ended its turn" means: the bubble's last turn action was a terminal and the turn did not come straight back to the sender (or it did, but the sender is on one card, D7).
While a turn is ended, only `SAY_IT` and `CALL_OUT` (and seal) remain.

`CALL_OUT` legality never reads a hand count or `exposed`.
That is load-bearing: if it did, a dimmed fan would tell every player who is on one card (3.7, test 7.1.4).

```c
/* May this draft be sealed into a bubble now? Non-empty, and not mid-turn:
 * either no turn actions, or the last one was a terminal. */
int pk_can_seal(const PkGame *g);
```

### 3.7 Apply, seal and undo

```c
int  pk_apply(PkGame *g, int seat, PkAct a);   /* 1 applied, 0 refused (g untouched) */
int  pk_seal(PkGame *g);                       /* close the bubble; 1 or 0            */
int  pk_undo(PkGame *g);                       /* take back the newest undoable thing */
int  pk_unsay(PkGame *g);                      /* drop this bubble's SAY_IT           */
int  pk_uncall(PkGame *g);                     /* drop this bubble's CALL_OUT         */
int  pk_floor(const PkGame *g);                /* history index of the draft's floor  */
```

**Opening a bubble.** The first `pk_apply` by a seat when no bubble is open sets `b_open`, `b_sender`, `b_exposed_at_open = exposed`, `b_call = NONE`, and appends a `PK_A_BUBBLE` record whose fields are filled in at seal.

**SAY_IT:** `b_said = 1`; clear `seat` from `exposed`; set it in `said`.
Order-independent: nothing a turn action does reads `exposed` or `said`.

**CALL_OUT(t):** `b_call = t`.
Nothing else happens until seal (D5d).

**DRAW:** reshuffle if `deck_n == 0` (3.5); pop the top onto the end of the hand; `t_drew = 1`; `actions++`; `idle_passes = 0`; `pk_hand_changed`; the draft floor moves here (`b_floor = hist_n`).

**PLAY(p, s):** take the card at position p out of the hand (the rest close up, order kept); push it on the stack; `live_suit` = the card's suit, or `s` for a wild; `actions++`, `turns++`, `idle_passes = 0`, `t_drew = 0`, `b_had_terminal = 1`; `pk_hand_changed`.
Then:

- hand now empty: `over = PK_OVER_OUT`, `winner = seat`, `turn = NONE`; nothing else.
- hand now one card: set `seat` in `exposed` and in `b_created`.
- effects, with `next(k)` = k seats on in `dir`:
  - number, Wild: `turn = next(1)`.
  - Skip: `turn = next(2)`.
  - Reverse: at n > 2 `dir = -dir`, then `turn = next(1)`; at n == 2 `turn = seat`.
  - +2: target `next(1)` draws 2 (penalty, 3.5 reshuffle rules), `pk_hand_changed(target)`, `turn = next(2)`.
  - Wild +4: target `next(1)` draws 4, likewise, `turn = next(2)`.

At n == 2, `next(2)` is the player themselves, which is how every action card gives them the next turn.

**PASS:** `actions++`, `turns++`, `b_had_terminal = 1`; if `t_drew` then `idle_passes = 0` else `idle_passes++`; `t_drew = 0`; `turn = next(1)`.
If `idle_passes == n`: `over = PK_OVER_STUCK`, winner by 1.10.

**The long-game stop:** after any turn action, if `actions == PK_MAX_ACTIONS`, `over = PK_OVER_LONG`, winner by 1.10's rule; the turn is cut where it stands and the bubble may be sealed mid-turn in this one case.

**CONTINUE** is appended when a terminal hands the turn back to the sender (at n == 2, not on one card, not over) and the sender then applies another turn action; `pk_seal` straight after the terminal is the other branch.

**Seal:**

1. Refuse unless `pk_can_seal`.
2. The catch, if `b_call != NONE` and the game is not over: if `b_exposed_at_open` has bit `b_call`, it is a HIT: clear `b_call` from `exposed`, the caught seat draws 2; otherwise a MISS: the sender draws 1.
   Each draw goes through the normal draw path (reshuffle included).
   A catch in a winning bubble is announced but deals nothing.
3. The window (D4): if `b_had_terminal`, `exposed &= b_created`.
   Any exposure that was open when this bubble began, and survived, closes safe.
4. Fill in the `PK_A_BUBBLE` record (sender, said, call).
5. `bubbles++`; if `bubbles == PK_MAX_BUBBLES` and not over: `over = PK_OVER_LONG`, winner by 1.10.
6. Clear every `b_*` and `t_drew`.

**Undo** is by replay, never by unwinding (the `uttt_undo` rule: a second, reverse set of rules would drift).
`pk_undo` rebuilds the game from its seed through `hist[0 .. k)` where k is the start of the newest turn action, and refuses if k would fall below `b_floor` (D8).
`pk_unsay` and `pk_uncall` rebuild with the bubble-level flag removed; they are always allowed while the bubble is open, because neither ever revealed anything.
A replay of 1,500 actions is microseconds.

The host's cancel (D9): on `didCancelSending`, rebuild the draft to `pk_floor` - which is the parent chain plus this bubble's draws and whatever was below them - rather than to the parent.

### 3.8 The masked view

```c
#define PK_VIEW_SPECTATOR (-1)
#define PK_VIEW_ALL       (-2)     /* tests and the finished board only */

typedef struct {
    uint8_t n, turn, dir, live_suit, over, winner;
    uint8_t top;                    /* stack top id                                 */
    uint8_t deck_n;                 /* shown ("27 left", D22)                       */
    uint8_t stack_n;                /* the pile's height, for drawing it            */
    uint8_t said;                   /* LAST stamps, public                          */
    uint8_t me;                     /* viewer seat, 0xFF spectator                  */
    uint8_t my_exposed;             /* 1 if the viewer may say "Last card!" now     */
    uint8_t my_n;                   /* the viewer's own hand                        */
    uint8_t my_hand[PK_HAND_CAP];
    uint8_t my_playable[PK_HAND_CAP]; /* 1 per position that PLAY accepts          */
    uint8_t can_draw, can_pass, can_seal;
    /* only when over: every hand, face up */
    uint8_t all_n[PK_MAX_SEATS];
    uint8_t all_hand[PK_MAX_SEATS][PK_HAND_CAP];
} PkView;

void pk_view(const PkGame *g, int viewer, PkView *out);
```

What each viewer can see:

| Fact | Own seat | Other seat | Spectator |
|---|---|---|---|
| own hand, faces, order | yes | - | - |
| other hands' faces | at game end | at game end | at game end |
| any hand's count | own only | **never while playing** | never while playing |
| stack top, live suit, direction, turn | yes | yes | yes |
| deck count | yes | yes | yes |
| LAST stamp (`said`) | yes | yes | yes |
| `exposed` | own bit only | never | never |

The other seats' counts are not in `PkView` at all until the game is over (`all_n` is zeroed), so no renderer can show one.
The kernel still knows them; animation gets them only as a stream of individual card events (section 5), which is public information.

Casual trust, as in foolish: the seed is in every bubble, so anyone who decodes their own bubble can compute every hand and the whole deck (`IMESSAGE_GAME_DESIGN.md` section 2).
The masking removes the one-tap version; it is not cheat-resistance.

---

## 4. Wire format

### 4.1 Shape

A bubble carries the whole game (D25): a fixed header, the roster, a two-byte check, and a body that is the mixed-radix code of every choice made since the deal.
Decoding is replay: the kernel re-deals from the seed and re-applies every choice through `pk_apply`, so an illegal or tampered choice simply fails (`IMESSAGE_GAME_DESIGN.md` section 7.3).
Like foolish's `msg_wire`, parse (structure, hostile bytes) and replay (semantics, public kernel calls only) are separate layers.

The text is UTTT's: `?m=` followed by the base32 of the bytes (`shared/c/b32.h`), because Messages drops an `MSMessage.url` whose scheme it will not vouch for and a bare query promises no destination (`uttt/c/src/uttt_msg.h`, `utm_text_encode`).

### 4.2 Byte layout (multi-byte fields little-endian)

```
off  size  field
0    1     magic       0xB9 (foolish is 0xF7, UTTT 0xB7)
1    1     format      1
2    1     phase       0 WAITING (lobby), 2 LIVE, 3 FINISHED  (foolish's numbers; 1 unused)
3    1     flags       bit0 DM (lobby capacity 2, else 8)
                       bit1 TIP_SAID (the newest bubble holds "Last card!"; derived)
                       bits 2-7 reserved = 0, refused if set
4    32    seed
36   2     lobby_rev   u16, roster changes so far (joins and leaves)
38   2     bubbles     u16, sealed bubbles since the deal (0 in WAITING)
40   2     turns       u16, completed turns (0 in WAITING)
42   1     n_seats     1..8 in WAITING, 2..8 once started (= n)
43   var   roster      n_seats x { tag[9], u8 name_len (1..48), name utf8 }
var  2     check       first 2 bytes of SHA-256 over every other byte
var  var   body        the code (4.4), to the end of the buffer; empty in WAITING
```

`bubbles`, `turns`, `TIP_SAID` and `phase` are **derived by sealing**: `pk_msg_seal` computes them by decoding the body it just wrote, so a host cannot emit a payload it would itself refuse (foolish `IMESSAGE_BODY_CODEC.md` section 5, point 2).
On decode they must agree with the replay, or the payload is refused.
The check turns a cut link into a refusal rather than a different game, because a mixed-radix code has no redundancy (`uttt_msg.h`).

There is no `parent8`, no send clock and no separate game id:

- the id is derived from the seed (D21);
- the preference order (4.8) never needs ancestry, because `bubbles` rises by exactly one from parent to child and never falls, which is the property foolish's ancestry clause existed to supply when its `turn` could fall;
- no rule here reads a time.

The **seat tag** is UTTT's: `SHA-256("pickemup.seat.1|" || seed || local participant id)[0..9)`, a value only the device that wrote it can recognise, salted by the game so it cannot follow a person between threads (`utm_tag`).
The **name** is the player's nickname, typed once and kept in the App Group, at most 16 characters (the memory note on seat-name clipping: 11-16 characters clip at 8 players; the cap is 16).

### 4.3 Errors

As `uttt_msg.h`: `PK_EOK 0`, `PK_ESHORT -1`, `PK_EMAGIC -2`, `PK_EFORMAT -3` (a format this build does not know: refuse, never misread), `PK_EFLAGS -4`, `PK_ECHECK -5`, `PK_EGAME -6` (the body does not replay, or disagrees with the header), `PK_EROSTER -7` (empty name, name too long, duplicate tag, seats out of range), `PK_ECAP -8`, `PK_ETEXT -9`.

### 4.4 The body

UTTT's coder, unchanged in method (`uttt/c/src/uttt_code.h`): the model is the rules, and the only thing stored is which option was chosen at each decision, as a digit whose base is the number of options the kernel lists there.
The number starts as the sentinel 1, digits are folded in backwards (`v = v * base + index`), bytes are the little-endian minimal representation, and the decoder walks forwards (`index = v % base; v /= base`) and must end at exactly 1.
A decision with one option is not a digit and costs nothing.

The digits, in order, per bubble (the decoder knows when to stop from `bubbles`):

```
BUBBLE b (for b = 1 .. bubbles), on the table as it stands:

  S  sender        base 2: 0 = the turn seat, 1 = someone else
                   if someone else: base n-1, index among the other seats in seat order
  Y  said          only if the sender is exposed:            base 2
  C  call          base 2: none / a call   (forced to "a call" - no digit - when the
                   sender is not the turn seat and did not say it, since a bubble
                   must hold something)
                   if a call: base (number of legal targets), index in seat order
                   among seats != sender whose LAST stamp is off
  T  turn present  only if the sender is the turn seat and (Y or C): base 2
                   (a turn seat with neither Y nor C must take its turn: no digit)

  then, if a turn is present, repeat:
     A  action     base = pk_legal's turn-action count; index into it
                   canonical order: DRAW (if legal), then every legal PLAY by
                   ascending hand position, a non-final wild as four entries in
                   suit order circle, triangle, square, diamond; then PASS (if legal)
     after a PLAY or PASS:
       if the game is over, or the turn did not come back to the sender, or the
       sender is on one card: the bubble ends
       else K continue   base 2: end the bubble / take the next turn too (D7)
```

Every constraint that makes a bubble valid is a menu the encoder and decoder both build, never a check after the fact, so any body that decodes also re-encodes to exactly its own bytes (the canonicality property foolish's tamper matrix asserts, `IMESSAGE_BODY_CODEC.md` section 5, point 4).
The long-game stop is a menu too: once `actions == PK_MAX_ACTIONS` the action menu is empty and the bubble ends.

The start bubble (D28) is just bubble 1 with the starter as sender.

### 4.5 Sizes

These are estimates from the menu sizes, to be replaced by measurement (test 7.4), exactly as foolish replaced its own estimates (`IMESSAGE_BODY_CODEC.md` section 2).

Bits: a turn-seat bubble's header costs about 1.3 bits (S, and a rare C); an out-of-turn catch about 1 + log2(7) + log2(7) = 6.6 bits at 8 players; a turn action about log2(menu), with menus of 2 to 12 options, averaging about 3.5 bits.

| Case | Body | Header + roster + check | Total bytes | Base32 chars (+3 for `?m=`) |
|---|---|---|---|---|
| 2p, 40 turns, 1 draw each | ~37 B | 43 + 2x15 + 2 = 75 | ~112 | ~183 |
| 4p, 80 turns, ~1 draw each | ~85 B | 43 + 4x15 + 2 = 105 | ~190 | ~307 |
| **8p, 40 turns, 6 draws each, 10 catches** (the owner's p99 case) | 280 actions x 3.5 + 50 bubbles ~ 1,120 bits ~ 140 B | 43 + 8x18 + 2 = 189 | ~329 | ~530 |
| 8p realistic long, 300 turns, 2.5 actions each | 750 x 3.5 + 330 bubbles ~ 3,200 bits ~ 400 B | 189 | ~589 | ~945 |
| **Capped worst case** (1,500 actions x 7 bits, 750 bubbles x 11 bits, 8 names of 48 bytes) | 18,750 bits = 2,344 B | 43 + 8x58 + 2 = 509 | 2,853 | **4,568** |

The worst case is bounded because every menu is at most 128 options (DRAW + PASS + 102 hand positions + 3 extra suits for each of 8 wilds = 128 = 7 bits) and a bubble header is at most 11 bits.
**4,568 characters is under Apple's documented 5,000-character `MSMessage.url` cap** (`docs/IMESSAGE_IMPLEMENTATION_HANDOFF.md`, "MSMessage.url cap is documented: 5,000 characters").
The realistic cases sit near foolish's self-imposed 1,000-character guardrail; that guardrail is a target, not a limit, and the test in 7.4 asserts the p95 8-player bubble under 1,000.

### 4.6 The lobby and seat handshake

The lobby is foolish's minus the rules checkbox (D27); the verdict functions are ported from `c/src/msg_wire.h` (`msg_lobby_offered`, `msg_lobby_can_exit`) with the rules arguments removed.

1. **New game.** The creator's phone draws 32 random bytes for the seed, seats the creator at seat 0 (tag and name), sets `DM` from the chat shape, `phase = WAITING`, `lobby_rev = 0`, and auto-stages the invitation.
   Creating never deals (`IMESSAGE_LOBBY_V3.md`, "One path for every chat shape").
2. **Join.** A player not in the roster takes the lowest free seat: their row is appended (seats are always contiguous `0..k-1`), `lobby_rev++`, and the reseal is auto-staged.
   Joins stop at capacity (2 in a DM, 8 in a group).
3. **Leave.** A seated player may leave once somebody else is seated (`msg_lobby_can_exit`): their row is removed and every later row moves down one seat, `lobby_rev++`.
   If seat 0 leaves, the new seat 0 is the dealer.
4. **Start.** Offered to a seated player when 2 or more are seated, except to the player who sent the newest lobby bubble while there is still room (foolish's M9 gate, with its full-lobby exemption).
   Start sets `n = n_seats`, deals from the seed at that count (3.5), `phase = LIVE`, `bubbles = 0`.
   The deal depends only on the seed and `n`, never on which route assembled the roster (`IMESSAGE_LOBBY_V3.md`, "Two routes to Start, provably one deal").
5. **Join and start** in one bubble is allowed exactly when the join fills the table (always true of the second player in a DM).
6. **First turn in the start bubble** (D28): if the starter is seat 1, they may go straight on into their turn before sealing.
7. A tapped WAITING bubble always renders as the lobby it says, even after the game has started (foolish round 7, `IMESSAGE_LOBBY_V3.md`).

Lobby controls (`pk_lobby_offered`, one enum, exactly one answer per state, walked exhaustively by test 7.8): `START`, `INVITE` (I am in alone and the newest bubble is not mine), `WAITING`, `JOIN`, `FULL`; plus the orthogonal `pk_lobby_can_exit`.

**Which seat am I** is UTTT's three witnesses in UTTT's order (`utm_resolve`): the device's own record keyed by game id, then the tag, then the sender in a DM; and foolish's nickname picker as the fallback for 3+ players when all three fail (`IMESSAGE_GAME_DESIGN.md` section 6.3).
In a lobby, a resolved seat counts only if the bubble in hand lists it (`msg_seat_resolve_in_lobby`).

### 4.7 Versioning

`format` is the second byte read, and a reader that meets a format it does not know refuses with `PK_EFORMAT` and the "newer version" screen.
A rules change that would deal or play any existing code differently is a new format number, with no migration (foolish's deal-order break, `docs/DEAL_ORDER.md`, "There is no migration and there will not be one").
The card id order, the shuffle, the reshuffle key schedule, the menu order and the digit order are all part of format 1.

### 4.8 Which of two bubbles wins

`pk_prefer(mine, tapped)`: negative for mine, positive for tapped, 0 identical.
Delivery order is never an input, so every phone holding the same pair agrees.

1. Different games (different seed): the tapped one.
2. A started chain (LIVE or FINISHED) beats a WAITING one.
3. More `turns` wins.
   A chain someone has actually played on is never clobbered.
4. More `bubbles` wins.
   This is what makes a child beat its parent, and an out-of-turn "Caught you!" beat the bubble it answered.
5. `TIP_SAID` beats not (D26): when "Last card!" and "Caught you!" answer the same bubble, saying it wins.
6. Higher `lobby_rev` wins, then more seats (racing Starts go to the fuller, later roster; foolish Rule P rule 3).
7. The lexicographically smaller SHA-256 of the envelope bytes.

The accepted costs, stated so nobody rediscovers them:

- A player's turn composed against bubble k beats an out-of-turn catch that also answered k (clause 3).
  The catch is lost; if the turn did not catch the same player, the window has closed (D4) and the exposed player got away with it.
  "The next completed turn decides."
- An exposed player's standalone "Last card!" loses to the next player's turn bubble that answered the same parent and caught them.
  They are caught.
- Nothing is merged, for foolish's reason: a device can only hold the chains it was awake for (`IMESSAGE_GAME_DESIGN.md` section 7.7).

A lost race on screen is the Conflict row of `UI.html`: the staged card flies home tinted red, then the winning chain plays forward.
The kernel's part is `pk_common_bubbles(a, b)`, the number of bubbles two chains of one game share, which tells the host what to take back and what to play.

---

## 5. Animation plan events

### 5.1 The event

```c
enum {
    PK_HALF_ACTION = 0,    /* what the sender did: plays at stage (channel A) */
    PK_HALF_SETTLE = 1,    /* what it caused: held until Send (channel B)     */
};

typedef struct {
    uint8_t  kind;         /* PK_EV_*                                          */
    uint8_t  half;         /* PK_HALF_*                                        */
    uint8_t  seat;         /* the seat it happens to, or PK_SEAT_NONE          */
    uint8_t  other;        /* the other seat involved (catcher, victim, sender) */
    uint8_t  card;         /* id, PK_CARD_HIDDEN when masked for this viewer   */
    uint8_t  suit;         /* live suit after the event                        */
    uint8_t  n;            /* how many (penalty size, reshuffle size, deal round) */
    uint8_t  i;            /* which of n (1-based), or the hand position       */
    uint16_t bubble;       /* 0 for the deal, else the bubble it belongs to    */
    uint16_t step;         /* the kernel step it belongs to (5.3)              */
    uint8_t  deck_n;       /* deck count after this event                      */
    uint8_t  dir;          /* direction after this event                       */
} PkEvent;                 /* 14 bytes, fixed layout, structgen'd to Swift     */

/* Every event from bubble `from` (exclusive; 0 = the deal) to `to` (inclusive),
 * masked for `viewer`. Returns the count written, or -1 if `cap` is too small. */
int pk_plan(const PkGame *g, int viewer, int from, int to, PkEvent *out, int cap);

/* The draft's own events (the open bubble), for channel A. */
int pk_plan_draft(const PkGame *g, int viewer, PkEvent *out, int cap);
```

Masking: a `DEAL` or `DRAW` card is `PK_CARD_HIDDEN` unless `viewer == seat`; every other card is public (played cards, the start card, buried cards); `REVEAL` is unmasked by definition.

### 5.2 Kinds and payloads

| Kind | half | seat | other | card | n, i | Emitted |
|---|---|---|---|---|---|---|
| `LOBBY_JOIN` | - | the joiner | - | - | - | a roster row appears (foolish's `ANIM_SURFACE_ROSTER`, a snap) |
| `LOBBY_LEAVE` | - | the leaver | - | - | - | a row goes; later rows move down |
| `LOBBY_START` | - | the starter | - | - | n = seats | the lobby gives way to the table (a fade) |
| `SHUFFLE` | - | - | - | - | n = 104 | once, before the deal |
| `DEAL` | - | receiver | - | masked | n = round 1..7, i = card 1..7n overall | one per card, round-robin from seat 1 |
| `FLIP` | - | - | - | the card | - | the top card turns over |
| `BURY` | - | - | - | the card | - | a flipped non-number goes to the bottom of the deck (1.4) |
| `START_CARD` | - | - | - | the card | - | the flip that stuck; `suit` is the live suit |
| `TURN_TO` | settle | new turn seat | previous | - | - | whenever the turn moves (after the deal, after each terminal) |
| `SAY_IT` | action | the sayer | - | - | - | "Last card!", the LAST stamp slams down (`UI.html` Motion, "One card left") |
| `CALL_OUT` | action | the target | the catcher | - | - | the fan is tapped; outcome not yet known |
| `DRAW` | action | the drawer | - | masked | n = draws this turn so far, i = 1.. | one per card, own draws |
| `RESHUFFLE_GATHER` | same as the draw it serves | - | - | - | n = cards | the stack (all but the top) slides to the deck well |
| `RESHUFFLE_SHUFFLE` | same | - | - | - | n = cards, i = reshuffle number r | the comic beat; a host may vary the gag by r |
| `RESHUFFLE_DONE` | same | - | - | - | n = new deck count | the deck is a deck again |
| `PLAY` | action | the player | - | the card | i = hand position | hand to stack, 420ms, lands crooked (`UI.html` Motion) |
| `WILD_SUIT` | action | the player | - | the wild | - | `suit` is the chosen suit; the halo changes |
| `SKIP` | settle | the skipped | the player | - | - | dim, then slash (`UI.html` Motion, "Skipped") |
| `REVERSE` | settle | - | the player | - | - | `dir` is the new direction; the word turns |
| `REVERSE_AS_SKIP` | settle | the skipped | the player | - | - | 2 players: a Reverse that skipped (D13) |
| `PENALTY` | settle | the victim | the cause's seat | the causing card or none | n = cards owed, i = kind (+2, +4, caught, wrong call) | before the penalty's draws |
| `PENALTY_DRAW` | settle | the victim | - | masked | n = owed, i = 1..n | one per card, 110ms apart (`UI.html` Motion, "Drawing two") |
| `PENALTY_SHORT` | settle | the victim | - | - | n = cards forgiven | the supply ran out (D15) |
| `PASS` | action | the passer | - | - | n = cards drawn this turn | the turn ends without a play |
| `CALL_HIT` | settle | the caught | the catcher | - | - | followed by `PENALTY` + 2 `PENALTY_DRAW` |
| `CALL_MISS` | settle | the catcher | the target | - | - | followed by `PENALTY` + 1 `PENALTY_DRAW` |
| `WIN` | settle | the winner | - | the last card | i = `PK_OVER_*` | the game ends |
| `REVEAL` | settle | a seat | - | a card | n = hand size, i = 1..n | at game end, every hand, one event per card |
| `BUBBLE_BEGIN` | - | sender | - | - | - | framing |
| `BUBBLE_END` | - | sender | - | - | - | framing |

### 5.3 Ordering guarantees

1. Events come out in the order the kernel applied things; nothing is reordered for display.
2. Every event carries its `step`: one step per kernel action (a draw, a play, a pass, a say, a catch declaration, a seal), and the automatic consequences of an action share its step.
   The composition default is foolish's: **within one step everything that moves goes at once; between steps, one after another** (`docs/MOTION_BEFORE_FLOW.md`).
   The one exception is written into the kinds: `PENALTY_DRAW`s and `REVEAL`s of one step are staggered, never together, because the player is being told a quantity.
3. Within a bubble:
   1. `BUBBLE_BEGIN`,
   2. `SAY_IT` if any, then `CALL_OUT` if any (bubble-level things first, whatever order they were tapped in),
   3. the turn(s) in order: per draw, the reshuffle triple if one was needed and then the `DRAW`; per play, `PLAY`, `WILD_SUIT`, then its effect (`SKIP` / `REVERSE` / `REVERSE_AS_SKIP` / `PENALTY` + `PENALTY_DRAW`s, with a reshuffle triple wherever a penalty draw needs one), then `TURN_TO`; per pass, `PASS`, `TURN_TO`,
   4. at seal, `CALL_HIT` or `CALL_MISS` with its penalty,
   5. `WIN` and the `REVEAL`s if the game ended,
   6. `BUBBLE_END`.
4. The deal is bubble 0: `LOBBY_START`, `SHUFFLE`, the `DEAL`s (7n, round-robin from seat 1), `FLIP` / `BURY` pairs, `START_CARD`, `TURN_TO` seat 1.
5. A reshuffle triple is always immediately before the draw that needed it, and has the same `half` as that draw.
6. **The cut.** Channel A (staged) plays the draft's ACTION events as they happen; SETTLE events of every turn except the bubble's last are released as soon as the sender continues (at 2 players the next turn needs them); the last turn's SETTLE events and the seal's `CALL_*` are held until Send (channel B).
   The cut is the kernel's, read off `half`, never re-listed by a host (foolish's `evw_is_settlement` rule).
   The catch outcome must never play before Send: it is the one event whose early display would leak (D5d).
7. Channels C, D and E (`UI.html` grid) all play both halves in this order; D and E play them from the sender's seat, with the sender's own draws masked.

### 5.4 What the kernel computes for the receiver

```c
typedef struct {
    uint16_t from, to;                 /* the bubble range summarised           */
    uint8_t  drawn[PK_MAX_SEATS];      /* own draws per seat                    */
    uint8_t  penalty[PK_MAX_SEATS];    /* penalty cards per seat                */
    uint8_t  plays[PK_MAX_SEATS];
    uint8_t  reshuffles;               /* how many happened in the range        */
    uint8_t  caught, caught_by;        /* last hit in the range, or NONE        */
    uint8_t  wrong, wrong_on;          /* last miss in the range, or NONE       */
    uint8_t  said;                     /* seats that said "Last card!"          */
    uint8_t  skipped;                  /* seats skipped                         */
    uint8_t  reversed;                 /* direction flips                       */
} PkSince;

void pk_since(const PkGame *g, int from, int to, PkSince *out);
```

- **Channel D** (a bubble opened cold) plays `pk_plan(g, me, bubbles - 1, bubbles)`: the newest bubble only, the one this message added (foolish's `n_new` idea, made exact by bubble boundaries).
- **Channel E** (an arrival on an open board) plays from the bubble on screen to the new tip, when `pk_common_bubbles(showing, arriving) == showing.bubbles`; otherwise it is a Conflict and the host takes back to the common prefix first.
- **Channel C** (my own bubble reopened) is D with nothing masked for me.
- `pk_since` is for the words: "Bo drew 9, the deck was shuffled, and Bo played a 3" is built from it (section 6), and a host that wants to show "you missed 3 bubbles" reads `to - from`.

`pk_since` counts are cards that moved, which are public; they never become a hand count on screen.

---

## 6. Words

Every user-facing string the kernel owns, English only for now, in the UTTT table shape (`uttt/c/i18n/keys.h`: one X-macro key list, a column limit per key, `{placeholders}`, a hole is a test failure).
No em dashes or en dashes in any of them (`shared/tools/release_strings.sh` refuses them), no full stop at the end of a caption or label, and none of the Mattel mark's words.

A bubble is composed on its sender's phone, so its caption is in the sender's language everywhere.
A bubble can never say "you": it is one bitmap and one line shown on every phone, so every caption names the actor.
Names come from the roster (`{who}`, `{target}`, `{next}`), because the kernel knows every seat's nickname and Messages' `$<uuid>` token only works for the sender.

### 6.1 Things

| Key | English |
|---|---|
| `GAME_NAME` | Pick 'Em Up |
| `SUIT_0` .. `SUIT_3` | circles / triangles / squares / diamonds |
| `SUIT_ONE_0` .. `SUIT_ONE_3` | circle / triangle / square / diamond |
| `CARD_NUMBER` | {rank} of {suits} |
| `CARD_SKIP` | skip on {suits} |
| `CARD_REVERSE` | reverse on {suits} |
| `CARD_PLUS2` | +2 on {suits} |
| `CARD_WILD` | wild |
| `CARD_WILD4` | wild +4 |
| `CALL_WORD` | Last card! |
| `CAUGHT_WORD` | Caught you! |
| `DIR_CW` | clockwise |
| `DIR_ACW` | anticlockwise |
| `DECK_LEFT` | {n} left |
| `STAMP_LAST` | LAST |

### 6.2 Bubble captions (one truncating line, composed by `pk_say_caption`)

A bubble's caption is its most important clause, then the next if it fits (`PK_CAPTION_MAX` columns), in this priority: game end, a catch, the play and its consequence, the draws, the next turn.

| Key | English |
|---|---|
| `CAP_INVITE` | {who} wants a game of Pick 'Em Up. Tap to join |
| `CAP_JOINED` | {who} joined |
| `CAP_LEFT` | {who} left |
| `CAP_STARTED` | Cards dealt. {next} goes first |
| `CAP_PLAYED` | {who} played {card} |
| `CAP_PLAYED_WILD` | {who} played {card}, now {suits} |
| `CAP_SKIPPED` | {who} skipped {target} |
| `CAP_REVERSED` | {who} turned the table around |
| `CAP_REVERSE_2P` | {who} reversed and goes again |
| `CAP_PLUS2` | {who} played +2. {target} draws two |
| `CAP_WILD4` | {who} played wild +4, now {suits}. {target} draws four |
| `CAP_DREW_ONE` | {who} drew a card |
| `CAP_DREW_N` | {who} drew {n} |
| `CAP_DREW_AND_PLAYED` | {who} drew {n} and played {card} |
| `CAP_DREW_AND_PASSED` | {who} drew {n} and passed |
| `CAP_PASSED` | {who} passed |
| `CAP_RESHUFFLED` | The pile went back in the deck |
| `CAP_SAID` | {who}: Last card! |
| `CAP_CAUGHT` | {who} caught {target}. {target} draws two |
| `CAP_WRONG` | {who} called {target} wrong and draws one |
| `CAP_NEXT` | {next} to play |
| `CAP_WON` | {who} is out and wins |
| `CAP_WON_WILD` | {who} went out on a wild and wins |
| `CAP_STUCK` | Nobody can move. {who} wins with the fewest cards |
| `CAP_LONG` | The game ran long. {who} wins with the fewest cards |

`summaryText` is the same line.

### 6.3 Screen lines (drawn for one phone, so they may say "you")

| Key | English |
|---|---|
| `HEAD_YOUR_TURN` | Your turn |
| `HEAD_WAITING` | Waiting on {who} |
| `HEAD_PICK_SUIT` | Pick a suit |
| `HEAD_STAGED` | Staged. Send it when you like |
| `HEAD_YOU_WIN` | You win |
| `HEAD_WINS` | {who} wins |
| `SUB_MATCH` | {suits}, or a {rank} |
| `SUB_MATCH_WILD` | {suits} |
| `SUB_PLAYABLE_NONE` | Nothing plays. Draw |
| `SUB_ORDER` | {a}, then {b}, then you |
| `SUB_ON_ONE` | One card. Say it before they catch you |
| `SUB_SAID` | You said it |
| `SUB_CAUGHT_YOU` | {who} caught you. Two cards |
| `SUB_WRONG_YOU` | Wrong call. One card |
| `SUB_DRAWN_STAY` | Drawn cards stay in your hand |
| `SUB_STAGED_SKIP` | {target} is skipped |
| `SUB_STAGED_PLUS` | {target} draws {n} |
| `SUB_STAGED_CALL` | You called {target} |
| `BTN_DRAW` | Draw |
| `BTN_PLAY` | Play |
| `BTN_PASS` | Pass |
| `BTN_SAY` | Last card! |
| `BTN_CAUGHT` | Caught you! |
| `BTN_AGAIN` | Again |
| `BTN_RULES` | Rules |
| `SPOKEN_FAN` | {who}'s cards. Tap to catch them on one |
| `SPOKEN_CARD` | {card}, {state} |
| `SPOKEN_PLAYABLE` | playable |
| `SPOKEN_NOT_PLAYABLE` | does not play |
| `SPOKEN_DECK` | Deck, {n} left. Tap to draw |
| `SPOKEN_STACK` | Pile, {card} on top, {suits} to match |

`SUB_ORDER` names at most the next two players, as `UI.html` does ("Bo, then Cy, then you").
No screen line ever says how many cards another player holds.

### 6.4 Lobby lines

| Key | English |
|---|---|
| `LOBBY_TITLE` | New game |
| `LOBBY_WAITING` | Waiting for players |
| `LOBBY_ALONE` | Only you so far |
| `LOBBY_FULL` | The table is full |
| `LOBBY_DEALER` | {who} deals |
| `BTN_JOIN` | Join |
| `BTN_START` | Start |
| `BTN_LEAVE` | Leave |
| `LOBBY_NAME_PROMPT` | Your name at the table |

### 6.5 Errors and staleness

| Key | English |
|---|---|
| `UNREADABLE` | Can't read that |
| `UNREADABLE_WHY` | That game came from a newer version of the app |
| `DAMAGED` | This game link is damaged |
| `MOVED_ON` | This game has moved on. Open the newest message |
| `LOST_RACE_PLAY` | {who} moved first. Your card came back |
| `LOST_RACE_CALL` | {who}'s turn landed first. Your call did not count |
| `LOST_RACE_SAY` | {who} caught you first |

### 6.6 The rules page

| Key | English |
|---|---|
| `RULES_TITLE` | How to play Pick 'Em Up |
| `RULE_1` | Match the top card of the pile by suit or by rank, or play a wild. Each suit is a shape and a colour |
| `RULE_2` | On your turn, draw as many cards as you like, then play one card or pass. You can only pass after drawing |
| `RULE_3` | Skip jumps the next player. Reverse turns the table around, and with two players it is a skip. +2 and wild +4 make the next player draw and miss their turn |
| `RULE_4` | A wild lets you pick the suit. There is no stacking |
| `RULE_5` | When you play down to one card, send that move first, then say Last card! If someone taps your cards before the next turn is done, you draw two |
| `RULE_6` | Tap someone's cards to catch them on one. Call it wrong and you draw one |
| `RULE_7` | Empty your hand to win |
| `RULE_8` | Cards you draw stay drawn. You can take back a card you played until you send it |

---

## 7. Test plan for the C kernel

Every test is mutation-checked before it is trusted: break the code it covers, see it fail on the right assertion, restore (the standing rule, and the memory note "mutation-check every test").
The mutation for each is named; "red" means it must fail on that assertion, not merely somewhere.
All run under `make -C pickemup/c run` and `make asan`, with no Mac.

### 7.1 Legality tables

1. **Matching.** A table of (top, live suit, card) -> playable, covering suit match, rank match of each action, wild on anything, wild on wild, a number on a wild's chosen suit.
   Mutation: drop the rank clause from the match (red on Skip-on-Skip); compare `pk_suit(top)` instead of `live_suit` (red on number-on-wild).
2. **Pass.** PASS refused before a draw with a draw available; allowed after one draw; allowed with nothing drawable and nothing playable; refused with nothing drawable but a playable card.
   Mutation: allow PASS whenever `t_drew || !can_draw` (red on the last row).
3. **Draw.** DRAW legal with a deck; legal with an empty deck and a stack of 2; refused with an empty deck and a stack of 1; legal while holding a playable card (D6).
   Mutation: `stack_n >= 1` (red on the stack-of-1 row).
4. **Call-out leaks nothing.** For every state in 10,000 random positions, `pk_legal`'s CALL_OUT set for any seat is identical whatever the target hand sizes and `exposed` bits are, given the same `said`.
   Mutation: add `&& hand_n[t] == 1` to CALL_OUT legality (red: sets differ).
5. **Say-it.** Refused in the exposing bubble; legal in the sender's next bubble while exposed; refused once said; refused after drawing.
   Mutation: set `b_exposed_at_open` after the play instead of at open (red on the first row).
6. **Menu order is the format.** `pk_legal`'s order is DRAW, plays by ascending position with a non-final wild as four suits in order, PASS; a golden list for fixed positions.
   Mutation: emit wild suits in reverse (red on the golden list, and on the coder round trip).
7. **Last-card wild has no suit.** A wild as a final card has one menu entry.
   Mutation: always four (red; and 7.3's canonicality then fails too).

### 7.2 Apply and effects

1. **Every action card at 2, 3 and 8 players**, a table of (n, seat, dir, card) -> (next turn, dir, victim draws).
   Mutation: Reverse at 2 flips `dir` (red on the n=2 row's `dir`); Skip uses `next(1)` (red).
2. **Penalty short supply.** A +4 with 2 cards available in deck and stack deals 2 and forgives 2, emitting `PENALTY_SHORT n=2`.
   Mutation: stop dealing at the first empty deck without reshuffling (red on the dealt count).
3. **Going out ends it at once.** A last-card +2 deals nothing and moves no turn; a catch in the winning bubble deals nothing.
   Mutation: apply effects before checking for the empty hand (red on the victim's hand count).
4. **Hand order.** Draws append; a play removes and closes the gap; order of the rest is unchanged (D24).
   Mutation: swap-remove (last card into the gap) (red).
5. **One representation.** After every action in 1,000 random games, `exposed` and `said` bits are set only for seats with `hand_n == 1`.
   Mutation: skip `pk_hand_changed` after a penalty draw (red).

### 7.3 The deck: deal, flip, reshuffle determinism

1. **Round-robin deal.** From a fixed seed at n=3, the first seven cards off the shuffled deck go to seats 1, 2, 0, 1, 2, 0, 1 in that order.
   Mutation: deal seven to each seat in turn (red on the second card).
2. **Shuffle golden.** A fixed seed gives a fixed 104-card order (a committed hex vector), and the deck is a permutation of 0..103.
   Mutation: `deal_rng_bounded(&rng, i)` instead of `i + 1` (red on the vector).
3. **Start card.** A seed whose top card is an action buries it at the bottom and starts on the next number; seeds forcing 1, 2 and 5 consecutive buries.
   Mutation: bury at the top (red: loops or starts on the same card).
4. **Reshuffle determinism.** Two games from the same seed and the same history produce identical decks after reshuffles 1, 2 and 3; reshuffle r computed by `deal_rng_seed_at` from scratch equals the one computed after walking reshuffles 1..r-1.
   Mutation: key reshuffle r from block r instead of r << 32 (red on the overlap test, below).
5. **No overlap.** The keystream words consumed by the initial shuffle and by reshuffle 1 come from disjoint blocks.
   Mutation: as 7.3.4.
6. **Top stays.** After a reshuffle the stack is exactly the old top with its live suit.
   Mutation: include the top in the reshuffle (red).
7. **Cross-engine.** The native build and the wasm replay build (if one exists by then) agree on 100 golden games' final states.
   Mutation: none needed beyond any single change to 7.3.2.

### 7.4 Wire round trip and size

1. **Round trip.** 10,000 bot games at each n in 2..8: encode after every bubble, decode, replay, compare the full `PkGame` (hands, deck, stack, flags, counters) and re-encode byte-identical.
   Mutation: drop the continue digit K (red on 2p games).
2. **Canonicality / tamper matrix.** Flip every bit of 500 envelopes: each either refuses or decodes to something that re-encodes to exactly its own bytes.
   Mutation: allow the empty-bubble option in the C digit (red: a flipped body decodes to an empty bubble that re-encodes differently).
3. **Header agreement.** A body whose replay disagrees with `bubbles`, `turns`, `phase` or `TIP_SAID` is refused `PK_EGAME`.
   Mutation: stop checking `turns` (red on a crafted payload).
4. **Check.** Every truncation of a valid body is refused `PK_ECHECK` or `PK_EGAME`.
   Mutation: skip the check (red on truncation by one byte).
5. **Size gate.** Measured median, p95, p99, p99.9 and max base32 length per n over 10,000 games of the bot the players will face; asserts p95 at 8 players < 1,000 chars and the capped worst case (a crafted maximal game) < 5,000.
   Mutation: raise `PK_MAX_ACTIONS` to 4,000 (red on the worst-case assertion).
6. **Caps are rules.** A game driven to 1,500 actions ends `PK_OVER_LONG` with the fewest-cards winner, and its bubble still seals and decodes.
   Mutation: check the cap only at seal (red: a 104-draw turn crosses it).

### 7.5 Masking

1. **Other counts never leak.** For every viewer and every position in 1,000 games, `PkView` for a playing game is byte-identical when any OTHER seat's hand is replaced by a different hand of a different size (same public history).
   Mutation: fill `all_n` while playing (red).
2. **Spectator** sees no hand at all.
   Mutation: treat -1 as seat 0 (red).
3. **Game end reveals all.** Mutation: keep masking when over (red).
4. **Events mask draws.** Every `DEAL` / `DRAW` / `PENALTY_DRAW` card is hidden for any viewer but its receiver; played, flipped and buried cards are never hidden.
   Mutation: mask by `other` instead of `seat` (red).

### 7.6 Plan events

1. **Golden plans.** Five scripted bubbles (a plain play; draw 9 with a reshuffle then play; a +2 at 2 players with continue; a hit catch in a turn bubble; a wild win with reveal) produce committed event lists, kind by kind.
   Mutation: emit the reshuffle triple after the draw (red on list 2).
2. **Ordering invariants** over 1,000 random games: SAY_IT and CALL_OUT come before any turn event of their bubble; CALL_HIT / CALL_MISS come after the bubble's last terminal; every reshuffle triple precedes a draw with the same step; step numbers never decrease.
   Mutation: emit CALL_HIT at the CALL_OUT step (red).
3. **The cut.** In a staged draft, no `CALL_HIT` / `CALL_MISS` / penalty of a catch is ACTION-half; at 2 players a continued turn's SETTLE events are releasable before the next action.
   Mutation: mark `CALL_MISS` as ACTION (red).
4. **One card per event.** Every card that changes location changes it in exactly one event (count cards moved by replaying state vs. by summing events).
   Mutation: fold two penalty draws into one event with n=2 (red).
5. **Since.** `pk_since` totals equal the sums over `pk_plan`'s events for random ranges.
   Mutation: count penalty draws in `drawn` (red).

### 7.7 Call-out windows

1. **The table of cases** (each a scripted chain, asserting who draws what):
   - exposed, next turn bubble catches: hit, 2 cards;
   - exposed, says it standalone first, then the next turn catches: miss, catcher 1;
   - exposed, next turn bubble does not catch: window closed, a later catch misses;
   - exposed, an out-of-turn catch lands before the next turn: hit;
   - exposed, an unrelated wrong catch on someone else in between: the window is still open (D4);
   - exposed at 2 players after a Skip: the exposed player's own next turn closes the window;
   - the exposed player draws: no longer exposed, the LAST stamp off;
   - two players exposed at once.
   Mutation: close the window at the end of every bubble (red on the fifth case).
2. **Judged at open.** A catch whose bubble also plays a +2 on the caught player still hits (judged before the +2).
   Mutation: judge at seal (red).
3. **Penalty at end.** The catcher's own draws in a catching bubble are the same cards as without the catch.
   Mutation: deal the catch penalty at CALL_OUT (red).
4. **Rule P races.** Siblings say-it vs catch: say-it wins; turn vs catch: turn wins; child vs parent: child; racing Starts: fuller; and `pk_prefer` is antisymmetric and transitive over 10,000 random pairs.
   Mutation: drop the `TIP_SAID` clause (red on the first); compare `turns` after `bubbles` (red on the second).

### 7.8 Lobby, 2-player edges, deck exhaustion

1. **Lobby verdicts exhaustive.** Every (my seat, joined, capacity, i sent the newest) has exactly one control and a way forward; the newest joiner cannot Start unless full; join-and-start is offered only on the filling join.
   Mutation: drop the full-lobby exemption (red: DM stranded).
2. **Two routes, one deal.** Join-then-Start and Join-and-Start deal identical hands.
   Mutation: deal from `lobby_rev` instead of `n` (red).
3. **Leave compacts seats.** Mutation: leave a hole (red on contiguity).
4. **2 players.** Reverse is a skip and `dir` unchanged; Skip, +2, +4 give a continue option; a play to one card ends the bubble even when the turn comes back.
   Mutation: allow continue on one card (red: the exposed player goes out in the same bubble).
5. **Stuck table.** With deck and stack exhausted and nothing playable, n bare passes end the game with the fewest-cards winner and the turn-order tie-break; a draw in between resets the count.
   Mutation: count passes that drew (red on the reset case).
6. **Undo floor.** Undo returns a play above the last draw; refuses to pass below a draw; `pk_unsay` / `pk_uncall` always work; undo-by-replay equals the state before the undone action.
   Mutation: set the floor at the last play instead of the last draw (red).
7. **Seat resolve.** UTTT's `utm_resolve` table ported (record, tag, sender, none) plus the lobby gate.
   Mutation: trust the tag over the record (red on the rotated-id row).

---

## 8. Open questions for the owner

1. **The name.** `Pick 'Em Up` is a placeholder with a same-genre collision (README); every caption uses `GAME_NAME`, so changing it is one string, but it has to be decided before a store listing.
2. **Hands past thirteen cards.** Free drawing (D6) with no hand cap (D23) means a hand can reach 102 cards; `UI.html` stops at thirteen, where the thin-face rule already fires.
   The kernel is fine either way; the surface needs a design (a scrolling hand, a second compression, or a cap after all) before D23 can stand.

Everything else is decided above and can be vetoed line by line.

---

## Appendix A. What `UI.html` needs to change

The surface study predates these rules; each item names the view and what this document makes wrong in it.

1. **Seat badges show card counts** ("4 cards", "7 cards") in every bubble, expanded and collapsed view, and the note under bubble 01 argues for them.
   D22 removes them: a seat shows its name and a constant card fan whatever the count, and the fan is the tap target for "Caught you!".
2. **Bubble 03 "Your count already reads 8"** goes with the counts.
3. **Bubble 04 "Alex has one card left."** and the expanded "One left" screen ("Everybody has been told") announce the one-card state automatically.
   Under 1.8 nothing is announced: the LAST stamp and "{who}: Last card!" appear only after the player says it.
4. **"They know.
   Bo is holding two wilds."** (expanded, One left) states another player's hidden cards; it must go.
5. **Bubble captions say "you"** ("Your turn", "Back to you", "You drew two") although the page's own note says a bubble can never say you.
   They become 6.2's lines ("{next} to play", "{target} draws two").
6. **The deck sits in foolish's top-left well** in the expanded and collapsed views; the owner's layout puts the draw deck immediately to the LEFT of the centre pile.
   The bubble view already draws them side by side.
7. **"Draw or tap a card"** implies one tap plays.
   The owner's inputs are: draw by tapping the deck, dragging from the deck to the hand, or the Draw button; play by dragging a card to the pile, or tapping a card and then a Play button.
   A tap only selects.
8. **No Pass button** exists; it is needed after a draw (D10), and a "Last card!" button for the exposed player.
9. **Undo** must show that drawn cards do not fly back (D8), with `SUB_DRAWN_STAY`.
10. **The suit picker** is not shown for a last-card wild (D17).
11. **The Conflict row** says two players cannot move at once; out-of-turn "Last card!" and "Caught you!" bubbles now can, and are the other source of lost races (4.8).
12. **The direction word** is hidden at 2 players (D13).
13. **The "One fork worth settling now" box** proposes "until the next bubble seals"; D4 settles it as "until the next completed turn", with the reason.
14. **The channel grid** needs rows for draws (one flight per card, staggered like "Drawing two"), the reshuffle (gather, comic shuffle, done), the deal (round-robin, one card at a time), buried start cards, a catch (declared at stage, outcome only at Send) and the end reveal.
    Section 5 lists them.

Already consistent and kept: the 7-card deal ("Shed seven cards"), ranks 1-9, the deck count ("27 left", D22), the LAST stamp slot under the badge, the modal centred suit picker that does not travel, "the move is not a move until the suit exists", new cards arriving on the right, the halo as the live suit, "direction is a word", skip as two pause bars and reverse as opposed solid triangles (LEGAL).

## Appendix B. Files this design relied on

- `pickemup/README.md`, `pickemup/LEGAL.md`, `pickemup/docs/UI.html`
- `docs/ARCHITECTURE_AS_A_PATTERN.md`, `docs/MOTION_BEFORE_FLOW.md`, `shared/README.md`, `werewolf/COMMON.md`
- `docs/IMESSAGE_GAME_DESIGN.md` (sections 4, 5, 6, 7, 11.5), `docs/IMESSAGE_BODY_CODEC.md`, `docs/IMESSAGE_LOBBY_V3.md`, `docs/DEAL_ORDER.md`, `docs/IMESSAGE_IMPLEMENTATION_HANDOFF.md` (the 5,000-character cap)
- `uttt/c/README.md`, `uttt/c/Makefile`, `uttt/c/src/uttt.h`, `uttt/c/src/uttt_code.h`, `uttt/c/src/uttt_code.c` (the sentinel coder), `uttt/c/src/uttt_msg.h`, `uttt/c/src/uttt_anim.h`, `uttt/c/src/uttt_say.h`, `uttt/c/i18n/keys.h`, `uttt/c/i18n/strings_en.c`
- `c/src/msg_wire.h` (envelope, Rule P, lobby verdicts, seat resolution), `c/src/evwire.h` (event kinds, the settlement cut), `c/src/anim_plan.h` (surface plans for lobby beats)
- `shared/c/deal_rng.h`, `shared/c/b32.h`, `shared/c/sha256.h`, `shared/tools/structgen/`, `shared/tools/datagen/`

