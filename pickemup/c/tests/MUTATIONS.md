# Every test, seen red

Each row is one mutation of the kernel, applied alone, with `make -i run` (every test binary, so an early red binary does not hide a later one), and the assertion that went red for it.
The file was restored byte for byte after each run and the suite was green again before the next.
A test that never went red does not count, so every test function in `pk_test.c`, `pk_plan_test.c`, `pk_say_test.c` and `pk_fuzz.c` has at least one row.
Section numbers are `pickemup/docs/RULES_AND_KERNEL.md`'s.
The fuzz ran 400 games per mutation, 2,800 for the two cap rows.
Run 2026-09-26 on the kernel as of this commit.

## pk_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| 7.1.1 matching | `matches` drops the rank clause (`return 0` for rank) | `pk_test.c:47` row 1 "number rank match (5 on 5)", row 3 "skip on skip" |
| 7.1.1 matching | compare `pk_suit(top)` instead of `live_suit` | `pk_test.c:47` row 10 "number on a wild's chosen suit", row 12 |
| 7.1.2 pass | PASS legal whenever `t_drew \|\| !can_draw` | `pk_test.c:73` "pass refused: nothing drawable but a playable card" |
| 7.1.3 draw | DRAW legal with `stack_n >= 1` | `pk_test.c:94` "draw refused with an empty deck and a stack of 1" |
| 7.1.4 call-out leaks nothing | CALL_OUT also needs `hand_n[t] == 1` | `pk_test.c:128` "calls 00 vs 02 after reshaping other hands" |
| 7.1.5 say-it | `b_exposed_at_open` gains the seat at the play that exposes it | `pk_test.c:142` "say refused in the exposing bubble (D3)" |
| 7.1.6 menu order | a wild's four suits emitted in reverse | `pk_test.c:180` "entry 2: 2/1/3" (want suit 0) |
| 7.1.7 last-card wild | a wild always has four suits | `pk_test.c:201` "one menu entry for a last-card wild, got 4" |
| 7.2.1 action cards | a 2-player Reverse also flips `dir` | `pk_test.c:248` row 1 "dir -1, want 1" |
| 7.2.1 action cards | Skip moves `next(1)` | `pk_test.c:247` row 0 "turn 1, want 0", row 6 |
| 7.2.2 penalty short supply | a draw from an empty deck never reshuffles | `pk_test.c:301` "dealt 0, want 2", `:304` "PENALTY_SHORT n=4, want 2" |
| 7.2.3 going out | the action's effect applied before the empty-hand check | `pk_test.c:321` "the victim drew nothing: 4" |
| 7.2.4 hand order | swap-remove (last card into the gap) | `pk_test.c:340` "gap closed, order kept: 24 89 50 76" |
| 7.3.1 round-robin deal | seven cards to each seat in turn | `pk_test.c:358` "card 2 went to seat 2" |
| 7.3.2 shuffle golden | `deal_rng_bounded(&rng, i)` instead of `i + 1` | `pk_test.c:382` "shuffle vector" |
| 7.3.3 start card | a buried card goes on top of the deck | `pk_test.c:410` "pk_new refused seed 1" (the bound turned the loop into a refusal), `:423` "a seed with 1 buries" |
| 7.3.4 reshuffle determinism | reshuffle r keyed from block (r + 1) << 32 | `pk_test.c:439` "r=1: drew the new top", `:440` "the deck is shuffle(block r<<32)" |
| 7.3.5 no overlap | reshuffle r keyed from block r | `pk_test.c:468` "initial shuffle reads [0, 7), reshuffle 1 starts at 1" |
| 7.3.6 top stays | the top card goes into the reshuffle too | `pk_test.c:485` "the top is not in the new deck (deck 10)", `:486` conserved |
| 7.7.1 call-out windows | the window closes at the end of every bubble | `pk_test.c:518` "the window is still open (D4)" |
| 7.7.2 judged at open | the catch judged against `exposed` at seal | `pk_test.c:571` "still a hit: 1 + 2 + 2 = 3, catcher 3" |
| 7.7.3 penalty at end | the catch's penalty dealt at CALL_OUT | `pk_test.c:584` "the catcher's own draws are the same cards with or without the catch" |
| 7.8.1 lobby verdicts | the full-lobby exemption dropped | `pk_test.c:612` "a full table: everyone may start" |
| 7.8.2 two routes, one deal | deal from `lobby_rev` instead of `n_seats` | `pk_test.c:643` "join-and-start", `:646` "identical hands and deck" |
| 7.8.2 two routes, one deal | `pk_plan_lobby` reports a leave as a join | `pk_test.c:658` "a leave then a join, at their seats" |
| 7.8.3 leave compacts seats | leave blanks the row instead of moving later rows down | `pk_test.c:669` "later rows moved down" |
| 7.8.4 2 players | continue allowed on one card | `pk_test.c:698` "a play to one card ends the bubble even when the turn comes back (D7)" |
| 7.8.5 stuck table | a pass that drew counts as bare | `pk_test.c:732` "a pass that drew is not bare (idle 1)" |
| 7.8.6 undo floor | the floor moves at the last play, not the last draw | `pk_test.c:752` "undo returns the play" |

## pk_plan_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| 7.5.1 other counts never leak | `reveal_n` / `reveal_hand` filled while playing | `pk_plan_test.c:81` "the view changed with another hand" |
| 7.5.2 spectator | the spectator (-1) treated as seat 0 | `pk_plan_test.c:97` "a spectator sees no hand and can do nothing" |
| 7.5.3 game end reveals all | keep masking when over | `pk_plan_test.c:113` "viewer -1 sees every hand at the end" |
| 7.5.4 events mask draws | mask a card by `other` instead of `seat` | `pk_plan_test.c:131` "kind 5 to seat 0 hidden 1" |
| 7.6.1 golden plans | the reshuffle triple not reported (the design's "after the draw" cannot be written: the draw needs the reshuffled deck) | `pk_plan_test.c:179` "draw 9 with a reshuffle, then play" |
| 7.6.2 ordering invariants | CALL_HIT emitted at the CALL_OUT step | `pk_plan_test.c:273` "the catch after the turn" |
| 7.6.3 the cut | CALL_MISS marked ACTION | `pk_plan_test.c:323` "a catch outcome is ACTION-half" |
| 7.6.4 one card per event | every second penalty draw not reported (two cards, one event) | `pk_plan_test.c:379` "game 0: hands 13 by events, 33 by state" |
| 7.6.5 since | penalty draws counted in `drawn` | `pk_plan_test.c:415` "per-seat counts" |

## pk_say_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| 6 the table | `CAP_LEFT` ends in a full stop | `pk_say_test.c:69` "CAP_LEFT: ends in a full stop" |
| 6 the table | an em dash (as a C escape) in `SUB_PLAYABLE_NONE` | `pk_say_test.c:67` "SUB_PLAYABLE_NONE: a dash" |
| 6 the table | the protected mark (as a C escape) in `BTN_RULES` | `pk_say_test.c:70` "BTN_RULES: the protected mark" |
| 6 the table | `CAP_INVITE` spells the game's name instead of `{game}` | `pk_say_test.c:72` "CAP_INVITE: says the game's name itself" |
| 6 the table | `CAP_JOINED` says `{whom}` | `pk_say_test.c:78` "unknown placeholder at \"{whom} joined\"" |
| 6.2 captions | the joint after "!" is ". " | `pk_say_test.c:184` "say, then the turn: \"Al: Last card!. Al drew 1 and passed\"" |
| 6.2 captions | no fit check before a second clause | `pk_say_test.c:95` "a hit comes first: \"Bo caught Ana. Ana draws two. Bo played ...\"" |
| 6.2 captions | a hit loses its clause | `pk_say_test.c:95` "a hit comes first: \"Bo played 7 of circles. Cy to play\"" |
| 6.2 captions over random games | a hit loses its clause (same run) | `pk_say_test.c:278` "game 57 bubble 141: \"\"" (a catch-only bubble had nothing to say) |
| 6.3 screen lines | the order line looks two seats ahead | `pk_say_test.c:236` "who is before you: \"\"" |
| 6.3 screen lines | a direction word at 2 players | `pk_say_test.c:252` "no direction word at 2 players (D13)" |

## pk_fuzz.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| fuzz conservation | a played card is not pushed on the stack | `pk_fuzz.c:58` "game 0 step 0: 104 cards accounted for" |
| 7.2.5 one representation | no `hand_changed` after a penalty | `pk_fuzz.c:61` "game 1 step 95: exposed 02 said 00 on a seat not on one card" |
| fuzz a turn ends | a draft may be sealed mid-turn | `pk_fuzz.c:64` "game 0 step 2: sealable mid-turn" |
| fuzz a turn ends (the caps) | the 750-bubble stop removed (2,800 games) | `pk_fuzz.c:70` "game 1307: within the caps" |
| fuzz undo by replay | the replay drops "Last card!" | `pk_fuzz.c:76` "the replay reproduces the game" |
| fuzz undo by replay | undo leaves a CONTINUE behind | `pk_fuzz.c:87` "undo is the state before the play" |
| fuzz undo by replay | undo ignores the floor | `pk_fuzz.c:82` "a draw was undone" |
| fuzz terminates | the long-game stop does not end the game (2,800 games) | `pk_fuzz.c:92` "game 1307 (n=7) ended within 1845 steps" |

## Not in this kernel

7.3.7 (native against wasm replay) waits for a wasm replay build; `make wasm` proves the kernel compiles freestanding for wasm32 and reaches only `memcpy`, `memset`, `strlen` and `strncmp`.
7.4 (the wire), 7.7.4 (Rule P races) and 7.8.7 (seat resolve) are the envelope's, and land with `pk_code` / `pk_msg`.
