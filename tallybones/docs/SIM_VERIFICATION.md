# Tallybones inside Messages - the simulator run

Run on 2026-09-27 by the integration worker, on branch `tb-integrate`.
It is a real run of the extension built from the kernel, inside Apple's Messages app, on an iOS simulator.
Both sides of the game were played, but on ONE simulator standing in for two people (see "How two people fit on one simulator"), because the Mac's two-simulator cap was shared with another agent's simulator the whole time.

## The device

- Simulator `TallybonesRig` (29FC952A-1D12-4E16-903B-14C46DEEC016), made by `rig.sh newsim`, the rig's 6.9" iPhone at 440 x 956 points, iOS 27.0 runtime (24A434), Xcode 27.
- Booted only after `xcrun simctl list devices | grep -c Booted` read 1 (another agent's `iPhone 17e` was the other); shut down at the end with `xcrun simctl shutdown`, leaving only that one booted.
- Messages' two stub threads: "+1 (888) 555-1212" (JA) played as Alex, "+1 (555) 564-8583" (KB) played as Bo. A bubble sent in one lands, as incoming, in the other (the iOS 27 direction of the rig's README).
- Debug build, light appearance, the rig's 9:41 status bar.

## The commands

```
source tallybones/ios/Tools/rig.env
xcrun simctl list devices | grep -c Booted                 # 1: room for one
eval "$(foolish/ios/Tools/rig/rig.sh newsim TallybonesRig)"
export RIG_SIM=29FC952A-1D12-4E16-903B-14C46DEEC016
foolish/ios/Tools/rig/rig.sh build     # make -C tallybones/c ios-lib, xcodegen, xcodebuild Debug, install
git status --short -- '*.entitlements' # empty after every build
foolish/ios/Tools/rig/rig.sh stage light
foolish/ios/Tools/rig/rig.sh enter
foolish/ios/Tools/rig/rig.sh open      # + -> Tallybones: the extension, compact
```

Then, for each step, a switch of person, taps and screenshots.
The switch is `printf Alex > <App Group>/dev.who` (or `Bo`) and `rig.sh killappex`; taps are `idb ui tap --udid $RIG_SIM X Y` in device points; Send is found by its accessibility label (`idb ui describe-all`, the Button labelled `Send`); screenshots are `xcrun simctl io $RIG_SIM screenshot`, shrunk to 460 x 1000 and 128 colours for `docs/shots/`.
The drawer was pulled down with `idb ui swipe` when Messages' `Send` was under an expanded drawer.
The driver was a throwaway script in the session's scratchpad; every step it took is the list above.

## How two people fit on one simulator

One simulator is one Messages identity: in both stub threads its `localParticipantIdentifier` is the same and every bubble reads as sent by this device.
The first attempt showed it: with the seat records wiped and the nickname changed to Bo, opening Alex's invitation in the other thread still seated the phone as "1. Alex (You)", waiting for players (the kernel's resolver matched the tag and the sender fact).
So a DEBUG-only dev file was added (DECISIONS T65): `dev.who` names the person, and the extension then uses that name as the nickname, a participant id derived from it, that person's own seat records, and no sender fact.
Everything else is the shipping code path: the same kernel, the same MessagesViewController, the same MSMessage sends, and every link a real `MSMessage.url` carried from one thread to the other by Messages.
What this does NOT prove is the resolver against two real, distinct `localParticipantIdentifier`s and a real sender fact; that path is covered only by the C and Swift two-phone tests.

## Game 1 (the build before the T66 fixes)

| Step | Screenshot | Seen |
|---|---|---|
| Alex opens the app from the + menu | `01_lobby_invite_staged.png` | The lobby, "1. Alex (You)", "Waiting for players"; the invitation staged in the compose field: the roster picture and the kernel's caption "Alex wants a game of Tallybones. Tap to join". |
| Send | `02_invite_sent.png` | The invitation on the right, Delivered. |
| Bo opens it in the other thread | `03_bo_join_offered.png` | "1. Alex" and a Join button: Bo is not seated, the kernel offers Join. |
| Bo taps Join | `04_bo_started_table.png` | In a DM, Bo's join fills the table and starts the game: the table replaces the lobby at once (this was a defect on the first try, the screen stayed on an empty lobby; fixed, T66), Alex's badge on turn, roll 1 of Alex's turn on the tray: 6, 4, 5, 3, 4, "Waiting on Alex", "Alex is on roll 1 of 3". |
| Bo sends the start | `05_bo_start_staged.png`, `06_bo_start_sent.png` | The start bubble shows the same five dice and "Tallybones is on. Alex to roll". |
| Alex opens Bo's start bubble | `07a_alex_opens_start_midsettle.png`, `07_alex_roll1.png` | Alex's table: "Your roll", "Tap dice to keep. Two rerolls left", the same 6, 4, 5, 3, 4, and the card's previews from the kernel (Threes 3, Fours 8, Fives 5, Sixes 6, Short Run 30, Any 22). The first frame was caught mid-settle, dice turned and one showing a tumble face; a later frame showed all five at rest on 6, 4, 5, 3, 4. |
| Alex marks four dice | `08_alex_keeps_marked.png` | Brass rings on dice 1 to 4. |
| Alex taps Reroll | `09a_keep_staged_blank.png`, `09_keep_staged_compact.png` | The KEEP is staged: die 5 is BLANK in the tray (dashed, no pips) and in the bubble picture; "Send it to reroll", "The new dice come when it is sent"; the previews are gone; the drawer collapses; the caption is "Alex keeps 3, 4, 5, 6 and rerolls one", naming no rerolled value. |
| Send | `10a_keep_sent_settling.png`, `10_keep_sent_reroll.png` | Right after the send the tray is still blank; a moment later the reroll is there, die 5 is a 6, "One reroll left", the previews back; the drawer stayed up (T55). The sent bubble keeps its blank slot, as T11 says it must. |
| Alex taps Short Run (30) | `11a_score_tapped.png`, `11_score_staged.png` | The row in brass, "Send it to score", "Short Run for 30", caption "Alex rolled a short run, 30 points". DEFECT seen: the tray and the bubble picture showed five blank dice and the turn bar moved to Bo before the send. Fixed after this game (T66). |
| Send | `12a_score_sent.png`, `12_score_sent_after.png` | Short Run 30 filled on Alex's card, "Waiting on Bo", "Bo is on roll 1 of 3", Bo's roll 1 settled on 5, 4, 6, 2, 4. |
| Bo opens Alex's score bubble | `13_bo_thread.png`, `14_bo_opens_score.png` | Bo's table: Alex's badge reads 30, "Your roll", and the same 5, 4, 6, 2, 4 Alex's phone showed. |
| Bo taps Alex's badge | `15_bo_views_alex_card.png` | Alex's card read-only: Short Run 30, Total 30. DEFECT seen: the card panel stretched to the sheet's height with blank space under it; fixed (T66). |

## Game 2 (after the fixes: T66, the pill, the sheet)

A new build was installed, which ends Messages and its in-memory threads, so the game was played again from a new invitation.

| Step | Screenshot | Seen |
|---|---|---|
| Bo's join starts the game | `g2_04_bo_started.png` | The table at once, roll 1 for Alex on 4, 1, 1, 4, 3; no Reroll pill on Bo's phone, since it is not Bo's turn. |
| Alex opens the start | `g2_07_alex_roll1.png` | The same 4, 1, 1, 4, 3; previews Ones 2, Threes 3, Fours 8, Any 13. |
| Alex keeps the two fours and rerolls | `g2_09_keep_staged.png` | Three blanks, caption "Alex keeps 4, 4 and rerolls three". |
| Send | `g2_10a_settling.png`, `g2_10_reroll.png` | Blank just after the send, then 4, 6, 5, 4, 2. |
| Alex taps Fours (8) | `g2_11_score_staged.png` | FIXED: the tray and the bubble picture keep the scored dice 4, 6, 5, 4, 2, the turn stays with Alex, the row is brass; caption "Alex scored 8 in Fours. Bo to roll". |
| Send | `g2_12_score_sent.png` | Fours 8 filled, "Waiting on Bo", Bo's roll 1 on 4, 5, 5, 4, 2. |
| Bo opens it | `g2_14_bo_opens_score.png` | Alex's badge 8, "Your roll", the same 4, 5, 5, 4, 2. |
| Bo opens Alex's card | `g2_15_bo_views_alex_card.png` | FIXED: the panel is the card's height; Fours 8, Numbers 8, Total 8, Close. |

In both games every die the receiver saw equals the die the sender saw, by replay of the link alone.

## What was NOT seen

- The settle frame by frame: the screenshots catch the tray before a roll and after it, and once mid-flight; no film was taken or measured.
- Two real identities: see "How two people fit on one simulator".
- Past the first turn: no Bo turn, no third roll, no bonus, no game end, no LEAVE, no group of three or more, no race between two bubbles.
- The kernel's stamp and turn-bar beats (not drawn, T62), dark mode, VoiceOver, other phone sizes.
- The incoming bubbles' pictures: on the iOS 27 simulator an incoming game message shows only its caption pill, and within one MSSession Messages collapses older bubbles to caption lines (the rig's README, traps 6 and 12); the pictures were seen on the sending side.
- Cosmetic, noted and not changed: the enabled wood pill reads as a bright orange texture on the felt, the lobby's bubble picture leaves most of its felt empty under a one-line roster, and after a KEEP is sent the kept dice keep their brass rings through the next choice.
