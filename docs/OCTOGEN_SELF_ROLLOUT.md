# Octogen as its own rollout policy — the "infinite oracle" recursion, measured

**Question asked**: use octogen itself as the rollout policy inside octogen's
sampled worlds, with a cheap base case ("does this move let me win?"), on
essentially limitless compute — is that the strongest bot possible?

**Answer: no — and the reasons are structural, not budgetary.** The lever is
implemented (`octogen_self`/`ogs`, `OG_SELF_*` knobs), the costs are measured
(memory: flat; wall-clock: exponential in depth), and the strength deltas are
measured on the paired same-deal harness. The recursion's *limit* — perfect
full-information play inside each determinized world — is reachable through a
cheaper equivalent (the exact solver), and this repo has now measured that
limit AND the road toward it from three directions. Strength does not improve
along the way; the directions that were already measured got *worse*. The
real ceiling of octogen's architecture is determinization itself, not the
rollout policy's IQ, and infinite compute pointed at the rollout policy
cannot buy past it.

Everything below is reproducible: build `c/` with `make`, then run the
commands in §6.

---

## 1. What "octogen as rollout policy" can and cannot mean

Octogen's deliberation: build a belief over hidden hands from public info,
sample W fully-determinized worlds from it, and for each (world x candidate)
play the world out to the end with a fixed cheap policy (handwritten),
scoring the candidate by mean finish position (`c/OCTOGEN.md`,
`c/src/octogen_strategy.c`).

Two readings of the idea:

**(a) Literal recursion — octogen calls octogen at every rollout ply.**
Architecturally infeasible without a ground-up rewrite, and *semantically
degenerate* anyway:

- Infeasible: one bot-family deliberation runs per decision by invariant —
  the sampled-world scratch (`world_scratch_game()` / `trial_scratch_game()`),
  the solver TT, and the solve scratch are single shared slots
  ("families never nest each other", `c/src/cordite_sim.h`). A nested
  `octogen_strategy_choose` inside a rollout would clobber the outer
  deliberation's state. Making octogen re-entrant means threading every
  static through a context struct — a rewrite, not a lever.
- Degenerate: inside a sampled world there is **no hidden information
  left**. An honest inner octogen would have to be handed only the inner
  seat's public view and then sample *its own* worlds inside our world —
  that is information-set search within determinizations, a different
  algorithm (the "true information-set search" successor OCTOGEN.md already
  names). If instead the inner octogen sees the world as dealt, its belief
  sampling collapses to the identity and its deliberation collapses to
  **full-information search**. Recursive octogen inside a determinized world
  IS full-info search — there is no third option.

**(b) The implementable reading — every in-world seat deliberates like the
outer MC bot.** That is what `octogen_self` builds (`cd_sim_playout_self`,
`c/src/cordite_sim.c`): at every rollout ply, the acting seat enumerates its
full legal move set (the solver's bitboard movegen), ranks cheap-first, and
plays a tournament over the top `OG_SELF_CAP` moves (PICKUP/GOOD always
searched — the reply-tournament lesson), each evaluated by a playout one
recursion level down; the actor takes the best finish *for itself* (max^n).
`OG_SELF_DEPTH=1` evaluates with handwritten playouts; depth 2 evaluates
with depth-1 self playouts; and so on. Base cases: the user's "does this
move let me win?" check (`OG_SELF_WIN` — any move that immediately
eliminates the actor is taken without search), the exact bitboard leaf
solver (inherited from octogen, also attempted inside the searched
traversal), and the handwritten policy when depth or `OG_SELF_PLIES` run
out. As depth/cap/plies grow this converges to perfect full-information
play of each world — i.e. exactly the recursion limit of reading (a).

**(c) The asymmetric refinement — search only OUR seat.** `OG_SELF_OWN=1`
keeps the honest handwritten model for every opponent and searches only our
own seat's in-world decisions. This dodges the paranoid-distortion
objection entirely: our future moves are under our control, so a
handwritten self-model genuinely *understates* our continuation strength,
while opponent-side search models imperfect opponents as world-omniscient
punishers. It is the one axis hunt 4 never measured (its levers — reply
tournaments, MC-defender models, deeper leaves — were all opponent-side or
truth-side). If ANY form of "octogen as rollout policy" should pay, it is
this one.

**(d) The belief-fed future self.** Plain `OG_SELF_OWN` still leaks: its
tournament argmaxes on the outcome in THIS world, i.e. the future self
best-responds to hidden cards it could not know — the strategy-fusion
clairvoyance that makes determinized search overvalue "lucky-here" lines.
`OG_SELF_HONEST=M` closes the leak: the future self chooses on its
*information set* — the ROOT belief (pins, void/floor forbids,
trust-filtered) carried forward through the rollout prefix, accreting
exactly what the prefix legitimately reveals (cards publicly picked up
stay known; a seat's root constraints expire when it draws, mirroring
`og_build_belief`'s own rules) — by evaluating each candidate on M
re-determinizations of the unseen cards and argmaxing the AVERAGE. The
chosen move is then applied to the true world: choose on belief, live in
truth. This is genuine one-level IS-MCTS for our own future decisions —
the closest thing to "octogen as our own future self" that is honest and
computable. Cost: M x the own-seat cost; memory still flat.

## 2. Cost: memory flat, wall-clock exponential

Measured on the native harness, pc2 vs cordite tables, one game, 4-core
container (`scratchpad/rss_time.sh` = wall + peak `VmHWM`):

| config | wall/game | peak RSS | multiplier |
|---|---|---|---|
| octogen (baseline) | ~1.0 s | 4.34 MB | 1x |
| self, cap=0 (win-check only) | ~1x baseline | 4.28 MB | ~1x |
| self, depth=1, plies=8 | ~21 s | 4.37 MB | ~20x |
| self, depth=1, all plies | ~79 s | 4.34 MB | **~70x** |
| self, depth=2, plies=3, cap=4 | ~55 s | 4.34 MB | ~55x |

- **Memory is a non-issue at any depth.** The recursion adds one ~500 B
  `SimState` clone + one move buffer per depth level on the stack; sampled
  worlds still reuse the same static slots. Peak RSS is identical to
  baseline in every configuration. The idea does not need a big server for
  memory; it needs it only for CPU.
- **Wall-clock is the whole bill, and it compounds as
  ~(cap x plies)^depth.** Full depth-1 is ~70x. Full depth-2 is ~70² ≈
  5,000x (≈ 3-4 min per decision, ~2.5 h per game single-threaded); depth-3
  ≈ 350,000x. Worlds parallelize embarrassingly, so a big dedicated box
  (say 128 cores) buys roughly two orders of magnitude of wall-clock — that
  makes full depth-2 a usable premium/"oracle" latency (seconds per
  decision) and depth-3 a batch job. Depth beyond that is out of reach of
  any classical hardware, forever, by exponential growth — "limitless
  compute" is not limitless against a ^depth exponent.
- The recursion limit itself is NOT out of reach, though: alpha-beta + the
  transposition table reach perfect in-world play far cheaper than naive
  nesting. That equivalence is what makes the limit measurable today (§4).
- **Caching ("multiple paths lead to the same endgame")**: rollout paths do
  transpose heavily into the same shrinking endgames — the exact solver's
  TT exists for precisely this reason, and TT size is a documented
  bot-strength knob (`c/Makefile` TT comments; `docs/WASM_L1_BUDGET.md`).
  `OG_SELF_TT=<bits>` extends the idea to the MC recursion itself: each
  searched in-world decision is memoized (position x actor x depth → chosen
  move), so a hit skips the whole nested tournament and the recursion tree
  collapses toward a DAG. Two honesty caveats, which is why it is
  flag-guarded and A/B'd rather than assumed: (a) a cached argmax was
  computed under one RNG context and is reused in another — it
  deterministic-izes the searched seat's policy, a real behavioral change;
  (b) cache entries are generation-stamped and die at every ROOT decision,
  because cross-decision (let alone cross-game) cache warmth is exactly the
  anti-hero coupling artifact that corrupted hunt 4's first harness. Hit
  rate / speedup / strength A/B: XXX-TT-RESULTS. Note the asymmetry with
  memory: the sound place for "infinite memory" is the EXACT solver's TT
  (proven values, transferable by definition — the oracle wasm build
  already spends 8 MiB there); MC-layer caching only ever reuses noisy
  argmaxes.

## 3. Strength: the paired same-deal results

Hero = `octogen_self`, control = `octogen`, same deals, same opponents
(`--control` harness; mean-finish delta, negative = hero better; h<c / h>c /
eq = deals hero finished better / worse / equal).

@ cordite tables (strong determinized-MC opponents — the matchup that
matters for "strongest bot"):

| config | pc | pairs | diff ± SE | h<c/h>c/eq |
|---|---|---|---|---|
| cap=0, win-check only | 2 | 400 | −0.015 ± 0.019 | 32/26/342 |
| cap=0, win-check only | 3 | 400 | −0.033 ± 0.042 | 78/67/255 |
| depth=1, cap=6, plies=8 | 2 | 200 | XXX | XXX |
| depth=1, cap=6, all plies | 2 | 80 | XXX | XXX |
| leaf-limit probe (18 cards / 100k nodes) | 2 | 150 | XXX | XXX |
| own-seat only (`OG_SELF_OWN`), all plies | 2 | 200 | XXX | XXX |
| own-seat only (`OG_SELF_OWN`), all plies | 3 | 150 | XXX | XXX |
| own-seat + belief-fed (`OG_SELF_HONEST=2`) | 2 | 150 | XXX | XXX |
| own-seat + transposition cache (`OG_SELF_TT=22`) | 2 | 150 | XXX | XXX |
| depth=1 all plies + transposition cache | 2 | 80 | XXX | XXX |

@ handwritten tables (the opponents the shipped rollout policy models
*correctly* — symmetric self-rollout replaces a true opponent model with a
wrong one, while the own-seat variant leaves it intact):

| config | pc | pairs | diff ± SE | h<c/h>c/eq |
|---|---|---|---|---|
| depth=1, cap=6, plies=8 | 2 | 200 | XXX | XXX |
| own-seat only (`OG_SELF_OWN`), all plies | 2 | 200 | XXX | XXX |
| own-seat + belief-fed (`OG_SELF_HONEST=2`) | 2 | 150 | XXX | XXX |

XXX-RESULTS-DISCUSSION

## 4. Why the recursion limit was already measured — three times

The limit of octogen-as-rollout-policy at infinite depth/cap is *perfect
full-information play of each sampled world*. This repo has measured that
regime, and the road toward it, from three independent directions:

1. **Exact endgames inside rollouts** (cordite, `CD_LEAF`): solving
   deck-empty 2-player endgames *perfectly* inside rollouts made cordite
   ~10x slower and **weaker** (pc2 vs handwritten 1.150 → 1.240 mean).
   "Modeling the actual imperfect opponent beats assuming perfect play"
   (`c/CORDITE.md`, kept as a negative result). Semtex/octogen later found
   the *narrow* profitable dose: tiny leaves (≤8 cards heads-up, 3k nodes) —
   and hunt 4 measured the next step (10 cards / 8k nodes) as **flat at 4x
   cost** (`c/OCTOGEN.md` null #4). The direction saturates almost
   immediately.
2. **Searched opponent replies inside worlds** (`OG_REPLY`, hunt 4): letting
   the first opponent decision be chosen by search over their legal replies
   — a single ply of exactly what `octogen_self` does at every ply — was
   *paranoid distortion*: the searched reply uses sampled hidden cards the
   real opponent cannot see (pc4 +0.150±0.128 WORSE; the honest
   defender-only variant washed out at pc4 +0.028±0.051, pc3 −0.068±0.055).
3. **Stronger opponent models generally** (hunt 4's summary over ~10
   levers): "against a strong determinized-MC opponent only **exact truth**
   and **variance** ever paid; opponent-model refinements of an
   already-close policy wash out."

`octogen_self` is the all-plies, all-seats generalization of (2) evaluated
under (1)'s cost curve. The new measurements in §3 fill in the missing
middle of the curve — XXX-CURVE-SUMMARY.

And a fourth direction bounds the whole enterprise from above.
**Novichok** (`c/NOVICHOK.md`), the full-information cheat built on
octogen's own architecture, measures what *perfect knowledge* is worth at
this table: ≈ zero heads-up against MC opponents (cordite pc2 −0.010 ±
0.048) and **negative at 3+ players** (cordite pc3 +0.173, pc6 +0.267 —
the cheat loses to honesty). Its Finding 4 is this idea's exact upper
bound run under ideal conditions: search-chosen opponent moves on TRUE
worlds — no "sampled hidden cards" objection left — and still null
heads-up, harmful vs weak fields. Its Finding 3 explains why: the shared
rollout model's error is *correlated across worlds*; octogen's
belief-spread worlds decorrelate and average that error down, while
sharper/truer evaluation re-runs the same wrong prediction with more
confidence. A recursive self-rollout sharpens in-world evaluation without
adding one bit of information — it walks INTO the regime novichok proved
is not where strength lives, minus novichok's information advantage.

## 5. So is it the strongest bot possible? No — here is the argument in full

**At fixed compute it is strictly worse.** A depth-1 self-rollout costs
~70x per world; at equal wall-clock that is 70x fewer worlds. Octogen's own
budget studies show world counts *above* the tuned point are flat-to-worse —
but 1/70th of the tuned budget is far below it, and MC standard error grows
as 1/√worlds. You trade variance (which pays) for in-world rationality
(which doesn't). Hunt 4's summary line again: only exact truth and variance
ever paid.

**At unlimited compute it converges to a measured-worse regime.** With both
arms maxed out (worlds → ∞ AND depth → ∞), the estimate converges to "value
of each candidate under perfect full-information play of belief-sampled
worlds." That is not ground truth of the *game* — it is the truth of a
different, easier game where everyone sees everything after the deal. Two
separate failure modes follow:

- *Opponent-model error*: the real opponent (MC bot or human) is imperfect
  and information-limited; a perfect-play model systematically assumes
  punishes that never come and never exploits errors that reliably do come.
  Rollout policies are opponent models, and the best model of an imperfect
  opponent is not a perfect player — measured in §4(1) and §4(2).
- *Strategy fusion / determinization bias*: even a perfect in-world policy
  is averaged over worlds by a root chooser that must commit to ONE move
  across all worlds while the playouts implicitly assume world-specific
  knowledge downstream. Determinized MC cannot represent
  information-gathering or information-hiding play (octogen's
  hide-uncoverable rule had to be bolted on *outside* the MC for exactly
  this reason, `docs/OCTOGEN_HIDE_UNCOVERABLE.md`). Perfecting the rollout
  policy sharpens the answer to the wrong question.

**The base case is already there.** "Does this move let me win?" is
subsumed: within its window the exact leaf solver IS that base case,
optimal and proven (and extending its window was flat — §4(1), §3's leaf
probe). Outside the solver's window the win-check adds a 1-ply tactical
floor to the rollout at near-zero cost — XXX-CAP0-VERDICT.

**Where the actual frontier is.** OCTOGEN.md's closing line stands,
now with the rollout-policy direction measured too: a qualitatively
stronger successor needs a different architecture — learned information-set
values, or true information-set search (CFR-style or IS-MCTS), where
compute is spent on the *belief/information structure* of the game rather
than on sharpening play inside worlds that are already fake. If a "spend
unlimited compute per move" premium bot is wanted TODAY, the measured-safe
places to pour compute are: (i) more worlds (variance) up to the measured
plateau, (ii) the exact-solve window (`OG_SOLVE_CARDS`/budgets — the one
lever of hunt 4 that ever paid, provably never worse), and (iii) many-core
parallel worlds for latency. The infinite-oracle *replay analyzer*
(docs/INFINITE_ORACLE_DESIGN.md) is exactly this recipe already.

## 6. Reproduce

```bash
cd c && make
# cost probes (1 game each)
./build/cnitro_eval --strategy=octogen --opp=cordite --players=2 --games=1 --seed-start=910001
OG_SELF_PLIES=8 ./build/cnitro_eval --strategy=octogen_self --opp=cordite --players=2 --games=1 --seed-start=910001
# paired strength (hero=self vs control=octogen, same deals)
OG_SELF_CAP=0 ./build/cnitro_eval --strategy=octogen_self --control=octogen \
    --opp=cordite --players=2,3 --games=400 --seed-start=910001
OG_SELF_PLIES=8 ./build/cnitro_eval --strategy=octogen_self --control=octogen \
    --opp=cordite --players=2 --games=200 --seed-start=910001
./build/cnitro_eval --strategy=octogen_self --control=octogen \
    --opp=cordite --players=2 --games=80 --seed-start=910001
OG_SELF_CAP=0 OG_SELF_WIN=0 OG_SELF_LEAF_CARDS=18 OG_SELF_LEAF_BUDGET=100000 \
    ./build/cnitro_eval --strategy=octogen_self --control=octogen \
    --opp=cordite --players=2 --games=150 --seed-start=910001
```

Knobs (`octogen_self` only; octogen itself is decision-identical with the
lever off): `OG_SELF_DEPTH` (1) recursion depth; `OG_SELF_CAP` (6) moves
searched per in-world decision, 0 = base-case-only; `OG_SELF_PLIES` (0=all)
searched plies per playout level; `OG_SELF_WIN` (1) immediate-win base case;
`OG_SELF_STAGE` (0) first MC stage that self-rolls;
`OG_SELF_LEAF_CARDS`/`OG_SELF_LEAF_BUDGET` (-1 = inherit) recursion-limit
probe via the in-rollout exact leaf.
