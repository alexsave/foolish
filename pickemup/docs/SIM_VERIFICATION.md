# Pick 'Em Up - simulator verification

The record of playing Pick 'Em Up inside Messages on a simulator, step by step, with the screenshot each step left in `pickemup/docs/shots/`.

## Status on 2026-09-27: not played, the simulator would not boot

The one device this pass may use, iPhone 17e `FC7586CF-78D5-4E61-810E-9449B9AC6C5A` (iOS 26.3), was booted three times between 06:28 and 06:56 UTC.
Each boot stopped on the black spinner with `simctl bootstatus` at "Waiting on System App" past its 90 second watchdog (the first was watched for 7 minutes).
The details are under BLOCKED B2 in `ORCHESTRATION.md`.
With no booted device neither Messages, nor the rig, nor a fallback preview host could run, so no step below has been played and no screenshot exists yet.
The device is shut down.

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
- The second seat joins, which also starts the game (I7): `lobby_two` before, `table_start` after, seven cards each, the pile's first card on its halo.
  Bridge: proven in pk_twophone_test.c `[S2 join starts the game]` (JOIN offered with join-and-start, a live game, seven each, the deal's caption) and `[S2 the deal on the receiver]` (the other phone's bubble 0: fourteen DEALs round robin from seat 1, the other hand masked, a number START_CARD).
- Tap a card to select it: `hand_select`, the 4pt red ring and nothing staged.
  Bridge: nothing to prove; selection is the host's alone and stages nothing in the kernel.
- A turn with three draws then a play, one by drag onto the pile: `drag_to_pile`, then `chips_after_draws` with the strip reading a back x3, a dot and the played card's chip.
  Bridge: proven in pk_twophone_test.c `[S4 three draws then a play]` (the draft plan is three DRAWs then the PLAY) and `[S4b undo a staged play]` (the play comes home, the drawn cards stay, D8); the undo that empties a draft is `[S5 an undo that empties the draft]`.
- Play a wild: `suit_picker`, four tiles at the compass points over the scrim, then the chosen suit's band on the pile.
  Bridge: proven in pk_twophone_test.c `[S6 a wild with a suit]` (the host is told a suit is needed, no suit is refused, the chosen suit is live).
- Play a +2, a Skip and a Reverse (a Skip at two players, D13, so no direction box at two).
  Bridge: proven in pk_twophone_test.c `[S7 a +2]` (two automatic PENALTY_DRAWs for the victim), `[S7 a Skip]` and `[S7 a Reverse]` (no direction change, `show_dir` 0).
- Draw until the deck is empty and once more: `reshuffle_state`, the riffle mark in the strip and the deck count back up.
  Bridge: proven in pk_twophone_test.c `[S8 reshuffle]` (the pile collapses to its top card and suit, the deck refills, the reshuffle triple before the draw on the receiver).
- A player plays down to one card and sends it; the other taps that fan: `catch_staged`, the amber ring, the fan at .95 and the red tip.
  Bridge: proven in pk_twophone_test.c `[S8b down to one]` (no "Last card!" in the exposing bubble, D3), `[S8c Caught you! staged]` (the draft holds the tap and no outcome, D5d) and `[S8d Caught you! after Send]` (two cards).
- The exposed player says Last card! in a later bubble, and the LAST stamp shows under their badge.
  Bridge: proven in pk_twophone_test.c `[S9 Last card! in a later bubble]` (said out of turn, the stamp is public, the fan can no longer be caught) and `[S11 Rule P]` (saying it beats a rival catch of the same bubble, and a stale bubble loses to the newer).
- The win: `end_reveal`, every hand face up, the plank with the headline, the results order and Again.
  Bridge: proven in pk_twophone_test.c `[S12 the win]` (every hand revealed, the ranking, the results rows, both headlines); each phone resolving to its own seat is `[S13 seat resolver]`.
- The transcript: `bubble_transcript`, the 300 x 195 bubbles with the kernel's captions.
  Bridge: every bubble's caption, staged and received, equals section 6's words in every step above; the test prints the transcript of the non-filler bubbles.
