# Pick 'Em Up - visual decisions made on the owner's behalf

Every visual choice in `UI.html` that the owner did not dictate.
Each one can be vetoed on its own.
Rule decisions live in `RULES_AND_KERNEL.md` (Dn), orchestration in `ORCHESTRATION.md` (On).

## Materials and layout

DECISION U1: the table uses foolish's shipped materials exactly: classic felt rgb(40,102,70) with cloud and grain noise and the TableBackground vignette (dark felt rgb(20,56,39) in the dark scheme), cream #F4EFE6 card faces with the locked deep-red #8B1A1A 1pt edge, Georgia bold ranks, foolish's fern card back, and wood pills and squares with square corners.
Alternative: the teal-green gradient and rounded teal pills the first draft of the study used.
Why: the owner asked for the felt and the look lifted from foolish; the first draft's felt and pills were not foolish's, only near it.

DECISION U2: the pile keeps foolish's board centre in the expanded view, and in the 340pt drawer it is lifted 24pt, exactly enough to clear a row of two pills.
Alternative: keep the pile at the board centre everywhere and let the pills overlap its lower corner.
Why: two pills (Draw plus Pass or Play) are common in this game, unlike foolish, and a pill over the pile hides the card being matched.

DECISION U3: the deck is a portrait stack of 50 x 70 backs 10pt to the left of the pile, centred on the pile's line, with foolish's FDeckWell layer counts and lean, and the count ("27 left", D22) printed on its top layer, the way foolish centres its count chip on the stock.
Alternative: a label under the deck, or foolish's landscape 66 x 46 stock.
Why: "immediately left of the pile" (1.3) reads as two piles side by side only if both are portrait; a label under the deck collided with the left square in the drawer.

DECISION U4: the corner freed by foolish's old deck well holds the status line: `HEAD_*` (15pt) and one `SUB_*` line (12pt), 128pt wide, top-anchored.
Alternative: no status line, as foolish has none.
Why: section 6.3 defines screen lines and they need a home; the corner is the only space every layout leaves empty.

DECISION U5: whose turn it is shows as a brass #D8B24A bar under that seat's fan and the name in brass; my own turn shows as the status line and the lit Draw pill.
Alternative: foolish's convention, where only role marks say whose move it is.
Why: this game has no roles, so without a new mark nothing would say who is up.

DECISION U6: no numeral on any fan, ever; every back in a fan is drawn, with its deep-red edge showing, at foolish's 10pt step, compressing (minimum 3pt) to keep the fan within 96pt.
Alternative: foolish's uncapped fan with a count chip.
Why: D22 and the owner's rule; the red edges make the number countable by a careful eye, which is the intended skill, and the cap stops a 20-card fan running into the next seat.

DECISION U7: the expanded hand follows O4 exactly (one row, foolish's two rows, overlap to a 16pt strip, then a horizontally scrolling two-row strip); the 340pt drawer keeps ONE row and goes straight from flat to overlap to scroll.
Alternative: O4's two-row box in the drawer as well.
Why: a 166pt box in a 322pt board sits on the pile and the side seats; this narrows O4 for one height only and should be confirmed by the orchestrator.

DECISION U8: overlapped cards keep a 40pt full face so each 16pt strip shows its corner index; flat cards under 40pt use foolish's thin face (rank over suit, no corners).
Alternative: overlapped thin faces.
Why: a thin face shows nothing in a 16pt strip.

DECISION U9: pills sit in a row, not foolish's column: Draw holds the trailing slot whenever a draw is legal and never moves; the slot to its left is Play (a card selected), Pass (drew this turn) or Undo (a play staged); Undo alone takes the trailing slot.
Alternative: foolish's 128 x 88 column.
Why: in the drawer the column's upper pill lands on the right seat's fan; a row clears it, and a fixed Draw builds muscle memory for the move made most often.

DECISION U10: the staged line lives inside the status corner and is chips only: a back chip with a count for draws, a riffle glyph for a reshuffle, a mini card for the play.
Alternative: a line of words under the pile (the first draft).
Why: under the pile it collided with the pills in the drawer, and the words are already in the staged bubble's caption in the compose field.

DECISION U11: the Last card! pill replaces the two squares on the left while the player is exposed, with amber lettering on the same wood.
Alternative: a third pill on the right, or a stamp-style button on the hand.
Why: it is the one control that can appear out of turn, so it must not share the turn pills' slots; the squares are the least-used controls on the board.

DECISION U12: the slot under a badge holds only a player's own speech or a verdict: LAST (cream), Caught you! (red), Wrong call (grey), OUT (brass). Nothing the app computes about a hand ever appears there.
Alternative: foolish's role row, or a LAST stamp placed by the app.
Why: 1.8 and D22; the owner said nothing may announce one card left.

DECISION U13: a staged catch shows the target fan pressed to .95 with a brass ring and a red "Caught you!" tip over it; the verdict is never previewed.
Alternative: a confirm sheet before staging.
Why: the owner's input is a tap on the fan; a confirm step would add a second tap to the one simultaneous-action moment in the game.

DECISION U14: the suit picker is four 60pt tiles, shape and colour and word, popped out of the wild to the compass points around the pile, with a .55 scrim and an x; picking dismisses it, the x or a scrim tap cancels and the card flies home on the Undo flight.
Alternative: a row of tiles over the pile, or a bottom sheet.
Why: no tile lands under the thumb that released the card, nothing asks Messages for a presentation change, and a cancel leaves nothing staged (D17).

DECISION U15: a chosen wild carries its suit as a band of that colour along its foot, plus the halo.
Alternative: recolour the whole card.
Why: the four-shape wild face stays recognisable as a wild, and the halo already carries the live suit for a glance.

DECISION U16: buried start cards (D14) peek face up from under the deck's lower edge, tilted, for the rest of the game.
Alternative: hide them in the deck.
Why: they are public, and it reuses the place foolish's trump peeked from; it is a small joke every time anyone looks at the deck.

DECISION U17: the transcript bubble is foolish's 300 x 195 BubbleSnapshot showing the public table with every hand as a countable fan, the sender's included, with no hand counts; the finished game's bubble shows every hand face up.
Alternative: the 274pt card the first draft drew.
Why: 300 x 195 is what the shipped app bakes; the first draft's size and its card counts were both wrong.

## Motion

DECISION U18: a play is foolish's flight: 500ms, timingCurve(.25,.46,.45,.94), ghost bulge 1.15, 25ms gap between steps.
Alternative: the 420ms the first draft proposed, which RULES_AND_KERNEL 5.2 quotes.
Why: foolish's number is the one its rig measured and its players already know; 5.2 should be updated to 500ms.

DECISION U19: a draw is its own shorter flight, 320ms with a 1.08 bulge, then a 180ms flip; replayed and penalty draws start 110ms apart, one card per flight, never several in one flight.
Alternative: foolish's rule that cards in one event fly together.
Why: every draw is its own kernel step here, and a quantity is being told; 110ms matches 5.2's PENALTY_DRAW.

DECISION U20: the deal is one 320ms flight per card, round-robin, start-to-start clamp(1800ms / cards, 45, 110), after a two-riffle shuffle; own cards flip where they land.
Alternative: a fixed stagger.
Why: a fixed 110ms makes an eight-player deal take 6s; the clamp keeps every table between 3.2s and 4.2s end to end.

DECISION U21: the reshuffle gag is gather (360ms ease-in each, 40ms apart), fatten (.7 to 1.14 to 1, 240ms), riffle twice (2 x 140ms), then the waiting draw.
Alternative: a plain fade of the pile into the deck.
Why: the owner asked for comedic reshuffles, and each beat is short enough that a five-draw turn still plays in under four seconds.

DECISION U22: stamps (LAST, Caught you!, Wrong call, OUT) are 340ms from 2.4x with an overshoot; skip is a 260ms red slash plus a 900ms dim round trip; reverse is foolish's TURN on the direction box (170 + 170ms); REVEAL flips each back 60ms apart.
Alternative: foolish's role-mark motions.
Why: there are no roles; these are the smallest set that gives every settle event a distinct, named motion.

DECISION U23: an undo of a draw is refused with three 60ms shakes of the newest drawn card and SUB_DRAWN_STAY in the status line.
Alternative: silently ignore the tap.
Why: D8 makes the refusal a rule, and a refusal that shows nothing looks like a bug.

## Drag

DECISION U24: dragging off the deck uses the same SwiftUI DragGesture(minimumDistance: 0) foolish puts on hand cards, attached with highPriorityGesture, and foolish's hand band (hand frame grown 64 up, 24 down) as the drop target.
Alternative: a UIKit pan recognizer with a delegate that blocks the host's gestures.
Why: it is foolish's proven gesture; whether a downward drag starting mid-board can ever trigger Messages' collapse is not answerable in HTML and is listed as an open item to prove on a device.

## Words

DECISION U25: the two call words are written once each, as `--callword` ("Last card!") and `--catchword` ("Caught you!") in the stylesheet; every pill, tag, tip and staged line on the page reads them.
Alternative: literal strings.
Why: the owner asked for the word in one place so it can be swapped; the rules worker chose "Last card!" (D2).

## Appendix A of RULES_AND_KERNEL.md, as applied

1. Seat counts removed everywhere (U6, U12, U17). Applied.
2. "Your count already reads 8" is gone with the old bubble view. Applied.
3. "Alex has one card left." and the "One left" screen are gone; LAST appears only after saying it. Applied.
4. "Bo is holding two wilds" is gone. Applied.
5. Captions are 6.2's lines, never "you", no full stop. Applied.
6. The deck sits immediately left of the pile (U3). Applied.
7. Tap only selects; Play and Draw pills, deck tap and both drags drawn. Applied.
8. Pass pill after a draw (U9) and a Last card! pill (U11). Applied.
9. Undo shows drawn cards staying (U23, SUB_DRAWN_STAY). Applied.
10. No picker for a last-card wild (picker view 05). Applied.
11. The Conflict row covers out-of-turn Last card! and Caught you! races with the LOST_RACE_* lines. Applied.
12. No direction word at 2 players (table 04, transcript 08). Applied.
13. "One fork worth settling" replaced by D4's next-completed-turn window, with the reason. Applied.
14. The grid has rows for draws, the reshuffle triple, the round-robin deal, buried start cards, the catch (aim at stage, verdict at Send) and the end reveal, and an index maps every PK_EV kind to its row. Applied.

ORCHESTRATION O4 applied in the expanded view (table 04 to 07, rulers 03); narrowed for the drawer by U7.
