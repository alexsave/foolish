# Every test, seen red

Each row is one mutation of the kernel, applied alone, with `make -i run` (every test binary, so an early red binary does not hide a later one), and the assertion that went red for it.
The file was restored byte for byte after each run and the suite was green again before the next.
A test that never went red does not count, so every test function in `pk_test.c`, `pk_rules_test.c`, `pk_plan_test.c`, `pk_say_test.c` and `pk_fuzz.c` has at least one row.
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

## pk_rules_test.c

Run 2026-09-27, each mutation applied alone by a script that kept its own copy of the file, deleted the binary so make could not reuse a same-second build, ran the one binary, and restored the file byte for byte.

| Test | Mutation | Assertion that went red |
|---|---|---|
| 1.8 say it in a later bubble, with the turn | SAY_IT reads only the live `exposed`, not `b_exposed_at_open` | `pk_rules_test.c:61` "not in the exposing bubble, even with the turn back (D3)" |
| D32 a stamp wiped inside the bubble still protects | CALL_OUT reads the live `said` | `pk_rules_test.c:82` "a catch on the seat stamped at open is still refused (D5c, D32)" |
| 1.8 the window and the bubbles that do not close it | a catch-only bubble closes the window too | `pk_rules_test.c:97` "the window is open, the catcher paid twice" |
| D5b a catch penalty reshuffles the stack in | a penalty draw never reshuffles | `pk_rules_test.c:133` "a hit, two cards, one of them after a reshuffle (1, r 0)" |
| 1.9 a reshuffle inside a penalty | the same | `pk_rules_test.c:159` "two cards, the deck's last one first", `:171` "draw, the triple, draw (2 events)" |
| 1.9 a reshuffle inside a penalty | the reshuffle keeps the stack's bottom card as the top | `pk_rules_test.c:161` "reshuffled once: the +2 stays on top", `:172` conserved |
| 1.9 the pile is only its top card | a short penalty goes on trying instead of forgiving the rest | `pk_rules_test.c:193` "three forgiven (1), one reshuffle" |
| D14 a Wild +4 turned at the start is buried | a Wild +4 may be the start card | `pk_rules_test.c:230` "seed 10: seat 1 starts on a number", `:234` "the +4 is at the bottom of the deck (-1)" |
| D7 a two-player chain | a 2-player Reverse moves the turn on | `pk_rules_test.c:251` "Reverse: a skip, dir unchanged (D13)" |
| D7 a two-player chain | CONTINUE never written | `pk_rules_test.c:259` "one CONTINUE per turn that came back (5 records)" |
| D7 a two-player chain | the direction word shown at 2 players | `pk_rules_test.c:263` "no direction word at two players" |
| 1.7 going out on a +4 or a wild | a last-card wild takes a suit | `pk_rules_test.c:277` "n 2: a last-card +4 takes no suit (D17)" |
| 1.7 going out on a +4 or a wild | WILD_SUIT announced for a suitless wild | `pk_rules_test.c:292` "no suit is chosen, so none is announced" |
| 1.11 the long-game stop | the stop's winner counted from seat 0, not the seat to move | `pk_rules_test.c:311` "fewest wins; seats 1 and 3 tie, and 3 is next from 2 (1)" |
| 1.11 the long-game stop | no stop after a draw | `pk_rules_test.c:311` "(255)", `:312` "the bubble seals mid-turn in this one case" |
| 1.11 the 750-message stop | the stop at `bubbles > 750` | `pk_rules_test.c:328` "the game is over at 750 bubbles" |
| 1.10 the stuck table | a tie goes to the last tied seat (`<=`) | `pk_rules_test.c:363` "row 0: winner 1, want 0" and rows 2 to 5 |
| D30 the history cap holds at its worst | `PK_HIST_CAP` lowered to 2,750 | `pk_rules_test.c:406` "game 0: hist 2815 actions 1500 bubbles 62 within the caps" |
| D8 an own-draw reshuffle is never undone | a draw does not move the floor | `pk_rules_test.c:422` "undo refuses: the draw is the floor" |
| D50 undoing a play whose penalty reshuffled is exact | a penalty draw moves the floor | `pk_rules_test.c:447` "game 7: the play is undoable" |
| D51 a device that left is never seated by a namesake | `pk_rec_find` returns -1 for a record with no row (before the fix) | `pk_rules_test.c:485` "the record now names a row that is gone" |
| D51 a device that left is never seated by a namesake | the resolver ignores `PK_REC_GONE` | `pk_rules_test.c:493` "the leaver's device is not handed the namesake's seat", `:495` "not by the sender witness either" |

## pk_plan_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| 7.5.1 other counts never leak | `reveal` filled while playing | `pk_plan_test.c:81` "the view changed with another hand" |
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
| fuzz a turn ends (the caps) | the 750-bubble stop removed (2,800 games) | `pk_fuzz.c:70` "game 587: within the caps" (re-run on `seed_wide`, 2026-09-26) |
| fuzz undo by replay | the replay drops "Last card!" | `pk_fuzz.c:76` "the replay reproduces the game" |
| fuzz undo by replay | undo leaves a CONTINUE behind | `pk_fuzz.c:87` "undo is the state before the play" |
| fuzz undo by replay | undo ignores the floor | `pk_fuzz.c:82` "a draw was undone" |
| fuzz terminates | the long-game stop does not end the game (2,800 games) | `pk_fuzz.c:92` "game 1307 (n=7) ended within 1845 steps", on `seed_of`'s deals; on `seed_wide`'s no fuzz game reaches 1,500 actions, and this mutation is now caught by `pk_msg_test.c` 7.4.6 below |
| fuzz terminates | `finish` leaves the winner NONE for a stop | `pk_fuzz.c:93` "game 587 has a winner" (`seed_wide`, 2026-09-26) |

## pk_msg_test.c

Run 2026-09-26 with `./build/pk_msg_test 5 100` (the ASan rows with `./build/asan_pk_msg_test 1 5` and `ASAN_OPTIONS=symbolize=0`, because the symbolizer hangs in this sandbox), each mutation applied alone by a script that restored the file from its own copy afterwards, never by `git checkout`.

| Test | Mutation | Assertion that went red |
|---|---|---|
| 7.4.1 round trip | the continue digit K dropped (a turn that came back always ends the bubble) | `pk_msg_test.c:103` "game 0 bubble 20 encodes (-6)" |
| 7.4.2 canonicality: every bit flipped | decode stops requiring the number to end on the sentinel | `pk_msg_test.c:402` "envelope 1 bit 304 (re-checked) reads as a different writing" |
| the corruption sweep | the same | `pk_msg_test.c:373` "envelope 7 byte 38 ^ 01" |
| 7.4.4 check: every truncation | the tag bounds check before a roster row removed | ASan global-buffer-overflow in the truncation sweep (the first sweep; nothing after `rule p` had printed) |
| 7.4.4 check: every truncation | the name bounds check removed | ASan global-buffer-overflow in the truncation sweep, likewise |
| 7.4.4 check: raw flips are refused | the check comparison skipped | `pk_msg_test.c:412` "21554 of 38784 raw flips read as a game" |
| 7.4.3 header agreement | decode stops comparing `turns` with the replay | `pk_msg_test.c:444` "a header that says turns + 1 is refused" |
| 7.4.5 size gate | every digit coded in base 255 on both sides (a coder that ignores the menus) | `pk_msg_test.c:131` "p95 at 8 players is 1143 characters, the guardrail is 1,000" |
| 7.4.5 size gate: the owner's p99 case | seat tags 24 bytes instead of 9 | `pk_msg_test.c:179` "the owner's p99 case is 541 characters at p99; 4.5 estimated 530" |
| 7.4.5 size gate: the capped worst case | `PK_MAX_ACTIONS` raised to 4,000 | the build: `pk_msg.h:79` static assertion "the capped worst case fits MSMessage.url" |
| 7.4.6 caps are rules | the long-game stop never fires inside a turn | `pk_msg_test.c:239` "the game stops at the action cap: over 0 after 1500 actions, 570 bubbles" |
| 7.7.4 Rule P races | clause 5 (TIP_SAID) dropped | `pk_msg_test.c:507` "race 2: say-it beats a catch of the same parent (1, -1)" |
| 7.7.4 Rule P races | `bubbles` compared before `turns` | `pk_msg_test.c:518` "race 1: a turn beats catches that answered the same parent" |
| 7.8.7 seat resolve | the tag trusted over the record | `pk_msg_test.c:593` "the record outranks the tag, the sender and the name" |
| 7.8.7 seat resolve | the lobby gate removed | `pk_msg_test.c:632` "a lobby row under another name is not mine, whoever sent the bubble" |
| lobby: rows, verdicts and the wire | a leave does not set LEFT | `pk_msg_test.c:705` "after a leave Bo, alone, is offered the invite" |
| the corruption sweep: several bytes at once | the check comparison skipped | `pk_msg_test.c:459` "envelope 0 trial 1 (5 bytes, raw) reads as a different writing", `:460` "accepted with a failing check" |
| the header's boundaries | the seat cap ignores DM (always 8) | `pk_msg_test.c:582` "DM with three seats", `:662` "a DM of three" |
| the header's boundaries | a started game's starter not range-checked | `pk_msg_test.c:594` "starter = n_seats", `:596` "no starter on a started game" |
| the header's boundaries | a body with a zero top byte accepted | `pk_msg_test.c:604` "a trailing zero byte: a second spelling (D48)" |
| the header's boundaries | a started game's lobby_rev unchecked (the decoder before D52) | `pk_msg_test.c:621` "a started roster with fewer joins than seats", `:623` "a started lobby_rev of the wrong parity" |
| the header's boundaries | the replay agreement accepts a phase that is neither LIVE nor FINISHED (the one judge of the phase, once its whitelist was removed as a second guard) | `pk_msg_test.c:573` "phase 1 on a live body is refused (0)" and phases 4 to 7 |

Run 2026-09-27 with `./build/pk_msg_test 1 10`, by the same restore-from-copy script as `pk_rules_test.c`'s rows.

The rules doc's 7.4.2 mutation ("allow the empty-bubble option in the C digit") does not apply: D40 makes every digit a menu of non-empty bubbles, so an empty bubble is not an option to allow, and a coder that offered one would still round-trip (the decoder's seal refuses it).
The sentinel mutation above is the one that breaks canonicality.

## The bridge (ios/)

| Test | Mutation | Assertion that went red |
|---|---|---|
| ios-smoke | `pk_api_text` writes the resident draft instead of a sealed copy | `pk_api_smoke.c:217` "the start bubble is a link" |
| ios-smoke | a record finds a row by its offset, not its tag | `pk_api_smoke.c:328` "Cleo's record finds her in the row she moved down to" |
| ios-smoke | `pk_api_leave` forgets the record (the bridge before D51) | `pk_api_smoke.c:340` "the first Bo, who left, is not seated by the name" |
| ios-smoke | the resolver ignores `PK_REC_GONE` | `pk_api_smoke.c:340` "the first Bo, who left, is not seated by the name" |
| swift-smoke | the host library stamped with a hash that is not the readers' | `pk_api_smoke.swift:36` "the library and the readers are one layout" |
| ios-smoke words | `W_STAGED_CAPTION` captions `bubbles - 1` of the sealed copy | `pk_api_smoke.c:226` "the staged caption is the sent bubble's" |
| ios-smoke words | `W_LOBBY_ROW` says "(You)" on every row but mine | `pk_api_smoke.c:179` "my own roster row says so", `:199` "somebody else's roster row" |
| ios-smoke words | `W_PUBLIC_ROW` marks my row "(You)" like `W_LOBBY_ROW` | `pk_api_smoke.c:183` "the bubble's roster says nobody is you" |
| ios-smoke words | `W_LOBBY_DEALER` names the last seat, not seat 0 | `pk_api_smoke.c:317` "seat 0 deals, whoever joined last" |
| ios-smoke words | `W_ERROR` maps `PK_ECHECK` (not `PK_EFORMAT`) to the newer-version line | `pk_api_smoke.c:421` "a newer format says so", `:423` "any other refusal is a damaged link" |
| ios-smoke layout | `TWO_ROW` 34 becomes 30 | `pk_api_smoke.c:95` "ten: two rows, the 166 box (table 04)", `:117` "card 5 of ten opens the lower row" |
| ios-smoke layout | `STRIP_MIN` 16 becomes 12 | `pk_api_smoke.c:105` "forty-two: the rows scroll (table 07)", `:113` "thirty in the drawer: scrolling (collapsed 03)" |
| ios-smoke layout | the top row takes the larger half | `pk_api_smoke.c:97` "thirteen: the smaller half on top", `:101` "twenty-seven: overlapped, 40pt faces (U8)" |
| ios-smoke layout | `pk_lay_max_rows` gives the drawer two rows | `pk_api_smoke.c:88` "the drawer keeps one row (U7)" |
| ios-smoke layout | the ring's x loses its minus sign (seats go round the other way) | `pk_api_smoke.c:125` "the next seat sits on my left" |
| ios-smoke layout | the fan step divides 96 instead of 96 less a card | `pk_api_smoke.c:130` "the fan steps 10, compressing to fit 96 (U6)" |
| ios-smoke layout | 12 cards draw 7 layers | `pk_api_smoke.c:132` "the deck's layers" |
| ios-smoke layout | the pile never lifts | `pk_api_smoke.c:136` "the pile lifts 24 in the drawer (U2)" |
| ios-smoke layout | the deck 8pt from the pile | `pk_api_smoke.c:140` "the deck 10pt left of the pile, on its line (U3)" |
| ios-smoke layout | Play offered for a selection when it is not my turn | `pk_api_smoke.c:152` "not my turn: a selection offers no Play" |
| ios-smoke cards | `pk_api_card_suit` reads the next id's suit | `pk_api_smoke.c:427` "a suited card's suit and rank (3.2)" |
| ios-smoke cards | `pk_api_card_rank` accepts id 104 | `pk_api_smoke.c:431` "an id off the deck is nothing" |
| ios-smoke buried | `pk_api_buried` reports every buried card without looking for it in the deck | `pk_api_smoke.c:372` "the buried cards are the deck's bottom ones" (at 1 and 0 left) |
| ios-smoke ranks | the losers sorted by most cards first | `pk_api_smoke.c:416` "then fewest cards first, ties in seat order" |
| ios-smoke adopt (I29) | a further-on bubble always lays out as opened | `pk_api_smoke.c:213` "further on in the same game: an arrival" |
| ios-smoke adopt (I29) | the lost-race branch never taken | `pk_api_smoke.c:231` "the staged card flies home first" |
| ios-smoke adopt (I29) | the same bubble again lays out (prior, to] instead of nothing | `pk_api_smoke.c:202` "the same bubble again moves nothing", `:220` "opened again: nothing new" |
| ios-smoke fan (I30) | a moved call uncalls and calls on the resident, not a copy | `pk_api_smoke.c:252` "a refused move keeps the call it would have replaced", `:253` "a second tap takes it back" |
| ios-smoke zones (I31) | the draw band's `BAND_DOWN` 24 becomes 0 | `pk_api_smoke.c:157` "U24: the hand band, 64 up and 24 down" |
| ios-smoke words (I33) | `W_INDEX` gives a Wild +4 the `RANK_PLUS2` index | `pk_api_smoke.c:562` "a Wild +4's index" |
| ios-smoke words (I33) | `W_STRIP_DRAWS` accepts a count of 0 | `pk_api_smoke.c:567` "no draws, no chip" |
| collapse (I37) | a pass does not collapse | `pk_api_smoke.c:489` "a play or a pass collapses; a draw, an undo, an un-say, an un-call do not" |
| collapse (I37) | a play does not collapse | twophone `[S4 three draws then a play]` "the play ends the turn: the drawer collapses once it rests", `pk_api_smoke.c:489` |
| collapse (I37) | a call in the draft does not stop a Last card! from being the whole bubble | twophone `[S9 Last card! in a later bubble]` "beside a call it is not the whole bubble; un-called, it is" |
| collapse (I37) | a draw, play or pass in the draft does not stop it either | twophone `[S4 three draws then a play]` "the bubble is not a lone Last card!" |
| collapse (I37) | no guard on an open draft of mine | `pk_api_smoke.c:500` "no draft open: nothing to collapse for" |
| collapse (I37) | every other touch collapses (`default: return 1`) | twophone `[S4 ...]` "a draw does not collapse the drawer", `[S8c Caught you! staged]` "a call alone does not collapse the drawer", `pk_api_smoke.c:477` "a call does not collapse the drawer" |
| stamp (I37) | OUT for every seat once it is over | twophone `[S12 the win]` "OUT under the winner and nothing else once it is over" |
| stamp (I37) | no Caught you! | twophone `[S8d Caught you! after Send]` "Caught you! under the caught, nothing under the catcher" |
| stamp (I37) | no Wrong call | `pk_api_smoke.c:499` "Wrong call under the caller, nothing caught on the other" |
| stamp (I37) | no LAST | twophone `[S9 Last card! in a later bubble]` "the kernel's stamp: LAST under the sayer" |
| stamp (I37) | the order: the newest verdict before OUT (the `over` test moved below it) | `pk_api_smoke.c:506` "once it is over OUT outranks the newest bubble's verdict" (the winning bubble also carries a wrong call) |
| lobby rows (A13) | `pk_api_join` remembers no lobby | `pk_api_smoke.c:564` "Join: Cleo's row fades up, 220ms after a 16ms beat", `:566` "Join: the row is unseen before its fade" |
| lobby rows (A13) | `pk_api_leave` remembers no lobby | `pk_api_smoke.c:586` "Leave: Bo's row fades out, 220ms after a 16ms beat", `:589` "...close up, 320ms on the card spring", `:591` "the row that went, as it read to Bo" |
| lobby rows (A13) | `pk_api_adopt` lays a lobby over its lobby out as nothing | `pk_api_smoke.c:601` "an arrival: the same two beats", `:603` "as it read to Cleo", `:605` "opened: the 100ms lead" |
| lobby rows (A13) | `pk_api_adopt` lays out a lobby over another game's lobby | `pk_api_smoke.c:611` "another game's lobby, cold: nothing moves", `:612` "a read is no lobby action of mine" |
| lobby rows (A13) | `pk_api_read` keeps the remembered lobby action | `pk_api_smoke.c:595` "a read forgets my lobby action" |
| lobby rows (A13) | `pk_api_new` keeps it | `pk_api_smoke.c:610` "a new lobby is no roster change" |
| lobby rows (A13) | `W_LOBBY_GONE` reads the current roster's names | `pk_api_smoke.c:591` "as it read to Bo", `:603` "as it read to Cleo" |
| lobby rows (A13) | `W_LOBBY_GONE` never says "(You)" | `pk_api_smoke.c:591` "the row that went, as it read to Bo" |
| lobby rows (A13) | a change that moved no row is a plan (`n < 0`) | `pk_api_smoke.c:606` "the same lobby again moves nothing" |
| lobby rows (A13) | an opened lobby takes the arrival's 16ms lead | `pk_api_smoke.c:605` "opened: the 100ms lead" |
| collapse slide (A14) | the push keeps the spring's tail (no linear fade to zero) | `pk_api_smoke.c:186` "nothing left at 600ms, and no step to it" |
| collapse slide (A14) | the drawer's response 300ms, not 338 | `pk_api_smoke.c:184` "the host's spring at half its response", `:188` "uttt's numbers" |
| collapse slide (A14) | a linear push | `pk_api_smoke.c:184` "the host's spring at half its response", `:186` "nothing left at 600ms" |
| collapse slide (A14) | nothing pushed at the flip (`t == 0` answers 0) | `pk_api_smoke.c:182` "the whole travel at the flip", `:183` "the push only ever falls" |
| send hint (A15) | `PK_T_SEND_HINT` 2000 | `[vocabulary]` "the Send reminder waits three seconds" |
| send hint (A15) | `SEND_HINT` empty | `[6 the table]` "SEND_HINT: empty", `[6.3 screen lines]` "the Send reminder's word, under the arrow" |

Putting the winner first by name (rather than by fewest cards) survived its mutation: the winner of an OUT game holds none and the winner of a STUCK or LONG game is the one with the fewest, so the two orders differ only on a tie the kernel breaks the same way. It stays for the reader, not for a test.

Each layout and words row was run alone with `build/ios_smoke` deleted first, because `cp -p` puts the restored file's old mtime back and make then keeps the mutated binary (the same-second trap).

## Native against wasm (tests/pk_cross.c, 7.3.7)

`make cross WASM_CC=/opt/homebrew/opt/llvm/bin/clang` plays 100 of the fuzz harness's deals with the random tests' bot (`tests/pk_bot.h`) natively and in a wasm32 build run by node, and compares one value a game: the kernel's hash of the final state folded with every event of the whole game's plan.
Each row was applied alone, both builds deleted and rebuilt, `make cross` run, and the file restored byte for byte; `make cross` was green again after the last.
Run 2026-09-27 with Homebrew clang 22 and node 26.

| Test | Mutation | What went red |
|---|---|---|
| cross | wasm32 alone swaps the first two cards of every reshuffle (`#ifdef __wasm__` in `pk_shuffle`) | `cmp`: "build/cross_native.txt build/cross_wasm.txt differ: char 3, line 1" |
| cross | wasm32 alone plans every TURN_TO with `i = 1` (`#ifdef __wasm__` in `emit`), the state untouched | `cmp`: "differ: char 3, line 1" (the plan's fold is what catches it) |
| cross | the node host reads the values one on (`at + 8`) | `cmp`: "differ: char 3, line 1" |

7.4 (the wire), 7.7.4 (Rule P races) and 7.8.7 (seat resolve) are in `pk_msg_test.c` above.

## The timeline (tests/pk_beats_test.c, and the bridge's motion checks)

Each row was applied alone as an exact string replacement by a script that deleted the one binary first (the same-second trap), rebuilt it, ran it (`pk_beats_test 100`, or `ios_smoke`), and restored the source byte for byte.
Run 2026-09-27; every mutant went red, and every test in the file is named at least once.

| Test | Mutation | Assertion that went red |
|---|---|---|
| play a number, draws replayed, skip, reverse, +2, pass, catch, the cut, deal row, reshuffle, real games | `step_start` drops the 25ms gap between steps | `:174` "riffle 1: 140 + 7 x 8", `:186` "28 cards at 1153 + i x 1800 / 28" (28 in all) |
| deal step, deal row | `pk_beats_deal_at` multiplies by the truncated step (`i * (1800 / cards)`) | `:133` "no drift down the deal: 1728", `:186` "28 cards at 1153 + i x 1800 / 28" |
| deal step | the 45ms floor is never applied | `:130` "6 seats: 42.9 clamps to 45", `:131` "8 seats: 45" |
| deal row | the start card turns 120 (not 250) after the deal | `:191` "FLIP 250 after my last card turned: 3488" |
| deal row | a buried card ends at -9 deg | `:197` "BURY: sleep(300), then under the deck at +9 deg, no bulge" |
| draws replayed, reshuffle | replayed draws start 320 apart instead of 110 | `:264` "16, 126, 236", `:269` "the play ... : 1001" |
| reshuffle | the gather has one ghost per gathered card, not three | `:305` "gather ... three ghosts 40 apart" |
| reshuffle | the fatten beat does not carry the new count | `:307` "the count snaps", `:319` "the new count as the deck fattens" |
| play a number, draws replayed | a play's bulge is 1.08 | `:347` "bulge 1.15", `:359` "the bulge peaks at 1.15: 1.079870" |
| play a +2 / +4 | a wild's flight also gets a halo on landing | `:444` "a wild lands with its halo clear" |
| play a skip | the skipped badge dims to .5 | `:384` "the badge dims to .45", `:390` "down to .45 and no further: 0.500000" |
| play a skip | a one-part beat is sampled over its part, not its envelope | `:394` "the slash stays across until the dim ends" |
| play a reverse, win and reveal | the direction word swaps at the TURN's start, not its midpoint | `:410` "the old word until the box is edge-on", `:554` "the first back is face up at its midpoint" |
| play a +2 / +4, the cut | a +2 / +4 victim is never skip-slashed | `:441` "then the victim's skip slash (946)", `:442` "then the turn bar past them" |
| play a +2 / +4, say it, catch, the cut | penalty draws all start together | `:438` "2 backs into the victim's fan, 110 apart" |
| pass | my hand dims to .45 on a pass | `:468` "my hand dims to .5, all at once" |
| pass, the cut, real games | a settle event of the last turn is never held (no cut) | `:469` "the turn bar is held until Send", `:577` "held: 0" |
| draw live, pass, the cut, real games | a draft event counts as played by its index (`i < 2`), not by matching the previous plan | `:234` "flight, flip, pulse", and the real games' A + B counts |
| real games | at Send the action half plays again | `:793` "kind 9 plays 3 times at A + B, 2 on open" |
| say it, catch | a catch's penalty starts with its stamp, not after it | `:506` "then the two cards, 110 apart" |
| win and reveal, real games | the reveal starts with OUT, not after it | `:542` "8 flips from 856, 60 apart", `:549` "the results fade in a game-over hold after the reveal" |
| win and reveal | my own hand's reveal is flipped too | `:542` "viewer 0: 5 flips from 856", `:544` "card by card" |
| win and reveal | the results fade in with no game-over hold | `:549` "the results fade in a game-over hold after the reveal" |
| host motions | an undone card flies home with a 1.15 bulge | `:641` "after the 16ms beat, back to its slot, no bulge" |
| host motions | the refused undo shakes once, not three times | `:653` "-5 at a fifth: -3.000000", `:655` "+5 at two fifths" |
| host motions | the picker's tiles pop together | `:662` "five tiles pop, 30ms apart" |
| draw live | a drawn card is seen the moment its slot opens | `:244` "the slot opens and the count ticks as it leaves", `:246` "still turning" |
| deal row, draw live, host motions, play a number | the pile's top changes as a card leaves, not as it lands | `:213` "seat 1's first card has landed", `:246`, and "the old top until it lands" |
| deal row, draw live, reshuffle, the cut, real games | a beat's deck count is never shown | `:213` "five have left the deck", `:218` "after: the dealt table" |
| say it, catch | a stamp is never held back until its beat | `:490` "the stamp waits for its beat" |
| real games | `pk_beats_pre` replays one bubble too far | `:782` "A + B end hand", `:806` "viewer 1: hand" |
| vocabulary | `pk_ease` returns the x polynomial (every curve linear) | `:114` "card-spring and the stamp overshoot", `:117` "the flight curve at half time: 0.500000" |
| budget, deal row, deal step | the deal's floor is 90ms | `:127` "3 seats: 85.7", and the 8-seat "shuffle and deal in ... ms" budget |
| draws replayed, +2 / +4, reshuffle, catch, the cut, vocabulary | `PK_T_DRAW_STEP` 450 | `:94` "flight", `:264` "16, 126, 236" |
| draw live, the cut, host motions, real games | a flip never shows its card | `:248` "seen once it has turned", `:627` "the drawn card is simply there", `:646` "and it is home" |
| budget (and every row) | `PK_T_GAP` 525 | `:834` "8 players: the p99 bubble plays in under 4s (4056)" |
| host motions | a beat that brings something in is not applied before its start (no fill backwards) | `:665` "a tile waits unseen" |
| host motions | a beat that takes something out is not applied after its end (no fill forwards) | `:678` "collapsed tiles stay gone" |
| ios-smoke motion | the bridge does not remember the draft after a build | `pk_api_smoke.c:224` "asked again with nothing new: nothing moves" |
| ios-smoke motion | `pk_api_beats` refuses `from == to` | `pk_api_smoke.c:215` "from == to: no motion, a new plan" |
| ios-smoke motion | `pk_api_beats_send` lays the bubble out as an open | `pk_api_smoke.c:248` "channel B: what staging held" |
| play a wild (A12) | the BAND beat is not applied before its start (no fill backwards) | `[play a wild: the band slides up]` "hidden under the card's edge before it starts" |
| play a wild (A12) | the BAND beat eases linearly | `[play a wild: the band slides up]` "E.out", "ease-out: past halfway at half time (0.500000)" |
| play a wild (A12) | the BAND sample fades (`opacity = p`) as it did before A12 | `[play a wild: the band slides up]` "it slides, it does not fade" |
| play a wild (A12) | a seat's wild gets a BAND beat too (`if (1)`) | `[play a wild: the band slides up]` "a seat's wild arrives with its band on", `[play a +2 / +4]` "arrival: the band is already on the card" |

## pk_twophone_test.c

The two-phone game of `SIM_VERIFICATION.md`, played through `pk_api.c` (seed k=1, 67 bubbles, 2,409 assertions).
Each row was applied alone, `build/pk_twophone_test` deleted and rebuilt, run, and the file restored byte for byte; the suite was green again after each.
The bracket is the step tag the test prints on a red (`[S8c ...]`), which stays put when lines move.
Where a mutation also stopped the seed search from finding a seed, the test played seed 0 with the assertions on, so the named assertion still went red.
Run 2026-09-27.

| Test | Mutation | Assertion that went red |
|---|---|---|
| twophone +2 | `PK_PEN_PLUS2` draws 3 | `[S7 a +2]` "the receiver's hand is the count kept here (6, want 5)", "the receiver's plan: event 2 ... n 3, want n 2", "pk_since counts the draws, penalty cards and plays" |
| twophone catch | the catch's penalty is 1 card, not 2 | `[S8c Caught you! staged]` "the receiver's hand is the count kept here (2, want 3)", `[S8d Caught you! after Send]` "the caught player drew two (1.8)" |
| twophone reshuffle | the reshuffle keeps the pile's bottom card instead of its top (`g->stack[0] = top` dropped) | `[S8 reshuffle]` "the reshuffle leaves the pile its top card and suit" |
| twophone catch window | the window closes at the end of every bubble (`g->exposed = 0` at seal) | `[S8c Caught you! staged]` "the staged caption: \"Alex called Bo wrong and draws one\", want \"Alex caught Bo. Bo draws two\"", "the receiver's plan: event 4 is kind 26 (CALL_MISS), want 25 (CALL_HIT)" |
| twophone Rule P | clause 4 compares the bubbles the wrong way round (fewer wins) | `[S11 Rule P]` "Rule P keeps the newer bubble, whichever way round", "the host adopts the newer: its tip said it" |
| twophone masking | `pk_view` fills `reveal` while the game is live | `[S4 ...]`, `[S5 ...]`, `[S7 a Skip]` and every bubble after: "no seat's card count reaches the receiver while the game is played (D22)" |
| twophone masking | the plan never masks a DEAL, DRAW or PENALTY_DRAW card | `[S4 three draws then a play]` "the receiver's plan: event 1 (kind 12, seat 1) shows a card only its receiver may see", `[S2 the deal on the receiver]` "fourteen DEALs ... Bo's hidden" |
| twophone deal order | the deal starts at seat 0 (`j % n`) | `[S2 the deal on the receiver]` "fourteen DEALs, one card at a time from seat 1 ... (first wrong 0)" |
| twophone caption key | a 2-player Reverse takes `CAP_REVERSED` (`seats >= 2`) | `[S7 a Reverse]` "the staged caption: \"Bo turned the table around\", want \"Bo reversed and goes again\"", "the receiver's caption of bubble 10" |
| twophone say-it timing | "Last card!" legal in the bubble that exposed the player (`ref = g->exposed`) | `[S8b down to one]` "not in the bubble that exposed me (D3)" |
| twophone undo | a draw is not the draft's floor (`b_floor` not moved) | `[S4 three draws then a play]` "a draw does not come back (D8)", `[S4b undo a staged play]` "the card is on the pile" |
| twophone seat resolver | `pk_api_seats_load` keeps no records | `[S4 ...]` and every bubble after: "the receiver resolves to its own seat by its record (me 0 by 2)" |
