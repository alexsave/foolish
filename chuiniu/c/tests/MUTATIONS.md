# Every test, seen red

Each row is one mutation of the kernel or the bridge, applied alone by hand with an editor, and the assertion that went red for it.
Before every run the test binary was deleted so make could not reuse a same-second build, and after every run the mutation was undone by hand and `git diff` showed the source back as it was.
A test that never went red does not count, so every test function of every binary has at least one row.
Rule and decision numbers are `chuiniu/docs/DECISIONS.md`'s.
Run 2026-09-27 on the kernel as of this commit, with `cn_fuzz 100` to `500`, `cn_msg_test 2` and the rest at their defaults; line numbers are the test files' as committed.

## cn_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| R1 R4 the start | `cn__new` puts seat 1 on turn | `cn_test.c:36` "n 2: seat 0 opens round 0" (every n), `:59` "one 2 is a bid" |
| R5 R7 the opening bid | `cn_can_call` drops `bid_q > 0` | `cn_test.c:51` "no call on the opening", `:52`, `:55` "every (q, f) ... 51", `:132` "the opener cannot call" |
| R5 R7 the opening bid | `bid_ok` allows `q == total + 1` | `cn_test.c:61` "R7: no more than the dice on the table", `:172` "bad move 2 refused" |
| R5 R7 the opening bid | `cn_legal` stops one quantity short | `cn_test.c:55` "... 45", `:57` "the top bid last", `:90` "call plus every rank from three 5s: 58"; `cn_fuzz.c:47` "the menu is every legal move (45 vs 50)" |
| R2 raising | `bid_ok` takes `>=` for the rank | `cn_test.c:77` "the same bid is not a raise", `:172`, `:173` |
| R2 raising | `cn_min_quantity` takes `f >= bid_f` | `cn_test.c:84` "faces up to 4 need four", `:95` "no face 6 above the top"; `cn_fuzz.c:55` "face 2: least 2, the table 1"; swift-smoke "above three 4s: four 2s to four 4s, three 5s and 6s" |
| R3 the call | the bid stands only when the count exceeds it (`count > q`) | `cn_test.c:110` "exactly the count: the bid stands, the caller loses"; `cn_fuzz.c:68` "the right loser" |
| R3 the call | `cn_count` does not count wild 1s | `cn_test.c:104` "three 3s plus two wild 1s plus one: 4", `:105`, `:109`, `:110`; `cn_plan_test.c:61` "the revealed dice count 3, the event 1" |
| R4 who opens next | the next round always opens with the seat after the loser | `cn_test.c:131` "the loser opens the next round", `:215` "n 3: 598 moves, the bound 609"; `cn_twophone_test.c:179` "Alex sees the table as it is", `:183` "the menu" |
| R1 the end | the winner is the loser | `cn_test.c:155` "seat 0 out, seat 1 wins"; `cn_fuzz.c:82` "one winner", `:83` "one die per call" |
| refusal leaves the game untouched | `cn__apply` records `last_seat` before the legality check | `cn_test.c:173` "bad move 0 changed nothing" (every bad move), `:159` "and the game untouched" |
| replay equals apply | `cn_replay` applies one move fewer | `cn_test.c:192` "game 0 same at 9"; `cn_fuzz.c:77` "replay equals apply" |
| the longest game | `cn_min_raise` stops one quantity short of the table | `cn_test.c:215` "n 2: 228 moves, the bound 279", `:216` "rounds 8" |

## cn_dice_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| K3 the recipe | the log salt reads `chuiniu.log.2\|` | `cn_dice_test.c:58` "game 0 round 0: the kernel's dice are the recipe's", `:74` "round 0 of seed 2026 at three seats", `:78` |
| K3 golden | seat `s` reads block `s`, not `s << 32` | `cn_dice_test.c:74` "round 0 of seed 2026 at three seats", `:78` "round 1, seat 1 on four", `:58` |
| K2 same state, same dice | `cn_roll_round` folds a static roll counter into the key | `cn_dice_test.c:97` "game 0 move 0: identical", `:74`, `:222` (the bridge's links stop encoding); `cn_msg_test.c:64` "the start: encodes (-6)": the encoder's read-back refuses a game whose dice do not replay |
| K2 a different parent, different dice | the round key hashes an empty log (`cn_log_digest(g->hist, 0, ...)`) | `cn_dice_test.c:130` "an earlier bid re-rolls the next round (400 of 400 alike)", `:78` |
| K3 per seat, per round | the round key leaves the round out | `cn_dice_test.c:159` "a seat alike across rounds 2000 times in 2,000" |
| K2 the cancel exploit, through the bridge | `cn_api_text` applies the staged move to the resident itself, so a cancel no longer takes it back | `cn_dice_test.c:236` "two 4s again", `:241`, `:243` "round 2's dice are byte-identical"; `cn_twophone_test.c:86` "committed"; `cn_api_smoke.c:97` "committed" |
| K2 the cancel exploit, through the bridge | staging refuses while a move is staged (no replace) | `cn_dice_test.c:241` "staging replaces: the call again", `:243`; `cn_twophone_test.c:266` "Alex stages 3 2, replacing it" |
| K8 a staged call reveals nothing | the staged plan is not cut at the CALL | `cn_dice_test.c:261` "the staged plan is the call alone", `:263` "the staged beats are the stamp alone" |
| K8 a staged call reveals nothing | `cn_api_view` shows the resident with the staged move applied | `cn_dice_test.c:258` "the view is the committed one", `:259` "no reveal while staged"; swift-smoke "nothing lifts while staged" |
| K8 a staged call reveals nothing | the caption composer does not gate the LOSE clause | `cn_dice_test.c:266` "the staged caption names the call only: Bo calls three 5s. Bo calls. Three 5s was true, Bo loses a die" |

## cn_plan_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| plan: the events of each move | the REVEAL event's count is one more than the table's | `cn_plan_test.c:54` "the events are the state's", `:61` "the revealed dice count 3, the event 4" |
| view: masking | `cn_view` fills `all[]` for every viewer | `cn_plan_test.c:101` "seat -1: other cups do not reach the view", `:110` "all[] only for CN_VIEW_ALL"; `cn_twophone_test.c:178` "Alex's view carries nobody's cup" |
| view: masking | my own dice are copied unsorted | `cn_plan_test.c:107` "sorted"; `cn_twophone_test.c:177` "Alex's die 0" |
| view: the menu and the reveal | `revealed` stays 1 after the next bid | `cn_plan_test.c:168` "a bid ends the news, the last call stays readable" |
| view: the menu and the reveal | `shown_counts` leaves out the wild 1s | `cn_plan_test.c:152` "die 5 counts exactly when a 6 or a 1", `:154` "the flags are the count (6)" |
| beats: the plan on a clock | a DROP lowers the count at its start, not its end | `cn_plan_test.c:250` "the die is still there until the drop ends" |
| beats: the plan on a clock | a SHAKE does not move the frame's round | `cn_plan_test.c:206` "the end is the settled board" |

## cn_say_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| the table | SUB_NONE ends in a full stop | `cn_say_test.c:29` "SUB_NONE ends in a full stop", `:93` |
| things | a sentence-initial quantity uses the lower-case word | `cn_say_test.c:51` "four 3s", `:55` "twelve 2s", `:117`, `:128`; `cn_twophone_test.c:250` "Bo calls. seven 2s was false, Alex loses a die" |
| captions | the caption composer does not gate the LOSE clause (K8) | `cn_say_test.c:115` "a call's caption: Bo calls four 3s. Bo calls. Four 3s was true, Bo loses a die", `:130`; `cn_twophone_test.c:227` |
| the screen at the end | the win headline goes to every seat but the winner | `cn_say_test.c:155` "game 0: Alex wins", `:160`; `cn_twophone_test.c:295` "Alex: You win" |
| the lobby and the errors | CN_EFORMAT says the link is damaged | `cn_say_test.c:201` "This game link is damaged" |
| the lobby and the errors | the joined caption uses the invite line | `cn_say_test.c:193` "Bo wants a game of Chui Niu. Tap to join"; `cn_twophone_test.c:152` |

## cn_fuzz.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| fuzz: dice on the table | a call leaves `total` alone | `cn_fuzz.c:63` "a call takes exactly one die (10 -> 10)", `:38` "total 10 is the counts' 9" |
| fuzz: the loser | `count > q` | `cn_fuzz.c:68` "game 0: the right loser" |
| fuzz: the menu | `cn_legal` stops one quantity short | `cn_fuzz.c:47` "the menu is every legal move (45 vs 50)" |
| fuzz: the picker's table | `cn_min_quantity` takes `f >= bid_f` | `cn_fuzz.c:55` "game 0 face 2: least 2, the table 1" |
| fuzz: one winner | the winner is the loser | `cn_fuzz.c:82` "game 0: one winner", `:83` |
| fuzz: replay | `cn_replay` applies one move fewer | `cn_fuzz.c:77` "game 0: replay equals apply" |
| fuzz: the body | `cn_code_encode` folds the digits forwards | `cn_fuzz.c:86` "game 0: the body round-trips" |

## cn_msg_test.c

| Test | Mutation | Assertion that went red |
|---|---|---|
| round trips and sizes | `cn_code_encode` folds the digits forwards | `cn_msg_test.c:64` "a bubble: encodes (-6)": the read-back refuses what the decoder cannot say, `:183` "pool 3 reads" |
| the worst case fits MSMessage.url | every digit stored in base 65,536 (both coders) | `cn_msg_test.c:64` "the longest game: encodes (-6)" at five and six seats (the body passes CN_CODE_MAX) |
| the hostile sweep | the check is not compared | `cn_msg_test.c:199` "pool 0 byte 3 ^01: read as another message" (134,914 of 283,815 read), `:259` "unsealed, the check refuses it" |
| the hostile sweep | the decoder drops the name-length bound (`n - at < len`) | ASan: heap-buffer-overflow READ in `cn_msg_decode` (`cn_msg.c:354`) from `cn_msg_test.c:186`, the prefix sweep, in `make asan`; the plain build stays green, which is why `asan` runs the same sweep |
| tampering | the decoder does not compare the header's phase with its replay | `cn_msg_test.c:245` "a live game claimed finished: 0, want -6", `:246` "an unknown phase" |
| the lobby | the full-table exemption for the newest joiner is dropped | `cn_msg_test.c:308` "the sixth joins and starts", `:309`, `:312` "the newest joiner waits unless full", `:331` "a DM: join and start" |
| Rule P and common moves | fewer moves win | `cn_msg_test.c:345` "more moves win"; `cn_twophone_test.c:207` "the newer bubble wins"; `cn_api_smoke.c:119` "prefer" |
| the seat resolver | `cn_msg_sender` always names the starter | `cn_msg_test.c:370` "and again" |

## cn_twophone_test.c

| Step | Mutation | Assertion that went red |
|---|---|---|
| the lobby | the joined caption uses the invite line | `cn_twophone_test.c:152` "Bo wants a game of Chui Niu. Tap to join" |
| a bubble | the next round opens with the seat after the loser | `cn_twophone_test.c:179` "Alex sees the table as it is", `:183` "the menu" |
| a bubble | my own dice unsorted | `cn_twophone_test.c:177` "Alex's die 0" |
| a bubble | fewer moves win Rule P | `cn_twophone_test.c:207` "the newer bubble wins" |
| a call | the outcome's bid in lower case | `cn_twophone_test.c:250` "Bo calls. seven 2s was false, Alex loses a die" |
| a call | the caption composer does not gate the LOSE clause | `cn_twophone_test.c:227` "Bo calls seven 2s. Bo calls. Seven 2s was false, Alex loses a die" |
| a bid | staging refuses while a move is staged | `cn_twophone_test.c:266` "Alex stages 3 2, replacing it", `:270`, `:273` |
| the end | the table reports no game phase once over | `cn_twophone_test.c:290` "Alex sees it finished" (every phone) |
| the end | the win headline goes to every seat but the winner | `cn_twophone_test.c:295` "Alex: You win" |
| a join that fills the table, then a leave | `shared/c/msg_lobby_roster.c` offered: the full table's START exemption dropped (the join that fills the table cannot start) | `cn_twophone_test.c:315` "the table is full: Bo, the newest sender, is offered Start (3) or a leave", `:327` "Bo joins and starts in one bubble" (green before these steps, 2026-09-27) |
| a leave once live is refused | `shared/c/msg_lobby_roster.c` can_exit: true after the start (`!l->started` dropped) | `cn_twophone_test.c:338` "Bo is offered no leave once live", `:339` "Bo's leave is refused", `:341` "the roster is unchanged: 1 seats, me 255", `:342` "the bubble encodes no departure" (green before these steps) |
| a join that fills the table, then a leave | `shared/c/msg_lobby_roster.c` offered: a lone seat offered START (the `n_seats >= 2` guard dropped) | `cn_twophone_test.c:321` "alone again, the newest bubble not his: Alex is offered Invite (1), not Start" (green before these steps) |

## ios/cn_api_smoke.c and ios/cn_api_smoke.swift

| Test | Mutation | Assertion that went red |
|---|---|---|
| C smoke | an unreadable link wins `cn_api_prefer` | `cn_api_smoke.c:120` "an unreadable one loses" |
| C smoke | a cold `cn_api_adopt` lays out nothing | `cn_api_smoke.c:79` "opened cold on the start: the cups shake", `:81` "shaking at 0" |
| Swift smoke | the same cold adopt | "the start's cups shake", "the frame at 0", "the frame at the end" |
| Swift smoke | `cn_api_view` shows the staged move | "nothing lifts while staged" |
| Swift smoke | `cn_min_quantity` takes `f >= bid_f` | "above three 4s: four 2s to four 4s, three 5s and 6s" |

## cn_stage_test.c and the stage through the bridge

Package D's rows (the stage, its C and Swift smokes, the Swift stage test) are in `chuiniu/c/docs_pkgD.md`, "Mutation checks": every test function of `cn_stage_test.c` went red at least once, and the smokes' stage checks named there did.
