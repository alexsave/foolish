# Pick 'Em Up - simulator verification

The record of playing Pick 'Em Up inside Messages on a simulator, step by step, with the screenshot each step left in `pickemup/docs/shots/`.

## Status on 2026-09-27, afternoon: played inside Messages

A fresh iPhone 17e `pk-b2` (iOS 27.0) booted in 43 seconds, `simctl launch com.apple.MobileSMS` returned in about a second, and the extension opened from the + menu in the stub thread; B2's hang did not come back.
One simulator gives the extension ONE participant id in every thread, so the rig's two-thread trick seated Alex twice; a Debug-only dev file, `dev.persona` ("1 Bo"), now makes an appex process another person (IOS_DECISIONS I41).
Alex plays in John Appleseed's thread and Bo in Kate Bell's: a bubble sent in one arrives in the other, and the rig's `leave` between them ends the appex so the persona file is read fresh.
The screenshots are in `shots/` (585 x 1266, or 468 x 1013 where the full size passed 400KB); `table_start.png` was saved with a 256-colour palette before that rule, so its ink is slightly off.
What one simulator cannot reach: a group thread (the simulator can only make a group as SMS, where the + menu offers no app), so nothing with three or more seats; and the catch, Last card! and the win, which need a hand played down to one card, many round trips away (Alex held 88 cards after the reshuffle test).
Those steps stay proven by the bridge and owed on a phone or a longer session.

Two defects were found and one is fixed:
- FIXED (I42): every flight to or from the pile (a play, the start card, a bury, a reshuffle's gather) flew to the board's top-left corner and snapped onto the pile at its end; the pile's anchor was moved into place with `.offset`, which its GeometryReader does not see, so it reported (0, 0). `AnchorTests` hosts a real `TableScreen` and was seen red on exactly that frame (41, 57.5 against 187, 317.5).
- OPEN, needs the owner (ORCHESTRATION B2): the compact drawer on an iPhone 17e is too short for the table as laid out. The pill row's inner pill (Undo or Pass beside Draw) is drawn over the pile, the deck's layers cover the status corner's sub-line when the staged strip is up, and the pile touches the top seat's fan (`compact_collision.png`, `skip_staged.png`). U2's 24pt lift was sized for a 340pt drawer; this board is about 299pt. See the B2 entry for the options.
- OPEN (MOTION_REPORT): an opened bubble's plan starts while the drawer is still white, so its first few hundred milliseconds play unseen.

## What was done without a simulator

`make -C pickemup/c ios-lib` built the xcframework and readers (layout 0xfcaaa7d7).
`xcodegen generate` left `DebugAppGroup.entitlements` untouched (`git status --short -- '*.entitlements'` empty).
`PickemupKitTests` builds for testing and `PickemupMessagesApp` builds, both for the iOS Simulator SDK 27.0.
O6 is in the card face (IOS_DECISIONS I27): Skip, Reverse and +2 print their suit's shape under the corner index, top-left and turned round at bottom-right, and say it through the kernel's `SUIT_ONE_n` word.
Fidelity fixes against UI.html, read from its CSS rather than from a screenshot:
- the staged strip's chips are the glyph alone, no index and no pip, radius 2 (`.strip .chip .cf`, I28);
- the strip's reshuffle mark is the study's two hooked arrows (`svg.rf`, 14pt, `#e3c985`), not a system symbol;
- the status corner spaces the strip 5 above and 1 below and the sub-line 3 below it (`.strip`, `.ss`);
- the badge stamps LAST, Caught you!, Wrong call and OUT share `.said`'s face (heavy 11pt, 0.06em) and carry its 10 x 5 tail pointing at the fan, 2pt into the slot (`.rrow`); LAST and OUT had been 10pt with wide tracking and no stamp had a tail;
- the suit picker's tiles have the lit top edge (`inset 0 1px 0 rgba(255,255,255,.2)`) and the label's 0.02em tracking.

## The game still to play, and the screenshot each step owes

Each line is one step of the two-seat game, the screenshot it should leave, and what to check in it.
The "Bridge" note under each step says where `pickemup/c/tests/pk_twophone_test.c` proves the same step through the entry points Swift calls (`pk_api.h`), without a simulator.
The tag in brackets is the step name the test prints beside every assertion of that step, so a red names it.
The screenshot is still owed for every step: the test proves the kernel and the bridge, not the pixels.

- Open the extension from the + menu in a fresh thread: `lobby_empty`, "Only you so far" and no button (I8).
  Bridge: proven in pk_twophone_test.c `[S1 lobby]` (seat 0, alone, WAITING, the invitation's caption).
  Seen: `name_gate.png` first (a fresh simulator has no name), then `lobby_empty.png` and `bubble_invite.png`: "1. Alex (You)", "Alex deals", "Waiting for players", no button, the invitation staged with "Alex wants a game of Pick 'Em Up. Tap to join".
  The line is "Waiting for players", not "Only you so far", and that is the kernel's rule: the creator sent the newest bubble, so `pk_lobby_offered` answers WAITING; INVITE ("Only you so far") is for a seat left alone after someone leaves. This step's expectation was the imprecise part.
- The second seat joins, which also starts the game (I7): `lobby_two` before, `table_start` after, seven cards each, the pile's first card on its halo.
  Bridge: proven in pk_twophone_test.c `[S2 join starts the game]` (JOIN offered with join-and-start, a live game, seven each, the deal's caption) and `[S2 the deal on the receiver]` (the other phone's bubble 0: fourteen DEALs round robin from seat 1, the other hand masked, a number START_CARD).
  Seen: `lobby_two.png` (Bo's phone: "1. Alex", Join), `deal_mid.png` (1.2s after Join, the board fading in, deck at 104), `table_start.png` (seven cards, "Your turn", the start card on its halo, Alex's fan a constant three backs, "89 left"). The deal is filmed in `shots/motion/` (MOTION_REPORT).
- Tap a card to select it: `hand_select`, the 4pt red ring and nothing staged.
  Bridge: nothing to prove; selection is the host's alone and stages nothing in the kernel.
  Seen: `hand_select.png`, the red ring on the 7 of diamonds and Play beside Draw, nothing staged.
- Drag a card sideways within the hand (O9, I38): `hand_reorder`, the card pinned under the finger while the others slide apart, then settled in its new slot; let go in the row and nothing is played or staged.
  Then draw (the new card lands at the right end), play the dragged card by dragging it onto the pile (the flight leaves from the slot it sits in), undo (it flies home to that same slot), and close and reopen the extension (the order is still the dragged one).
  Also owed on a real phone in the compact drawer: a sideways drag on the one row never plays a card and never collapses the drawer.
  Bridge: proven in `pickemup/c/tests/pk_arrange_test.c` (`[bridge: ...]` steps: a reorder, a draw and a play after it, undo, a received bubble, the record's save and restore, a corrupted record, the frames) and in `ArrangeTests`, now run and mutation-checked (`pickemup/ios/TESTS_MUTATED.md`).
  Seen: `hand_reorder.png`, the 1 of circles dragged from the first slot to the fourth and settled there, nothing played or staged; the three draws after it landed at the right end (`after_draws.png`). Undo, close and reopen after a reorder, and the compact-drawer drag on a phone, were not played.
- A turn with three draws then a play, one by drag onto the pile: `drag_to_pile`, then `chips_after_draws` with the strip reading a back x3, a dot and the played card's chip.
  Bridge: proven in pk_twophone_test.c `[S4 three draws then a play]` (the draft plan is three DRAWs then the PLAY) and `[S4b undo a staged play]` (the play comes home, the drawn cards stay, D8); the undo that empties a draft is `[S5 an undo that empties the draft]`.
  Seen: `after_draws.png` (ten cards in two rows, the strip a back x3, Pass beside Draw), `drag_to_pile.png` (the 7 of diamonds on the pile, Undo), `chips_after_draws.png` (the drawer collapsed, the strip a back x3, a dot and the 7's chip, the staged bubble "Bo drew 3 and played 7 of diamonds"). In the collapsed frame the deck covers the strip's sub-line: the OPEN compact defect above.
- Play a wild: `suit_picker`, four tiles at the compass points over the scrim, then the chosen suit's band on the pile.
  Bridge: proven in pk_twophone_test.c `[S6 a wild with a suit]` (the host is told a suit is needed, no suit is refused, the chosen suit is live).
  Seen: `suit_picker.png` (a wild +4 selected and played with Play, since a scrolling hand has no drag to play, I11: four tiles at the compass points over the scrim, the x beside), `wild_chosen.png` (triangles chosen: the gold band on the pile, the sub-line "triangles", the caption "Alex played wild +4, now triangles. Bo draws four").
- Play a +2, a Skip and a Reverse (a Skip at two players, D13, so no direction box at two).
  Bridge: proven in pk_twophone_test.c `[S7 a +2]` (two automatic PENALTY_DRAWs for the victim), `[S7 a Skip]` and `[S7 a Reverse]` (no direction change, `show_dir` 0).
  Seen: the Skip only, `skip_staged.png`: "Alex skipped Bo. Alex to play", Alex's turn again after Send, no direction box. The +2 and the Reverse were not played.
- Draw until the deck is empty and once more: `reshuffle_state`, the riffle mark in the strip and the deck count back up.
  Bridge: proven in pk_twophone_test.c `[S8 reshuffle]` (the pile collapses to its top card and suit, the deck refills, the reshuffle triple before the draw on the receiver).
  Seen: 86 draws emptied the deck (a dashed "0 left" well), and the 87th reshuffled: `reshuffle_state.png`, the strip x87 with the hooked riffle arrows, the pile down to its top card, "1 left". Bo opened that bubble (87 draws, the reshuffle, the +4 and the penalty draws) and his board settled on "Waiting on Alex" with his two penalty cards.
- A player plays down to one card and sends it; the other taps that fan: `catch_staged`, the amber ring, the fan at .95 and the red tip.
  Bridge: proven in pk_twophone_test.c `[S8b down to one]` (no "Last card!" in the exposing bubble, D3), `[S8c Caught you! staged]` (the draft holds the tap and no outcome, D5d) and `[S8d Caught you! after Send]` (two cards).
  Seen: not played (no hand got near one card); still owed.
- The exposed player says Last card! in a later bubble, and the LAST stamp shows under their badge.
  Bridge: proven in pk_twophone_test.c `[S9 Last card! in a later bubble]` (said out of turn, the stamp is public, the fan can no longer be caught) and `[S11 Rule P]` (saying it beats a rival catch of the same bubble, and a stale bubble loses to the newer).
  Seen: not played; still owed.
- The win: `end_reveal`, every hand face up, the plank with the headline, the results order and Again.
  Bridge: proven in pk_twophone_test.c `[S12 the win]` (every hand revealed, the ranking, the results rows, both headlines); each phone resolving to its own seat is `[S13 seat resolver]`.
  Seen: not played; still owed.
- The transcript: `bubble_transcript`, the 300 x 195 bubbles with the kernel's captions.
  Bridge: every bubble's caption, staged and received, equals section 6's words in every step above; the test prints the transcript of the non-filler bubbles.
  Seen: the staged bubbles in `bubble_invite.png`, `chips_after_draws.png`, `skip_staged.png` and `wild_chosen.png`, each a board picture with the kernel's caption. Incoming bubbles show only their caption on the iOS 27 simulator, and Messages gives an older bubble's collapsed line the newest caption; both are the simulator defects the rig README records (12 and the iOS 27 note), not this product's.
