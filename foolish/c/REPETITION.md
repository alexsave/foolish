# The repetition draw

A game is over, a draw, when the same position occurs for the third time, by any route, as in chess.
Every seat still in is then a fool, and the seats that already went out keep their places.
At two seats both are fools.

## Why

Bot Cup game 0e68bd (sapper v haiku) looped: an eight-move cycle, two pickups a pass, from its 73rd move to the end of its stored log and on past it.
Neither bot remembers a position, and a rollout that never ends scores the same as a loss, so nothing in either brain breaks a loop.
Before this rule the engine played the whole stored log with the game still running.
With it the game ends at the 98th move, the third time the position after the 73rd move comes back.
`tests/repetition_0e68bd.h` holds the game's seed and moves, and `tests/tests.c` `test_repetition_ends_game_0e68bd` holds the engine to that move.

An arena of about 69,000 seeded bot games at 2 to 8 seats found ten more loops, all at 3 to 8 seats, each repeating every six pickups.
One of them, 3 seats of handwritten, loops with nine cards still in the stock: every hand holds six or more, so no pickup draws.
`test_repetition_ends_the_arena_loops` holds the eight that still loop on the kernel the rule landed on to their draw (two robusta games stopped looping when the bot memory changed robusta's play).

## Where it is checked

Only right after a pickup that drew nobody a card (`game.c` `handle_pickup`, then `rep_checkpoint`).
A pickup that refills a hand changes the stock, and a bout that ends on the discard changes the discard, so no position can come back across either.
The history starts over whenever the stock count, the discard length or the set of seats still in changes.

## What a position is (`game.h` `GameRepPosition`, `game_rep_position`)

Everything that matters for play: the rules bits, the seat count, the trump suit, the first attacker and the defender, whether the trump is still under the stock and which card it is, the discard length, the stock count and its cards, the goods and the good timestamp, the table's battles, each seat's status, awaiting flag and hand, and the out seats in order.
At a checkpoint the table is empty and nobody has said good, but every field is folded anyway, so the definition is the state and not an argument about it.
Hands are sets: their order is a display, not play.
The stock is a set too: it cannot reorder without a draw, and a draw starts a new history.
The fields are folded in that order, each through splitmix64, into a 64-bit fingerprint.

## What a Game keeps (`game.h` `GameRepetition`)

A ring of the fingerprints of the last 16 checkpoints of the current history, plus the history's stock count, discard length and seats in.
The third occurrence is the current fingerprint found twice in the ring.
The ring is 136 bytes, the Game's last field, after `logs`, so no Monte-Carlo clone, solver child, sampled world or snapshot copies it: `offsetof(Game, logs)`, the clone and slot sizes and the rollout clone are what they were.
A field past the logs lies beyond every head-sized or short-log slot, so nothing touches it unless the Game's owner attached it as a full Game (`game_rep_attach`, the one-byte `Game.rep_live`, in what was padding).
Every copy of a head into other storage goes through `game_copy_head`, which clears that byte, and `tests/rep_head_copies.sh` fails `make tests` on any other copy of a Game prefix.
Every deal attaches its own Game, unless the Game marks itself a short slot (`log_cap` or `log_room`), and a Game loaded rather than dealt is attached by its owner (`table_init`, the wasm bridge's state load); a clone, a sampled world and a client's masked board play without the rule.
Between two operations the server keeps a game resident as its head and the ring kept apart (`TableResident.rep`), and the resident encodes to the same row a load reads.
`Game.logs` is never read: the server does not load the session log for a human move, keeps only its first 1024 records when it does, and stores no more than 1024 (game 0e68bd's stored log stops there, mid-loop).

## Two honest limits

A position that comes back further apart than the ring is not caught: the third occurrence needs the two before it in the ring, so a position that recurs every p checkpoints is caught when 2p is at most 16.
One that recurs every 8 checkpoints is caught on its third visit, one that recurs every 9 never is (`test_repetition_window_is_the_ring`).
Every loop the arena found repeats every 2 or 6 checkpoints.
Two different positions share a fingerprint with probability 2^-64 per pair, and such a pair would be called a repeat.

## The rule has one owner

`game_rep_position` and `game_rep_checkpoint` are the fingerprint and the ring.
The engine calls them from `handle_pickup`, and the replay codec's own model (`replay.c` `RModel`) fills its own position and calls the same pair, so a drawn game's v6 code ends on the same pickup and its header names the fools.
A board with a hidden card in a hand or in the stock (a host's masked mirror) keeps no history; the draw reaches it on the authoritative board.
For the same reason the server sends the operation that ends a game as a draw as the board cast, never a delta (`table_cast_delta_ok`).

## The outcome

The game's own status says GAME_OVER with two or more seats still in.
`game_done` returns the lowest of them, so `>= 0` still means over to its callers; `game_fools` is the mask of every fool and `game_drawn` says whether there is more than one.
`game_fools` reads the status and the elimination order only, so it answers the same on a live board, a parked one and a host's board.
`anim_finish_rows` ranks every seat the elimination order does not name at the last place, by seat, so every end screen shows each fool.
`TableView.drawn` lets a screen say "Draw by repetition" (`result_draw`, `ios.msg.draw` in `c/i18n`) instead of naming one fool.
Ratings score a half between two seats that share a place: with c the win's change, c against a seat below, c - 10 against one above, c - 5 against an equal (`table.c` `elo_deltas`).
Where one seat must stand for the fools (the iMessage rematch opener, `TableCommit.fool`, `ReplaySummary.fool`, the history row's fool) it is the lowest.

## The durable row

The server loads a game from its row for every move, so the history is in the state blob from v7 on, as section 2 of the sections v6 introduced for the bot memory (`view.h` `STATE_BLOB_FORMAT_V7`, `STATE_BLOB_SECTION_REPETITION`), after the bot memory's section 1.
It is a format number of its own rather than a second tag under v6 because v6 deployed first: a v6 kernel refuses a tag it does not know, so a rollback refuses a v7 row at its format byte, and a v6 row, which can never hold a ring, reads as a game with no history.
Only the entries in use are written: no section at all for a game with no history, and otherwise a 3-byte section header and 7 bytes plus 8 per checkpoint, at most 138 bytes.
A v5 row reads as a game with no history yet.

## Bots and the solvers

The bitboard sim (`cordite_sim.c`) has no history and never calls a draw.
A rollout that loops runs out of turns and scores last place, which is what a draw is for every seat still in, so the valuations agree.
The exact solver (`cd_sim_solve`) is untouched: treating a repeated position as a draw inside a search is the graph-history error `ENDGAME_CYCLES.md` measured, and the sim's searches never see the rule.
Game-level searches (`bot_solve`, astrolite, the analyser's exact pass) copy the real history with the head, so a draw they reach is the real game's, and they score it as a loss for every fool.
The difftests stop where the engine's game is over, so a draw ends a difftest game rather than diverging from the sim.
