# Post-game analyser: handoff

A systematic, LLM-free replacement for the hand analysis that diagnosed a real
game (see the worked example at the bottom).
Design and groundwork only.
Nothing is wired into a product surface.

**Held pending the Infinite Oracle endgame fix.**
The analyser must not rest on Oracle scores; see "The bug you must not build on".

## The monetization plan names this product

`docs/MONETIZATION_ROADMAP.md` §5, Phase 2, "Cordite Coach":

> **Post-game review ("What would Cordite do?").**
> After any game, a move-by-move review: accuracy score, blunder markers, "best
> move" with the belief-model's reasoning.
> One free review/day, unlimited with Foolish Premium (~$4-6/mo or $30-40/yr).

`docs/ORACLE_MONETIZATION_ENGINEERING.md` §11 prices a one-off **"single deep
review" at 25 Telegram Stars**, which is exactly the shape of a per-game
expensive computation.
§0 calls the whole thing "worth paying for as a conversion product, not a
business by itself", and the only Durak move-analysis product in the market.

### The tension the next agent must resolve consciously

The plan's economics assume **"the analysis runs on the buyer's own CPU"** with
**COGS = an edge-function token check**.
A paired-sampling, multi-engine, exhaustive-world analyser is not free.

- Server-side breaks the stated ~100% gross margin.
- Client-side wasm must fit a phone budget, single-threaded, in seconds - and
  wasm gets none of the `make OMP=1` parallelism native builds enjoy.

The plan also says the billable path is **metering, not a better analyser**.
Building a better one is justified on CORRECTNESS grounds - the shipped Oracle's
scores are provably wrong in the last plies, so the current wow feature produces
false verdicts in exactly the moments that decide games - but that is a different
argument from the monetization one, and should be said out loud rather than
blurred.

The plan wants per-move blunder markers; this design produces a verdict on the
decisive moment.
Chess.com's Game Review does both, and the two-pass structure below maps onto it:
the cheap scan pass is the per-move strip, the deep pass is the headline.

## What is already built, tested, and must not be re-rolled

In worktree `.claude/worktrees/agent-a22c16ea8962a3a29`, uncommitted:

- `c/src/replay_steps.h` +36: `REPLAY_MAX_ACTIONS`, `ReplayAction`,
  `replay_deal_v6()`, `replay_deal_start()`, `replay_action_apply()`.
- `c/src/replay_steps.c`: `rs_play` rewritten as a CONSUMER of those three.
  The collector writes into caller storage instead of a 4096-entry static.

This is the load-bearing lift: it hands an analyser the deal, the deck and the
recorded actions without a second copy of the deck rebuild.
Compiles clean; `cnitro_tests` 3203 passed; `make difftests` green including
`replay_difftest` (280 games, 322,827 checks) and `replay_v6_test` (316,992).

One behaviour change, untested either way: the deal-derivation check now runs
BEFORE the deal's step callback, not after, so a code whose rebuilt deal is wrong
no longer emits a deal frame before failing.

`analyse.c`, `analyse.h`, `main_analyse.c` do NOT exist.

## The pipeline

    cnitro_analyse --code=<base32>  [--seat=N]
                   [--scan-engine=robusta] [--scan-worlds=24]
                   [--deep-engine=octogen] [--deep-worlds=256] [--deep-nodes=6]
                   [--opp-engines=octogen,cordite,robusta,handwritten,blackpowder]
                   [--exhaustive=20000]

0. **Decode** - base32 to bytes, `replay_deal_v6`, then `replay_deal_start` +
   `replay_action_apply` per action. Built.
1. **Deal report** - each seat's opening trump count with its exact
   hypergeometric probability, plus whole-game trump supply per seat from the
   DRAW atoms. Exact, not sampled. This is "Eva was dealt no trump" and "Alex
   drew six of the nine diamonds".
2. **Belief model** - known-located = own hand + table + discard + flip + pinned;
   pinned[p] = cards p was watched picking up and has not since played; unknown U
   = deck minus known-located; free slots f_p = hand_count - popcount(pinned).
   **Conservation assertion: `|U| == d + sum(f_p)`.**
   If it ever fails the tool must say so rather than print a verdict.
   That one line is what catches a silently wrong world sampler.
3. **World installation** - shuffle U, deal f_p alongside pinned, remainder
   becomes the deck, set `deterministic_deck` so the installed order IS the
   future.
4. **Node evaluation** - every candidate x every world seed from a FIXED SHARED
   LIST, played out to a fool. Paired differences, not independent binomials.
5. **Exhaustive mode** - when the world count is under the cap, enumerate instead
   of sampling. Deck ORDER is a second dimension: `d <= 1` is a proof, `d > 1` is
   "W worlds x R futures" and must never be called one.
6. **Verdicts** - LOST (best candidate wins 0 worlds: the "what was NOT the
   mistake" line) / DECISIVE / CHANCE / DECLINED (paired CI spans the threshold;
   print under honourable mention and refuse to call it).
7. **Robustness** - re-run top nodes against several engines. "There was a win
   available" surviving all of them is the printable claim; "this card was the
   winning card" is engine-specific and must be hedged.
8. **Running win probability** - free, it is the played-move column of the scan.

## Do this first

**Measure one playout per engine before writing another line.**
It was not measured, and it is the whole product question: it decides whether
this is a client-side wasm feature or an async server job.
Estimate only, to be falsified rather than quoted: a scan pass is roughly
30 nodes x 6 candidates x 24 worlds = ~4,300 playouts and the deep pass adds
~9,200; with octogen at every seat a playout is plausibly 10-100ms, putting a
full analysis in minutes to tens of minutes single-threaded.
Robusta on the scan pass should be seconds.

Then build the fixture generator (trap 13) so the analysed game exists as a real
replay code, then the belief model and its assertion, then evaluation, then
verdicts. Prose last.

## Traps

1. `branch.c` (scratchpad `ogx/`) hardcodes 2 players, takes first-attacker from
   argv, overwrites hands AFTER `start_game`, and analyses seat 1. It is a
   fixture loader for one game. Do not port it; use the checked path instead.
2. Replay formats 5-8 are dead (deal order changed, renumbered to 9/10). Only
   v9/v10 codes exist.
3. The v6 line is hidden-state-lossless - DEAL and DRAW atoms carry real card
   ids, hands are exact and never retrodicted. That is why this is possible.
4. **Paired sampling is not optional.** Same world-seed list for every candidate
   at a node, then a paired difference statistic. Two independent binomial CIs
   throw away most of the power and you will decline calls you could have made.
5. **The Oracle's MC scores are wrong in the last plies.** Never let a verdict
   rest on them. Everything must be a played-out game or an exact solve.
6. Installing a fake world into a Game whose log is real history is SAFE, and
   understanding why matters: the no-cheat contract means bots read only their
   own hand plus public state, and the belief bots reconstruct from table events
   rather than trusting the log's card lists. So keeping the log lets octogen and
   cordite play at full strength. If a playout ever behaves impossibly, suspect
   this first.
7. **Cap sets must not be mixed and the analyser needs the wide ones.** Native
   defaults drop moves past the cap SILENTLY IN ENUMERATION ORDER, so the winning
   move can simply be absent from your candidate list. Use the iOS caps and give
   `cnitro_analyse` its own object directory, as `ios-lib` does. A `LegalMoves` at
   those caps is ~475KB: do not put one on the stack.
8. Run bots through `bot_roster_choose`, not `*_strategy_choose` directly - the
   roster installs each bot's knobs, and the brain at C defaults is a DIFFERENT
   bot from the one the user played.
9. `draw_index` returns 0 when `deterministic_deck` is set. Set the flag
   explicitly after installing a world.
10. `cd_sim_solve()` is a real proof engine for the deck-empty phase, difftested
    against the struct solver at np=2,3,4 with 0 mismatches. Before the deck
    empties you still need world enumeration.
11. Base32 decode is not in `CORE_SRC` - it is a static in `c/ios/ios_api.c`
    (stops at `-` for the extras suffix). Lift it; do not write a third one.
12. `TUTORIAL_MOVES_CODE` in `src/components/tutorialGame.ts` is a valid 3-player
    43-step code for smoke testing. It will not exercise the 2-player exhaustive
    path.
13. **You cannot easily encode the analysed game.** `replay_encode_v6_from_game`
    needs the deal SEED, which a finished Game cannot answer. Use the lower-level
    `replay_encode_v6`, which takes the reveals explicitly, and feed it
    `deal.txt` / `moves.txt` from the ogx scratchpad. That gives you the exact
    game the hand analysis was done on, which is the only ground truth.

## The honest weakness list

- **Cost is unquantified and it is the product question.** If octogen-quality
  playouts cost minutes, the options are an async deep-review job (which matches
  the 25-Stars SKU) or a cheaper playout engine and weaker verdicts. What must
  NOT happen is quietly shrinking world counts until it fits, because the paired
  power is what makes small differences readable.
- **"What you should have played" is engine-relative.** A win rate measures wins
  against THIS bot's replies. Five engines agreeing is weaker than it looks -
  they share ancestry and rollout policies, so they are correlated. Only the
  exhaustive-plus-exact-solve region is a proof. Say which claims are proofs and
  which are frequencies, in the output.
- **The opponent in a playout is a bot; the game was played by a human.** "48 of
  48 win" means against a bot's defence.
- **Exhaustive mode ignores deck order when `d > 1`**, so "every world" is a
  half-truth away from the endgame. R was never chosen and the degradation as `d`
  grows was never checked. This is where an over-confident verdict appears first.
- **The pinned model assumes perfect recall.** Right for "what was available to
  you", wrong for "what a human could reasonably have found". A tool that says
  you should have tracked eleven cards through four pickups is correct and
  commercially useless. The plan's own example - "by move 12, Cordite knew your
  opponent held no spades, here's why" - suggests the EXPLANATION matters as much
  as the verdict, and nothing was built toward that.
- **n > 2 is under-designed**, and most real games are not heads-up.

## Sketch, not decided

Prose templating (nothing written), n>2 exhaustive enumeration, deck-order
sample count R, the thresholds separating DECISIVE / CHANCE / DECLINED (choose
them by reproducing the hand analysis, not by taste), and whether to report the
OPPONENT's mistakes too - cheap, same machinery, probably a better product.

## To become a product surface

Split `analyse.c` / `analyse.h` from `main_analyse.c`, add to the wasm source
list with a single export, reconcile the wide caps against the wasm memory budget
(`docs/WASM_L1_BUDGET.md`; a 475KB `LegalMoves` is a real problem there), add a
work-budget parameter so it can be interrupted, and decide client-side versus
server-metered - which the monetization doc has decided for the CURRENT Oracle
but not for anything this expensive.
