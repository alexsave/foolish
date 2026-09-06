# The post-game analyser

A systematic, LLM-free review of a finished game: which decisions were mistakes, how large, and what the evidence is.
It is the tool `docs/ORACLE_ANALYSER_HANDOFF.md` (landing with `claude/kernel-lift-from-swift`) specified, built in C on the real kernel.
Nothing is wired into a product surface; it is ready and callable, with a CLI that proves it on a real recorded game.

Code: `c/src/analyse.{h,c}` (the analyser and the packed wire, reader beside writer), `c/src/main_analyse.c` (the CLI), `c/tests/analyse_test.c` (the recorded game, the proofs, the cost measurement).

## Running it

    cd c && make analyse
    ./build/cnitro_analyse --code=<base32 v6 code> --seat=1 --engine=robusta --worlds=24 --threads=8
    ./build/cnitro_analyse --code=... --seat=1 --engine=robusta --deep-engine=octogen --deep-nodes=3 --deep-worlds=64 --threads=8

`make analyse-test` runs the suite on the recorded game and prints the cost of one playout for the two cheap engines; `./build/analyse_test --cost` times every engine (minutes); `./build/analyse_test --code` prints the recorded game's code so the CLI can be pointed at it.
`make difftests` runs the suite too.

The parameters, with their defaults (`analyse_params_default`):

| flag | default | meaning |
|---|---|---|
| `--seat` | every seat | the seat whose decisions are reviewed |
| `--engine` | robusta | the bot that plays EVERY seat in a playout (a bot roster key) |
| `--worlds` | 24 | sampled worlds per decision, the same list for every candidate |
| `--exhaustive` | 512 | enumerate the worlds when the hand assignments are at most this many |
| `--futures` | 4 | deck orders per enumerated world when the stock holds more than one card, at most |
| `--candidates` | 0 = all | cap on candidates per decision; when it bites the node says so |
| `--solve` | 200000 | exact-solve node budget per (candidate, world); 0 turns proofs off |
| `--deep-engine` | none | a second engine for the K largest scan losses |
| `--deep-nodes`, `--deep-worlds` | 3, 64 | K and its world count |
| `--threads` | 1 | the world loop across threads; the bytes are the same at any count |
| `--seed` | 1 | the world sampling seed |
| `--raw=<file>` | | write the packed bytes |

Everything the CLI prints is read back from the packed bytes through `analyse_read_header` / `analyse_read_node`, so what it shows is what any other consumer of the bytes would see.

## What it measures

The pipeline, in the order the handoff laid it out.

**Rebuild.**
A v6 code is hidden-state-lossless, so the whole game is rebuilt through the real engine: `replay_deal_v6` gives the deck and the recorded actions, `replay_deal_start` deals it under the code's rules and checks the rebuilt deal against the recorded opener, `replay_action_apply` plays each action.
There is no second deck rebuild and no second ROUND_END translation; the analyser branches the same game the replay screen shows.
A ROUND_END is a decision for every attacker still to declare (throw in, or close the bout), taken in seat order before the transition.

**The belief.**
At each decision of the analysed seat the analyser builds what that seat could know from public information: its own hand, the table, the discard, the flip, and the cards it watched each opponent pick up and not yet play (the flip is pinned to whoever drew it last).
Everything else is the pool U of unlocated cards; each opponent has f_p free slots and the stock d cards.
The conservation assertion `|U| == d + sum(f_p)` is checked at every node; if it ever fails the node is flagged `BELIEF_FAIL`, the header says so, and the node carries no verdict.
There is no inference from behaviour (no voids, no rank floors): the analyser asks what was available to you, and that question has an exact answer.

**Worlds.**
A world assigns the pool to the free slots and the stock.
When the number of hand assignments is at most `--exhaustive` they are all enumerated (`EXHAUSTIVE`); the deck order within a world is fixed when the stock holds at most one card and otherwise sampled, up to `--futures` orders per world, spending the remaining budget (`FUTURES` says so, and such a node is not "every world").
Otherwise `--worlds` worlds are sampled.
Whichever way, the world list is generated ONCE per node and every candidate is evaluated on the same list with the same random streams and a cold transposition table, so the difference between two candidates is a paired difference.

**Evaluation.**
Every legal move is a candidate (at the production caps: the arena caps drop legal moves silently, so the analyser is never built at them).
For each (candidate, world) the world is installed into a clone of the real board, the candidate is applied through `handle_*`, and the game is played to the end by the engine at every seat, through `bot_drive`, the one bot cycle every host uses, with the roster's knobs installed.
The outcome is the analysed seat's finish position (1 = first out, N = the fool).
Where two seats are in and the stock holds at most one card the world is a game of perfect information, and each candidate is instead solved exactly: a full minimax over the real kernel with the bitboard solver taking the deck-empty tails, budget-bounded; a resolved solve is a proof for that world, an unresolved one falls back to a playout.
A node whose every (candidate, world) resolved carries `PROOF`.

**Verdicts.**
Per candidate: mean finish position, how often the seat was the fool, the paired mean and standard error against the played move, and the proof status (proven win in every world, proven loss in every world, or proven and mixed).
Per node: the best candidate (lowest mean; ties keep the played move), the loss (mean of the played move minus mean of the best, paired), and one word:

| verdict | meaning |
|---|---|
| FORCED | one legal move |
| BEST | the played move is the best candidate, or within 0.03 finish positions of it |
| DECLINED | a better move scored higher, but the paired 95% interval spans zero: the tool refuses to call it |
| MISTAKE | the interval excludes zero (under a proof, the point estimate decides) |
| DECISIVE | the last MISTAKE of the fool's seat after which every later decision of that seat was LOST or FORCED |
| LOST | every candidate is the fool in every world: nothing to be done |

The thresholds (0.03 floor, 1.96 sigma) were chosen by reproducing the hand analysis of the sample game, not by taste: its smallest real gap is 0.09 finish positions and its largest non-mistake 0.02.
The running win probability is the played move's column: `1 - P(fool)` after the move, per node.

**The deal report.**
Each seat's opening trump count with its exact hypergeometric probability (and P(at most that many)), plus every trump that entered the hand through the deal or a draw over the whole game.
"Eva was dealt no trump (15.2%)" and "Alex drew six of the nine diamonds" are these two numbers.

**The deep pass.**
With `--deep-engine`, the K scan nodes with the largest loss (excluding forced, lost and proven ones) are re-evaluated by the second engine at `--deep-worlds`; the node carries both results, says whether the deep pass agreed on the best move, and the deep result owns the verdict.
This is the handoff's two-pass structure: the cheap scan is the per-move strip, the deep pass is the headline.

## The wire

`analyse_packed(code, code_len, params, out, cap)` returns bytes written or `-ANALYSE_E*`; `analyse.h` documents every byte.
Little-endian integers, finish positions x1000, probabilities x10000, nothing floating.
The reader (`analyse_read_header`, `analyse_read_node`) refuses every truncation with `ANALYSE_ETRUNC` rather than reading a shorter analysis; the test proves that at every cut of a real result.
There is no JSON anywhere in this: a host that wants the result decodes the bytes, the way the `fio_*_packed` entries are consumed.
A `fio_analyse_packed` wrapper over the resident game's code is a few lines; it is deliberately not added, because nothing is wired up.

## What it cannot see

State these before quoting a number.

1. **The playout opponent is a bot, and its own rollout model declines the trump throw-in.**
   The analyser does not use the Oracle's rollout mean.
   Every playout is a real game in which the engine at each seat makes its own decision by its own search, so at the moment the refutation is available (the trump king thrown onto the king she just laid down) octogen or robusta decides it by Monte-Carlo, not by `sim_trump_attack_prob`'s 2%.
   But the Monte-Carlo bots' INTERNAL rollouts (`cordite_sim.c`) still use that 2% gate while the deck lives, so a bot deep inside a playout may still misjudge a line whose value depends on a throw-in it models as rare.
   The exact-play region is immune to this (no policy is consulted); everywhere else "48 of 48 win" means against this engine's play, and the deep pass across a second engine is the only robustness check offered.
   The handwritten engine is the cheap scan option and is exactly the policy the Oracle rolls out; its verdicts inherit that policy's blind spots wholesale.

2. **The Oracle cannot report a move octogen never considered; the analyser can, but only within its own caps.**
   `og_pick_candidates` keeps at most ten covers ranked by product of card scores and can rank the only full cover out.
   The analyser scores every legal move at the production caps (`MAX_LEGAL_MOVES=4096`, `MAX_MOVE_CARDS=28`), and `--candidates` is off by default.
   If `--candidates` is set, or a menu outruns the caps, the node carries `CAPPED` and the dropped moves are simply not there; the ranking that survives keeps the played move, pickup and good, then the cheapest and largest of each family.
   At the three uncovered eights of the sample game the analyser lists all 27 moves, and the full cover octogen never scored is scored here.

3. **Engine-relative frequencies.**
   Outside the proof region every number is a win rate against THIS engine's replies.
   Two engines agreeing is weaker than it looks: they share ancestry and rollout policies; and on the sample game they did not even agree, octogen naming a different best move than robusta at every node the deep pass touched.
   "There was a better move" survives that; "this was the better move" does not.
   Only `PROOF` nodes are proofs, and the wire says which are which.

4. **The opponent in a playout is a bot; the game was played by a human.**

5. **Deck order is sampled when the stock holds more than one card.**
   An `EXHAUSTIVE` node with `FUTURES` enumerated every hand assignment but not every deck order; it is "W worlds x R futures" and the CLI says "deck order sampled" rather than "every world".

6. **Perfect recall.**
   The pinned model assumes the seat remembered every pickup.
   Right for "what was available to you", wrong for "what a human could reasonably have found"; nothing here explains a verdict in terms a human can act on.

7. **Sampling resolution.**
   At 24 worlds the paired interval on a 2-player node is about +-0.28 finish positions, so a scan at that budget calls only gross mistakes and declines the rest; the sample game's robusta scan declines 12 of its 30 decisions.
   The sharp verdicts come from the exhaustive and proof regions and from the deep pass; shrinking world counts to make an engine fit is exactly the wrong lever, because the paired power is what makes small differences readable.

8. **More than two seats is under-exercised.**
   The belief, the enumeration and the playouts are general (the arena-caps test analyses 2 and 3 seat games for every seat), but exact play is heads-up only and the verdict thresholds were tuned on a 2-player game.
   An analysis holds at most 512 decisions and a node at most 255 candidates and 4,096 worlds; past those the scan stops, the candidate list is capped (and says so), and the world list is truncated.

9. **Bots read the log.**
   Installing a sampled world into a board whose log is the real history is safe because the belief bots reconstruct from table events and never read a draw's identity from the log; that was checked for every bot the roster can name (`LOG_DRAW` is read only to pin the flip).
   If a future bot reads draw identities off the log, its playouts will see cards that are no longer where the log says, and this is the first thing to suspect.

## Measured cost

The handoff's first instruction was to measure one playout per engine before anything else.
On this Mac, one thread, from a mid-game node of the recorded game (`./build/analyse_test --cost`):

| engine | ms per playout |
|---|---|
| handwritten | 0.02 |
| robusta | 7.6 |
| cordite | 46 |
| blackpowder | 58 |
| octogen | 531 |

A scan of one seat's 30 decisions at 24 worlds is about 9,900 playouts (every legal move, not six): robusta 30 s on one thread, 9.3 s on eight; handwritten under a second; octogen would be about 90 minutes on one thread.
The two-pass review the handoff describes, on the same seat, eight threads:

| pass | playouts | wall | CPU |
|---|---|---|---|
| robusta scan, 24 worlds | 9,900 | 9.3 s | 38 s |
| plus octogen deep pass, 3 nodes x 64 worlds | 11,628 | 3 m 24 s | 16.5 min |
| octogen everywhere, 24 worlds | 9,900 | 11 m 19 s | 52 min |

All three name the same decisive moment (the cover of the last eight, a proof) and the same LOST tail; the engines differ on the sampled nodes.

In that run the deep pass DISAGREED with the scan on the best move at all three nodes it re-examined (`deep DISAGREES` on the strip), while agreeing that the played move was not it.
That is the engine-relativity of "what you should have played" made visible, and the reason the wire carries both results rather than one.

What this says about the product question, recorded and not decided: an octogen-everywhere review is a server job or a long background job, a robusta scan plus an octogen deep pass is minutes on a laptop core and tens of seconds across cores, and a handwritten scan is instant but is the Oracle's own policy with all of its blind spots.
The world loop is embarrassingly parallel and the result is identical at any thread count, so a client-side build inherits whatever threads the host has.
Every bot's scratch, the solver's table and the engine's RNGs are already per thread (the native OMP arena relies on it); `bot_drive`'s menu scratch joins them here.
Three kernel globals are still shared and are written with identical values by every thread (`engine_last_reject`, `engine_snap_hook` around a bot's choose, the dropped-log sink `g_log_sink`); a sibling change (PR #118, "the last shared kernel globals go thread-local") retires that caveat and nothing here depends on it.

## The sample game

`c/tests/analyse_test.c` carries the recorded game as its deal and its move list and encodes it to a v6 code at test time (a frozen code would rot with the format; a deal and a move list do not).
On it the analyser reproduces the brief's findings by exact play, not by playouts:

- at the cover of the last eight (step 50) the jack cover is a proven loss in every one of the five worlds the belief admits and the king cover a proven win in one, so the node is a MISTAKE under a PROOF, and it is the DECISIVE moment;
- two steps later every candidate is a proven loss: LOST, "what was NOT the mistake";
- Eva was dealt no trump (P = 15.2%) and Alex drew six of the nine diamonds.

The brief enumerated six placements of the last deck card; the analyser enumerates five, because the queen of spades is pinned (Eva watched Alex pick it up), and the belief is right to exclude it.

## Verification

`make tests` (3245 cases at the arena caps, 39 of them the analyser's: the deal arithmetic, the verdict rule, the belief held against the engine's truth at every ply of 2, 3 and 4 seat games, and the packed entry end to end on generated games) and `make analyse-test` (79 cases at the production caps on the recorded game: the fixture, the belief at every decision of both seats, world legality, the proofs above, determinism, one thread against four, the deep pass and the reader).

Every test was mutation-checked: the thing it guards was broken on purpose, the suite was seen to fail, and the break was reverted.
The mutations are listed in each test file's header and were run as a batch (`scratchpad/mutate.py` in the session): a swapped deal card, a pinned card that stays pinned after it is played, a world missing a slot, an exact solve scored for the wrong seat, a world shuffle that ignores its seed, a reader that reads past the end, a bot scratch shared across threads, a writer that forgets the deep flag, a hypergeometric without its denominator, swapped DECLINED and CHANCE, and a header claiming one node too many.

<!-- MUTATION_NUMBERS -->

## Prior art

**`origin/claude/octogen-replay-explain`** (3 commits, 2026-07-10) is an earlier draft of the octogen deliberation X-ray that landed on main the next day as `12539c2` and grew there (`0341283` belief dump, `4e9a3ce` wasm drive, `383c5d4` multi-player).
Same tool, two paths: `cnitro/` is the kernel's old home, `c/` its current one.
Main's `c/tests/og_explain.c` is the branch's file plus the wasm-faithful reseed; main's `c/tools/og_explain/` has the branch's `build_data.py`, `gen_html.py`, `README.md` in rewritten, parameterised form plus the multi-player pipeline; the branch's `wasm_anatomy` edits are the pre-L1-budget wording of text main has since updated.
Nothing on the branch is missing from main in a better form.
The analyser reuses what that work established (the driven replay, the deal-from-moves idea) but not its code: the recorded game now enters through the real replay codec rather than a fixture loader.

**`origin/claude/oracle-mode-b`** (9 commits) is a shared-memory, multi-threaded wasm build of octogen's per-decision deliberation: N worker threads run `octogen_strategy_choose` over one shared board and fold per-candidate scores into a C accumulator, with the exact endgame verdict probe hoisted out of the explain build.
Does it solve the analyser's cost problem?
Partly, and in the right shape.
It does not reduce the work: the cost above is CPU time, and Mode B divides wall time by the thread count (its own benchmark shows near-linear scaling and a ~10% per-choose saving over the instance fleet).
What it establishes is that freestanding wasm threads with real TLS work, which is exactly what the analyser's world loop needs in a browser: the loop is already a fan-out over independent (candidate, world) cells with all scratch thread-local, so it maps onto that build directly.
The analyser was designed for it (one entry, packed result, a per-cell function with no shared state) but does not depend on it landing; natively the same loop runs over pthreads.
The cost problem itself is decided by the engine choice, not by the threading.

## To become a product surface

- Add `analyse.c` to the wasm source list with a single export, and reconcile the production caps against `docs/WASM_L1_BUDGET.md`: the analyser keeps one `Game` per thread, a `LegalMoves` at these caps is about 240 KB, and the (candidate, world) cell matrix is 2 MB static.
- A work-budget parameter so it can be interrupted, and a progress callback for a strip that fills in.
- The prose: the wire carries every number a sentence needs, and nothing here writes sentences.
- Which claims are shown as proofs and which as frequencies, in the UI, from the flags the wire already carries.
- Client-side versus server-metered: `docs/ORACLE_MONETIZATION_ENGINEERING.md` decided this for the current Oracle; the table above is the input for deciding it for this.
