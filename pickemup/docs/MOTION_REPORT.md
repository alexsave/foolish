# Pick 'Em Up - motion report

What the motion layer plays, measured where it can be.

## The filmed measurement is owed

No take was filmed.
The iPhone 17e simulator `6E0A730D` booted within the watchdog and ran `BeatPlayerTests` and `TableModelTests` green, but every test that puts a window or a renderer on screen hung there (`ActionCardCornerTests`, which hosts a card in a `UIWindow`, and `RenderTests`, which renders the bubble), and Messages cannot be launched at all on this host (ORCHESTRATION B2).
A filmed take needs exactly that: the board in a window, on screen, recorded.
So the two takes the brief asks for (a live arrival with three draws, a reshuffle and a play; a deal) are given below as the kernel's own timeline, which is what the phone plays: the Swift layer samples these beats every frame and adds no time of its own.
Owed after a reboot of the host: film both takes at normal speed with the rig (`foolish/ios/Tools/rig` with `pickemup/ios/Tools/rig.env`) or `xcrun simctl io <udid> recordVideo`, run the `animation-measure` skill over them, put the contact sheets in `pickemup/docs/shots/motion/` and the scores here (ORCHESTRATION B3).

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
| 2: a three-player deal, opened | 4426 ms, of which the shuffle and deal are 2586 ms (845 to 3431) | U20: "between 3.2s and 4.2s end to end" (see the FOUND in ANIMATION_DECISIONS.md) |
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

