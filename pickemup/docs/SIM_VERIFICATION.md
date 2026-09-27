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
- Open the extension from the + menu in a fresh thread: `lobby_empty`, "Only you so far" and no button (I8).
- The second seat joins, which also starts the game (I7): `lobby_two` before, `table_start` after, seven cards each, the pile's first card on its halo.
- Tap a card to select it: `hand_select`, the 4pt red ring and nothing staged.
- A turn with three draws then a play, one by drag onto the pile: `drag_to_pile`, then `chips_after_draws` with the strip reading a back x3, a dot and the played card's chip.
- Play a wild: `suit_picker`, four tiles at the compass points over the scrim, then the chosen suit's band on the pile.
- Play a +2, a Skip and a Reverse (a Skip at two players, D13, so no direction box at two).
- Draw until the deck is empty and once more: `reshuffle_state`, the riffle mark in the strip and the deck count back up.
- A player plays down to one card and sends it; the other taps that fan: `catch_staged`, the amber ring, the fan at .95 and the red tip.
- The exposed player says Last card! in a later bubble, and the LAST stamp shows under their badge.
- The win: `end_reveal`, every hand face up, the plank with the headline, the results order and Again.
- The transcript: `bubble_transcript`, the 300 x 195 bubbles with the kernel's captions.
