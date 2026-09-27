# Every test, seen red

Each row is one mutation of the kernel or the bridge, applied alone by a script that kept its own copy of the file, deleted the one test binary so make could not reuse a same-second build, rebuilt and ran it, and restored the file byte for byte (checked) before the next row.
The cell is the first failure of the test the row is about; most rows turned other tests red too.
A test that never went red does not count, so every test function in `tests/*.c` and both smokes has at least one row.
Run 2026-09-27 on the kernel as of this commit, with `make run`'s arguments or fewer games (`tb_msg_test 2`, `tb_fuzz 140`, `tb_beats_test 20`); the Swift rows ran `make swift-smoke`.

THE T11 ROWS are the four under `tb_test.c` named T11, the two Swift rows, and in `tb_twophone_test.c` the staged-link guard and the send echo: DECISIONS.md T11's two named mutations are the first T11.1 row (the draft's step given a body) and the first T11.2 row (the roll seeded without the history).

## tb_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| T4 categories | Full House also takes five alike | `tb_test.c:46` "row 15: 44444 in 8 scores 25, want 0" |
| T4 bonus | the bonus at more than 63 | `tb_test.c:64` "63 earns the bonus: 0" |
| T4 bonus | `tb_bonus_known` drops the earned-bonus clause (integration, T61) | `tb_test.c:67` "an earned bonus is known with rows still open" |
| T4 bonus | `tb_bonus_known` never sees a full numbers half (`numbers + 1u`) (integration, T61) | `tb_test.c:73` "62 with every numbers row filled never will", `tb_test.c:77` "the view carries it: 0 0" |
| T4 zero | a SCORE is legal only where the dice score | `tb_test.c:84` "any open category may be taken" |
| T3 legality | a KEEP is legal at roll 3 | `tb_test.c:114` "no fourth roll" |
| T5 game end | the next turn does not skip a full card | `tb_test.c:144` "winner bit 0" |
| T5 ties | the winners keep only the first best seat | `tb_test.c:160` "seats 0 and 1 share it: 1" |
| T5 left skipped | the next turn does not skip a seat that left | `tb_test.c:175` "seat 2 rolls next, seat 1 skipped: 1" |
| T6 formula | the seat left out of the roll's hash | `tb_test.c:204` "roll 1 of turn 0 from the seed and the empty history" |
| T6 same history | a per-call counter mixed into the roll's hash | `tb_test.c:234` "same history, same dice (0)" |
| T11.1 draft unknown | the draft's step is given a body (the seed): the draft derives | `tb_test.c:270` "game 0 keep 3: die 2 is 1, a draft has values only where kept" |
| T11.1 draft unknown | the staged plan steps with a body | `tb_test.c:279` "the draft's ROLL event has no value" |
| T11.2 adopt | the roll seeded without the history (seed, seat, turn and roll only) | `tb_test.c:320` "A and B produce the same reroll at positions 3 and 4 in 64 of 64 games" |
| T11.2 adopt | a kept die rerolled too | `tb_test.c:304` "the kept dice held" |
| T11.3 no value field | the KEEP menu is left out when die 0 shows 6 | `tb_test.c:339` "the menu reads no die" |

## tb_msg_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| wire lobby | a leave does not move later rows down | `tb_msg_test.c:56` "Bo leaves; Cy moves down" |
| wire draft | the encoder leaves the pending move out | `tb_msg_test.c:79` "a draft writes, and reads back as resident" |
| wire race | the sender is the first move's seat | `tb_msg_test.c:107` "then Bo's keep: Bo sent it" |
| wire race | rule 4 inverted: fewer bubbles win | `tb_msg_test.c:99` "the second keep beats the first" |
| wire fit | the body bound at 4 bits a bubble | `tb_msg_test.c:159` "8 players, 312 bubbles, 48-byte names: 1075 chars" |
| wire sweeps | the check is never compared | `tb_msg_test.c:195` "a corruption that reads is its own game" |
| wire draft | the byte decode reads without deriving | `tb_msg_test.c:82` "the receiver derives die 0" |
| wire games | the link decode reads without deriving | `tb_msg_test.c:226` "2 seats game 0 bubble 1 reads back" |

## tb_fuzz.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| fuzz invariants | the turn skips a seat at 3 and more | `tb_fuzz.c:26` "seats in play keep within one turn of each other (0..2)" |
| fuzz spec roll | the forward body multiplies before it adds (S += P b i) | `tb_fuzz.c:102` "the roll at bubble 1 is SHA-256 over the body the wire carries through it" |
| fuzz wire | the fold writes digit 3 one lower | `tb_fuzz.c:47` "decode(encode) is the game at bubble 6" |
| fuzz draft | a draft records the wrong pending move | `tb_fuzz.c:65` "the draft's body says the move" |
| fuzz keep | die 0 rerolls even when kept | `tb_fuzz.c:96` "a kept die holds" |

## tb_say_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| say table | CAT_9 says "Small Straight" | `tb_say_test.c:29` "CAT_9 says "straight": Small Straight" |
| say table | CAT_11 names the branded game | `tb_say_test.c:29` "CAT_11 says "yahtzee": Yahtzee" |
| say table | CAT_11 spells the name out instead of {game} | `tb_say_test.c:43` "CAT_11 spells the game's name out: Tallybones" |
| say captions | the kept dice are not sorted | `tb_say_test.c:77` "said "Alex keeps 3, 5, 3 and rerolls two", want "Alex keeps 3, 3, 5 and rerolls two"" |
| say end | a tie is captioned as a win | `tb_say_test.c:77` "said "Bo wins with 200", want "Alex and Bo tie at 200"" |
| say screen | the rerolls left counted one short | `tb_say_test.c:170` "Tap dice to keep. One reroll left" |
| say games | the caption's width rule 30 columns wider | `tb_say_test.c:216` "one line: Alex took a zero on Full House. Bo to roll" |
| say games | a keep caption also lists die 5 | `tb_say_test.c:211` "a staged keep names the kept dice and no other: Alex keeps 2, 2, 6, 6 and rerolls one" |

## tb_beats_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| beats start | the leads swapped | `tb_beats_test.c:38` "an opened bubble leads by 100: 16" |
| beats keep | every settle rolls all five | `tb_beats_test.c:64` "the rerolled three only" |
| beats send | a sent keep's plan starts from the values it replaced (the SEND blanking skipped) (integration, T64) | `tb_beats_test.c:99` "sent: the rerolled dice start blank: 4 4 2" |
| beats score | a stamp lands when it starts | `tb_beats_test.c:102` "in the air: not on the card yet" |
| beats end | no hold after the result | `tb_beats_test.c:122` "a leave that ends it: fade, results, hold (2)" |
| beats games | the turn bar ignores a turn away from seat 1 | `tb_beats_test.c:156` "the end frame is the settled board" |

## tb_twophone_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| lobby | a cold open plays nothing (the range starts at the tip) | `tb_twophone_test.c:156` "opening the start settles five dice" |
| first turn | the staged-link guard dropped: my own staged link reads, and derives | `tb_twophone_test.c:169` "my own staged link does not read" |
| first turn | the view ignores the staged move | `tb_twophone_test.c:101` "keep 3 staged, known 31" |
| first turn | the send echo peeks instead of the resident replay | `tb_twophone_test.c:183` "sent: the reroll is there" |
| first turn | `tb_api_view` shows a staged SCORE as the draft's next turn (the T66 overlay skipped) (integration) | `tb_twophone_test.c:219` "staged: the scored dice stay on the tray and the turn stays with Alex (T66)" |
| the rest | a read past bubble 6 adopts the peek (no dice) | `tb_twophone_test.c:249` "both phones see one game (bubble 9)" |
| lobby rules (2026-09-27) | `shared/c/msg_lobby_roster.c` offered: a lone seat offered START (the `n_seats >= 2` guard dropped) | `tb_twophone_test.c:287` "alone again, the newest bubble not his: Alex is offered Invite (1), not Start" (green before this step) |
| lobby rules (2026-09-27) | `shared/c/msg_lobby_roster.c` offered: the full table's START exemption dropped (the join that fills the table cannot start) | `tb_twophone_test.c:131` "Bo joins and starts" (red before this step too), `:279` "the table is full: Bo, the newest sender, is offered Start (3) or a leave", `:293` "Bo joins and starts in one bubble" (read with the step run first: in place, the 40-line report cap is spent on the red game before it) |
| lobby rules (2026-09-27) | `shared/c/msg_lobby_roster.c` can_exit: true after the start (`!l->started` dropped) | STAYS GREEN, and cannot go red through this product: the bridge asks `can_exit` only while WAITING and turns a live leave into the T5 game move, so nothing here reaches the roster's verdict once live. The step still asserts the live roster never shrinks. Only `msg_lobby_roster_test` sees this one. |

## ios/tb_api_smoke.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| the layout | a kept die is not lowered | `tb_api_smoke.c:121` "die 4, kept" |
| ranks | the ranks by lowest total | `tb_api_smoke.c:96` "Alex leads" |
| the staged link | the staged-link guard dropped | `tb_api_smoke.c:90` "my staged bubble is not mine to read" |

## ios/tb_api_smoke.swift

| Test | Mutation | Assertion that went red |
|---|---|---|
| swift bridge | the send echo peeks instead of the resident replay | "the reroll is here"; "the frame lands on the view" |
| swift bridge | the staged view derives (tb_draft given a body) | "staged: the rerolls are blank" |
