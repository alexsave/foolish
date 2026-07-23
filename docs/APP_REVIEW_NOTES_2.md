# App Review notes - Foolish (iMessage), second pass

Reviewer's log. I was told one thing about this submission: it is Russian Durak.
Everything below comes from driving the app into states and looking at what it
put on screen. I did not read the developer's documentation.

## How this pass was run

The shipping product is an `MSMessagesAppViewController`, and Apple's Messages
host cannot be given 3-8 fake participants in a simulator, so I drove the
developer's own harness target (`FoolishHarness`), which mounts the *same*
`MessagesRootView` the extension mounts and feeds it a fake transcript. Every
state below was reached by calling the same model entry points the real chrome's
buttons call - no synthetic taps - then screenshotting once the state settled.

Read the screenshots with two caveats:

- The **orange bar and the "you are:" pill row at the top of every shot are the
  harness**, not the product. Ignore them. The product is everything below the
  black line.
- Presentation-style transitions, real insert-vs-send semantics, and receiving a
  message while the extension is closed belong to Apple's host and are **not
  covered** by this pass. Nothing here should be read as a verdict on those.

Screenshots: `docs/review-shots-2/`. Device: iPhone 16 (iOS 26.3) unless a shot
is prefixed `r_se_` (iPhone SE, 4.7").

---

## Blockers

### B1. A name longer than 12 bytes breaks the game, silently, and lands the user on "This game link is damaged."

This is the one that would stop the submission on its own.

The setup screen takes a free-form name with **no length cap, no counter, and no
inline validation** (`r_setup_longname.png` - a 60-character name is accepted and
"Create game" is lit). Sealing that name into the message payload fails, and the
create path turns any failure into `damaged = true`, i.e. the user types their own
name, taps Create game, and is told the *link* is damaged
(`r_damaged_trunc.png` is that screen). There is no error naming the real cause
and no way forward from it (see M2).

I narrowed the boundary by seating one hostile name at a time:

| Name | UTF-8 bytes | Result |
|---|---|---|
| `Vera` | 4 | lobby renders (`r_name_control.png`) |
| `Вера` | 8 | lobby renders (`r_name_cyr.png`) |
| `Konstantinos` | 12 | lobby renders (`r_name_13.png`) |
| `Konstantinoss` | 13 | **fails** (`r_name_13ascii.png`) |
| `Владимир` | 16 | **fails** (`r_name_vladimir.png`) |
| `Bartholomew Aloysius Featherstonehaugh` | 38 | **fails** (`r_name_long.png`) |
| `<script>alert(1)</script>` | 25 | **fails** (`r_name_script.png`) |
| `🃏🂡🂢🂣🂤` | 20 | **fails** (`r_name_emoji.png`) |
| `····` (spaces only) | 4 | **fails** (`r_name_ws.png`) |

The cap is 12 **bytes**, not 12 characters. That means:

- **Владимир - eight letters - does not fit.** So does Екатерина, Александр,
  Константин. This app ships a Russian localization for a Russian card game, and
  a majority of common Russian given names cannot be entered. Korean is
  three bytes per syllable, so a four-syllable Korean name breaks too.
- The failure is silent at the seal layer: `r_longnames.png` shows a lobby I
  seeded with four hostile names simply *not appearing* - the app fell through
  to the New game screen with no message at all.

Fix needs all three: a byte-aware cap enforced in the text field, a truncation
or rejection that the user can see, and an error path that does not claim the
link is damaged when nothing is wrong with the link.

### B2. The game-over leaderboard grows off the side of the screen as players are added

- 3 players: correct, inset, legible (`r_end3.png`).
- 5 players: the rank column is sliced off at the left edge - `#1`..`#4` are
  half-cut and the loser's label `Fool` reads as `ool` (`r_end5.png`).
- 7 players: **the entire name column is off-screen.** What is left is a blank
  orange slab with letter fragments (`a`, `k`, `s`, `va`, `(You)`) bleeding off
  the left edge. You cannot tell who won or who lost (`r_end7.png`).

Not device-specific - same failure on iPhone SE (`r_se_end5.png`), where the
"New game" button is also clipped by the bottom edge.

Root cause is visible from the outside and confirmed in
`MessageTableView.swift:1205-1238`: the ranking is a `ZStack { WoodFill(); rows }`
constrained with `.frame(height: plankHeight)` only. `WoodFill` is an
aspect-**fill** image, so its proposed width scales with the height it is given.
More players -> taller plank -> wider plank -> overflows the screen and gets
centred, clipping both ends. It needs `.frame(maxWidth: .infinity, height:
plankHeight)`.

### B3. Dynamic Type makes the cards unreadable - the game becomes unplayable

At Accessibility XXXL (`r_a11y_take.png`), every playing card loses its rank
entirely and the suit pip scales *past the card's own bounds*, so detached
hearts, spades and diamonds float over the tabletop above and below each card.
There is no way to tell a 7 from a King, in hand or on the table. The "Pickup"
button label is clipped to "ickup" and has a stray club pip sitting on it.

Card faces need to opt out of Dynamic Type (`.dynamicTypeSize(...)` clamp) or
use geometry-relative type, and the pip needs clipping to the card.

### B4. The game-over list collapses at large text

Same setting, 5 players (`r_a11y_end5.png`): rows are pinned to a fixed row
height, so the names overlap each other line-on-line, the last row is sliced by
the plank's hard clip, and the rank column has degenerated to a pair of
truncation dots off the left edge. Combined with B2 this screen fails at both
axes independently.

---

## Major issues

### M1. Leaving the name blank makes your name the word "You"

The join button renders literally **"Join as You"** (`r_spectator.png`), and the
empty-name path substitutes the localized string for "You" as the player's
name. The roster then shows a player called "You" one line above another player
marked "(You)". Two different meanings, one word, same screen. Blank should
either be rejected or fall back to the Messages participant name.

### M2. "This game link is damaged" is a dead end

`r_damaged_trunc.png` (truncated link) and `r_foreign.png` (a `foolish.cards`
URL that is not a game). Both land on a title, one line of body copy at 55%
black on a busy pink weave, and **nothing else** - no "Start a new game", no
retry, no dismiss. The only exit is closing the extension. Since B1 routes a
perfectly ordinary user action into this screen, it needs an action on it.

### M3. At 8 players the table collides with the seat badges

`r_board8_busy.png`: the row of covered battles is drawn straight through the
"Boris" and "Mila" name labels and over the top of their card-count badges. On
the SE (`r_se_board8.png`) it is worse - Boris's badge is behind a card - and
the player's own hand is clipped by the bottom edge of the screen. The layout
has no reserved gutters; it just stacks and hopes.

### M4. The hand compresses instead of fanning

`r_bighand.png` - 11 cards. Each card is squeezed to a ~44pt sliver with a
distorted aspect ratio; the mirrored bottom-right index is gone and only one pip
survives. Durak routinely leaves a defender holding 15-20 cards after two
pickups, which puts card width well below Apple's 44pt minimum hit target - and
these are drag sources, not taps. Fan or overlap them.

### M5. Nothing on the board tells you it is your turn or what you may do

`r_endgame.png`: it is my move, the table is empty, and the screen offers no
button, no prompt, no highlight - just my hand and a lot of tabletop. The only
way to attack is to drag a card, which is never stated anywhere. `r_take.png`
offers a single "Pickup" button while covering (the other legal option) remains
a hidden drag gesture. First-time players will be stuck.

### M6. The message bubble does not say what happened

`r_compact.png`, top half - what actually lands in the thread is an unlabeled
pink rectangle captioned "Foolish", inside which the trump card is drawn on top
of a seat's card-count badge. In a group thread this bubble is the entire
notification surface for a turn-based game, and it carries no text: not whose
turn it is, not what the last player did. Compare any turn-based iMessage game -
the bubble is the product.

The compact drawer below it is the phone-sized board with the bottom cropped
off, not a summary built for the drawer: two of the four seats and every action
control are simply outside the crop.

### M7. Any lobby member can start the game on everyone else

`r_lobby_partial.png` - 3 of 8 joined, and "Start game" is offered to Boris, who
joined last. One tap and the other five people in the chat are locked out with
no warning and no confirmation. The screen also never states the capacity ("3 of
8"), and offers no invite affordance at this state.

### M8. Trump is communicated by a sliver of a card

On every board the trump is a card peeking out from *under* the deck stack
(`r_board8_busy.png` - the Q♦ is half-hidden behind the "4" badge). When the
talon empties, that becomes a bare unlabeled ♠ glyph floating in the top-left
corner (`r_endgame.png`). Trump suit is the single most important piece of state
in Durak and it has no first-class indicator.

---

## Minor issues

- **m1. Body copy is set at 55% black over a high-frequency pink/tan weave.**
  "Your name" (`r_chatswitch.png`), the damaged-link line, the lobby's seat
  numbers - all sit near or below the contrast floor. The weave is a procedural
  texture with a hot-pink thread mixed in (`WoolTexture.swift:139-155`), so the
  background luminance changes every few pixels; no fixed-opacity text will
  survive it. Text needs a plate or a scrim.
- **m2. Dark mode is ignored.** `r_dark_take.png` is pixel-identical to the light
  shot. A full-bleed hot-pink panel inside a dark Messages thread at night is
  jarring, and the extension does nothing to soften it.
- **m3. Secondary buttons are effectively invisible** - white text on a clear
  fill over the weave (`r_seatpick8.png`). That particular screen is
  `#if DEBUG`-gated (Release correctly shows a spectator board instead of a seat
  picker, which is the right call), but the button style is shared.
- **m4. Unexplained iconography.** Small crossed-sword glyphs and a grey shield
  appear next to seats on every board with no legend and no accessible
  explanation. At the size they are drawn, on this background, they read as
  smudges.
- **m5. Most of the table is empty.** Content is banded at the top and bottom
  with a large dead zone between (`r_endgame.png`, `r_bighand.png`), while the
  parts that do have content are the parts that collide (M3).
- **m6. VoiceOver labels are hard-coded English.** `FDeckWell`, `FBattleGrid`,
  `FSeatBadge`, `FCard` and `MessageTableView` all pass English string literals
  to `.accessibilityLabel` ("You attack first", "N cards discarded"), while all
  116 visible strings go through the localization table. A Russian or Korean
  VoiceOver user gets an English board. The labels themselves are otherwise
  well-chosen - this is a wiring problem, not a missing-feature problem.
- **m7. No way to choose the game size.** Capacity is implicit (2 in a DM, 8 in a
  group) and never surfaced. Five friends in a chat of eight cannot say so.
- **m8. `#1` is pale gold on an orange plank** (`r_end3.png`) - the winner's rank
  is the least legible thing on the results screen.

---

## Needs verification on real devices (not called a defect)

**Opening an older lobby bubble showed a stale roster and offered me a seat I
already held.** `r_oldbubble.png`: a thread with two lobby bubbles (Alex+Vera,
then Alex+Vera+Boris+Dima). Tapping the *older* one as Alex showed the 2-person
roster and a lit "Join as Alex" button - a join that would seat a second "Alex".

I am flagging this as unconfirmed on purpose. My rig produced two lobby chains
carrying the same game id and identical parent hashes, which gives the chain
ranking nothing to order them by, and the receiving "device" had an empty seat
cache where a real creator's device would have one. Either of those could be
the whole explanation. It is worth a two-device check because the failure mode
(duplicate seat, stale roster) is expensive if it is real.

---

## What worked

Credit where it is due - these were all things I actively tried to break.

- **Conversation scoping holds.** Staging a game in one thread and switching to
  another lands cleanly on New game; no board leaks across threads
  (`r_chatswitch.png`).
- **Corrupt input is handled, not crashed.** A truncated payload, a payload with
  the right prefix and wrong length, and a non-game `foolish.cards` URL all
  produce the damaged screen rather than a crash or a garbage board
  (`r_damaged_trunc.png`, `r_foreign.png`). My complaint is about the dead end
  (M2), not the detection.
- **Release refuses to offer a seat picker** on an ambiguous identity and shows a
  public spectator board instead, so a receiver cannot claim someone else's hand
  and read it. That is the correct security posture and it is deliberate in the
  code.
- **Dismissing the drawer entirely leaves a normal, usable chat** with the compose
  bar and the "+" app drawer intact (`r_dismissed.png`) - no trapped state.
- **Visible strings are fully localized** across en/ru/ko, all 33 iMessage keys
  present in each - which is exactly why B1 and m6 stand out.
- **The lobby scales with Dynamic Type** (`r_a11y_lobby.png`) - it is the one
  screen that does.

---

## Verdict

Not approvable as submitted. **B1** alone blocks it: a user typing an ordinary
Russian name into a Russian card game is told their game link is damaged. **B2**
and **B3/B4** are visible-on-first-launch layout failures at supported player
counts and supported text sizes.

The underlying game logic held up under everything I threw at it - malformed
links, stale bubbles, cross-thread switching, 8-way tables. The failures are all
in the layer above it: input validation, layout constraints, and telling the
player what is going on.
