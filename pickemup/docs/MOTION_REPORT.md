# Pick 'Em Up - motion report

What the motion layer plays, measured where it can be.

## Filmed and measured (2026-09-27, iPhone 17e simulator, iOS 27.0)

Filmed inside Messages at normal speed with `xcrun simctl io <udid> recordVideo` (stopped with SIGINT, never slowed), two seats played through the Debug persona (IOS_DECISIONS I43), frames extracted 1:1 with their real times by `animation-measure`'s `extract_frames.sh`.
Times are milliseconds from the tap that starts the take (the recorder's first changed frame, which is the touch going down, so a plan that starts on the touch going up starts a little after it).
A region's "activity" is a frame-to-frame mean pixel difference above 1.5 inside that region; a landmark is a colour box tracked by `track_landmark.py` and scored by `motion_metrics.py` (jerk against a critically damped spring's floor at the same frame times, roughness as the sum of squared second differences).

What could not be filmed on one simulator: a LIVE arrival (the receiving thread is never on screen while the other thread sends, so every take is an opened bubble, with the 100ms open lead instead of the 16ms arrival lead), a three-seat deal (the simulator can only make a group thread as SMS, which offers no apps), and a reshuffle after the fix below (the one reshuffle take predates it).

### FOUND and FIXED: every flight to or from the pile went to the top-left corner

The first takes showed the start card rise from the deck to the board's top-left corner, turn over there and snap onto the pile; a rejected (Reverse) start card snap back to the corner and fly down into the deck; and an opened play fly from the fan to the corner and snap onto the pile (`shots/motion/deal_bury_before_fix_sheet.png`, frames 236 to 350).
The pile's anchor was moved into place with `.offset`, which its GeometryReader does not see, so it reported (0, 0, 82, 115) (IOS_DECISIONS I44).
After the fix every take below lands on the pile from where it left, and `AnchorTests` pins the anchor.

### Take A: the deal, two seats, the joiner's phone (the Join that starts the game), after the fix

60Hz median frame interval, 311 frames; `shots/motion/deal_2p_sheet.png`.
The kernel's plan for this shape (2 seats, viewer seat 1, a number start card; a scratch copy of `pk_beats_dump.c`) is 28 beats, 3948ms end to end with the arrival lead and 4032ms with the open lead: hold and fade to 736, two riffles 761 to 1153, fourteen deal flights 1153 to 2903 with the hand's flips ending at 2953, the start card's flight 3203 to 3703, halo and turn bar to 3948.

| Marker | Planned | Measured |
|---|---|---|
| the deal's hand phase, first card to last flip | about 1600 (1153 to 2953, of which the hand region sees each flight only as it arrives) | 1629 (1233 to 2862) |
| last hand flip ended to start card landed | 750 | 775 |
| the start card's face-up half (a 500ms flight that turns at its midpoint) | 250 | 275 (4.920s to 5.195s in the film) |
| the start card lands, from the tap | 3703 (arrival lead) to 3787 (open lead) | 3637 |
| the last visible change | 3948 (the turn bar's fade) | 3607 (the halo's tail and the bar's fade are below the difference floor) |

Every interval inside the plan is within 25ms of the kernel's; the take as a whole runs about 70ms ahead of the plan measured from the touch going down, which means the board's clock starts on the join itself and not on a later frame.
The start card's tracked face-up half (its right edge, 16pt of travel and the settle) scores jerk 24 against a spring floor of 22 and roughness 2: smooth.

### Take B: an opened bubble, three draws and a play, after the fix

Bo drew three and played the 6 of triangles by drag; Alex opened the bubble. The host rendered this take at a 29Hz median (a busier moment on the host), so it is not a smoothness measurement.
The play flies from Bo's fan straight down onto the pile (`shots/motion/take1_open_sheet.png`, frames 110 to 128). Its tracked face-up landing (the card's top edge, 34pt from 5.430s to 5.585s) scores jerk 141 against a spring floor of 92 and roughness 21, the excess from one repeated frame at 5.555s.

FOUND, OPEN: the drawer is white for about a second after the tap, and when the board's first frame appears the three draw flights are already in the air near Bo's fan (frames 74 to 83).
The plan's clock starts when the bubble is adopted, while the hosting view is still hidden until the drawer is up (`MessagesViewController`, uttt's `appeared` rule), so the first few hundred milliseconds of an opened bubble play unseen.
The fix belongs with the lifecycle owner: start the plan's clock at the first frame the board is visible, not at the adopt.
The same white second shows on every first open of a new appex process (about 3.5 seconds on the very first open after an install).

### Take C: the reshuffle, before the fix (stage channel, Alex's own draw)

86 draws emptied the deck and the 87th reshuffled (`shots/reshuffle_state.png`: the strip x87 with the riffle arrows, the pile down to its top card, "1 left"). Its gather flight leaves from the pile's anchor, which was the broken one then, so this take is not scored; re-filming it after I44 is owed.

## How to read the tables

Produced by `make -C pickemup/c beats-dump` (`tests/pk_beats_dump.c`) from real games: the kernel's plan events of one bubble, laid out by `pk_beats_build` exactly as a phone does.
Start and length are milliseconds from the moment the plan begins (16ms after an arrival, 100ms after a bubble is opened).
From and to are the `UI.html` anchor names the views report; `hand.i` is my card i, `fan.k` seat k's fan, where a flight lands at the fan's right end.
A back is a card flown face down, `back (38)` my own card 38 flown face down before it turns over; a bare number is a card flown face up.
Parts are staggered copies of one beat: the three gathered under-cards, the eight riffled layers.

## Numbers against the grid

| Take | Measured from the timeline | The grid's budget |
|---|---|---|
| 1: five draws, a reshuffle and a play, three players | 2618 ms end to end | U21: a five-draw turn with a reshuffle "in under four seconds" |
| 2: a three-player deal, opened | 4426 ms, of which the shuffle and deal are 2586 ms (845 to 3431) | U20: the shuffle and deal "under 4.2s end to end" (corrected from "between 3.2s and 4.2s", see the FOUND in ANIMATION_DECISIONS.md) |
| Every eight-player arrival in 400 played games | p50 806 ms, p99 about 2.9 s | U21's four seconds, held at the p99 by `pk_beats_test` |

Every row of the grid is pinned by `pickemup/c/tests/pk_beats_test.c` against the numbers of `UI.html`'s demo script, and every one of those tests was seen red (`pickemup/c/tests/MUTATIONS.md`).

### Take 1: an arrival, three players: draws, a reshuffle, a play

12 beats, 2618 ms end to end.

| # | event | beat | start ms | ms | easing | from | to | card | parts |
|---|---|---|---|---|---|---|---|---|---|
| 0 | DRAW | flight | 16 | 320 | flight | deck | fan.2 | back | - |
| 1 | DRAW | flight | 126 | 320 | flight | deck | fan.2 | back | - |
| 2 | RESHUFFLE_GATHER | gather | 236 | 440 | in | stack | deck | back | 3 x 360, 40 apart |
| 3 | RESHUFFLE_SHUFFLE | fatten | 676 | 240 | card-spring | deck | deck | - | - |
| 4 | RESHUFFLE_SHUFFLE | riffle | 916 | 196 | ease-out | deck | deck | - | 8 x 140, 8 apart |
| 5 | RESHUFFLE_SHUFFLE | riffle | 1112 | 196 | ease-out | deck | deck | - | 8 x 140, 8 apart |
| 6 | DRAW | flight | 1308 | 320 | flight | deck | fan.2 | back | - |
| 7 | DRAW | flight | 1418 | 320 | flight | deck | fan.2 | back | - |
| 8 | DRAW | flight | 1528 | 320 | flight | deck | fan.2 | back | - |
| 9 | PLAY | flight | 1873 | 500 | flight | fan.2 | stack | 50 | - |
| 10 | PLAY | halo | 2373 | 220 | ease-out | stack | stack | - | - |
| 11 | TURN_TO | turn bar | 2398 | 220 | ease-out | seat.2 | seat.1 | - | - |

### Take 2: the deal, three players, the dealer's phone, opened

36 beats, 4426 ms end to end.

| # | event | beat | start ms | ms | easing | from | to | card | parts |
|---|---|---|---|---|---|---|---|---|---|
| 0 | LOBBY_START | hold | 100 | 500 | ease-out | board | board | - | - |
| 1 | LOBBY_START | fade | 600 | 220 | ease-out | board | board | - | - |
| 2 | SHUFFLE | riffle | 845 | 196 | ease-out | deck | deck | - | 8 x 140, 8 apart |
| 3 | SHUFFLE | riffle | 1041 | 196 | ease-out | deck | deck | - | 8 x 140, 8 apart |
| 4 | DEAL | flight | 1237 | 320 | flight | deck | fan.1 | back | - |
| 5 | DEAL | flight | 1322 | 320 | flight | deck | fan.2 | back | - |
| 6 | DEAL | flight | 1408 | 320 | flight | deck | hand.0 | back (38) | - |
| 7 | DEAL | flip | 1728 | 160 | linear | hand.0 | hand.0 | 38 | - |
| 8 | DEAL | flight | 1494 | 320 | flight | deck | fan.1 | back | - |
| 9 | DEAL | flight | 1579 | 320 | flight | deck | fan.2 | back | - |
| 10 | DEAL | flight | 1665 | 320 | flight | deck | hand.1 | back (66) | - |
| 11 | DEAL | flip | 1985 | 160 | linear | hand.1 | hand.1 | 66 | - |
| 12 | DEAL | flight | 1751 | 320 | flight | deck | fan.1 | back | - |
| 13 | DEAL | flight | 1837 | 320 | flight | deck | fan.2 | back | - |
| 14 | DEAL | flight | 1922 | 320 | flight | deck | hand.2 | back (80) | - |
| 15 | DEAL | flip | 2242 | 160 | linear | hand.2 | hand.2 | 80 | - |
| 16 | DEAL | flight | 2008 | 320 | flight | deck | fan.1 | back | - |
| 17 | DEAL | flight | 2094 | 320 | flight | deck | fan.2 | back | - |
| 18 | DEAL | flight | 2179 | 320 | flight | deck | hand.3 | back (1) | - |
| 19 | DEAL | flip | 2499 | 160 | linear | hand.3 | hand.3 | 1 | - |
| 20 | DEAL | flight | 2265 | 320 | flight | deck | fan.1 | back | - |
| 21 | DEAL | flight | 2351 | 320 | flight | deck | fan.2 | back | - |
| 22 | DEAL | flight | 2437 | 320 | flight | deck | hand.4 | back (7) | - |
| 23 | DEAL | flip | 2757 | 160 | linear | hand.4 | hand.4 | 7 | - |
| 24 | DEAL | flight | 2522 | 320 | flight | deck | fan.1 | back | - |
| 25 | DEAL | flight | 2608 | 320 | flight | deck | fan.2 | back | - |
| 26 | DEAL | flight | 2694 | 320 | flight | deck | hand.5 | back (49) | - |
| 27 | DEAL | flip | 3014 | 160 | linear | hand.5 | hand.5 | 49 | - |
| 28 | DEAL | flight | 2779 | 320 | flight | deck | fan.1 | back | - |
| 29 | DEAL | flight | 2865 | 320 | flight | deck | fan.2 | back | - |
| 30 | DEAL | flight | 2951 | 320 | flight | deck | hand.6 | back (42) | - |
| 31 | DEAL | flip | 3271 | 160 | linear | hand.6 | hand.6 | 42 | - |
| 32 | FLIP | flight | 3681 | 500 | flight | deck | stack | 82 | - |
| 33 | START_CARD | halo | 4181 | 220 | ease-out | stack | stack | - | - |
| 34 | START_CARD | fade | 4181 | 220 | ease-out | dir | dir | - | - |
| 35 | TURN_TO | turn bar | 4206 | 220 | ease-out | seat | seat.1 | - | - |

