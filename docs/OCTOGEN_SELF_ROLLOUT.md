# Octogen as its own rollout policy — the "infinite oracle" recursion, measured

**Question asked**: use octogen itself as the rollout policy inside octogen's
sampled worlds, with a cheap base case ("does this move let me win?"), on
essentially limitless compute — is that the strongest bot possible?

**Answer: not universally — the sign depends on the opponent — but the hunt
produced a real successor candidate.** The lever is implemented
(`octogen_self`/`ogs`, `OG_SELF_*` knobs), costs are measured (memory: flat;
wall-clock: ~(cap x plies)^depth), and the strength deltas are measured on
the paired same-deal harness. The headline cells, which partially OVERTURN
this repo's hunt-4 priors:

- vs **cordite** (strong determinized-MC) pc2: the full symmetric
  self-rollout is **−0.237 ± 0.060** mean finish (86.2% vs 62.5% wins,
  23/4/53) — a ~4σ strength WIN at ~70x decision cost, dose-responsive
  (plies=8 gives −0.080 ± 0.045).
- vs **handwritten** (the opponent the stock rollout models *exactly*) pc2:
  the same lever is **+0.105 ± 0.032 WORSE** — the paranoid-distortion
  prior, confirmed.
- vs **random** pc2: saturated null (−0.007 ± 0.007; both bots ~always win).

The rollout policy is an opponent model; making it "smarter" helps exactly
when the real opponent is search-like and hurts when it isn't. Under the
constraint that a bot may never *know* opponent types, the deployable
result is **`OG_SELF_TELL`** — engage the searching rollout only against
seats that have *behaviorally proven* strategic play (the belief's
`mc_tell`, read from the public log like everything else): measured
**never worse in any cell** (handwritten: 200/200 deals decision-identical
to octogen; random: 150/150 identical) and **−0.113 ± 0.033 better vs
cordite** — the same never-worse/strictly-better acceptance bar octogen
itself cleared over semtex. The recursion's far limit (perfect
full-information in-world play) remains unreached and theoretically
suspect (§4-5), and the true architectural frontier is still
information-set search — but as a practical matter, "octogen as rollout
policy, gated by observed opponent behavior" is the strongest legitimate
bot this repo has measured.

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
  anti-hero coupling artifact that corrupted hunt 4's first harness.
  Measured (1-game single-thread probes + paired A/Bs): hit rates 30.9%
  (d1full) / 33.6% (depth-2) / 29.4% (own-seat) of all searched in-world
  decisions are within-decision transpositions; wall-clock 76.3s → 45.1s
  (1.69x) at depth 1 and 56.2s → 27.9s (2.01x) at depth 2 — the DAG
  collapse compounds with depth exactly as predicted; RSS 4.4 MB → 135 MB
  (the 128 MB table is the first configuration in this hunt where memory
  buys anything). Strength A/B on the same seeds: preserved within noise
  in both configs (own −0.067 vs −0.075; symmetric full −0.188 vs
  −0.237). Note the asymmetry with
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
| depth=1, cap=6, plies=8 | 2 | 200 | **−0.080 ± 0.045** | 49/33/118 |
| depth=1, cap=6, all plies | 2 | 80 | **−0.237 ± 0.060** | 23/4/53 |
| leaf-limit probe (18 cards / 100k nodes) | 2 | 150 | +0.000 ± 0.058 | 38/38/74 |
| own-seat only (`OG_SELF_OWN`), all plies | 2 | 200 | **−0.075 ± 0.045** | 48/33/119 |
| own-seat only (`OG_SELF_OWN`), all plies | 3 | 150 | −0.087 ± 0.091 | 52/43/55 |
| own-seat + belief-fed (`OG_SELF_HONEST=2`) | 2 | 150 | −0.073 ± 0.056 | 41/30/79 |
| own-seat + transposition cache (`OG_SELF_TT=22`) | 2 | 150 | −0.067 ± 0.052 | 36/26/88 |
| depth=1 all plies + transposition cache | 2 | 80 | **−0.188 ± 0.069** | 24/9/47 |
| **TELL-gated** (`OG_SELF_TELL=1`, symmetric) | 2 | 150 | **−0.113 ± 0.033** | 22/5/123 |

@ handwritten tables (the opponents the shipped rollout policy models
*correctly* — symmetric self-rollout replaces a true opponent model with a
wrong one, while the own-seat variant leaves it intact):

| config | pc | pairs | diff ± SE | h<c/h>c/eq |
|---|---|---|---|---|
| depth=1, cap=6, plies=8 | 2 | 200 | **+0.105 ± 0.032 (WORSE)** | 11/32/157 |
| own-seat only (`OG_SELF_OWN`), all plies | 2 | 200 | −0.020 ± 0.022 | 12/8/180 |
| own-seat + belief-fed (`OG_SELF_HONEST=2`) | 2 | 150 | −0.033 ± 0.026 | 10/5/135 |
| **TELL-gated** (`OG_SELF_TELL=1`, symmetric) | 2 | 200 | **+0.000 ± 0.000** | 0/0/200 |

@ random tables (worst case for any opponent model; heads-up is a
saturated ceiling cell — both bots ~always win):

| config | pc | pairs | diff ± SE | h<c/h>c/eq |
|---|---|---|---|---|
| depth=1, cap=6, all plies | 2 | 150 | −0.007 ± 0.007 | 1/0/149 |
| **TELL-gated** (`OG_SELF_TELL=1`, symmetric) | 2 | 150 | +0.000 ± 0.000 | 0/0/150 |

Reading of the table, in order of importance:

1. **The symmetric self-rollout's sign is a function of the opponent.**
   A large, dose-responsive win against cordite (−0.080 at 8 searched
   plies → −0.237 at all plies); a solid loss against handwritten; nothing
   against random. There is no universally-strongest rollout policy.
2. **The TELL gate converts the tradeoff into strict dominance.** With the
   searching rollout engaged only on behavioral evidence (`mc_tell`), the
   bot is *decision-identical* to octogen on every handwritten and random
   deal (350/350 exact equals — the gate provably never fires there,
   including zero false fires vs random, whose seats the belief profiles as
   loose rather than strategic) and keeps a 3.4σ win vs cordite (−0.113;
   about half the ungated gain, since the tell needs in-game evidence to
   accumulate while early decisions run stock). Never worse in any cell —
   the same acceptance bar octogen cleared over semtex.
3. **The decomposition (§6): most of the effect is the opponent-model
   half.** Own-seat-only search keeps the true opponent model and captures
   −0.075 of the −0.237 vs cordite; it is a clean null vs handwritten
   (−0.020 ± 0.022) — i.e. the entire handwritten harm came from the
   opponent side of the policy swap, and own-seat search is safe
   everywhere (mildly positive at pc3 too, within noise).
4. **Belief-feeding the future self (`OG_SELF_HONEST`) is strength-neutral**
   on both tables (−0.073 vs −0.075 clairvoyant @ cordite; −0.033 vs
   −0.020 @ handwritten, same seeds) at 2x cost: the clairvoyant argmax's
   strategy-fusion leak measurably wasn't biting at depth 1. The
   clairvoyant variant is the keeper; honest stays as the insurance A/B.
5. **The transposition cache preserves strength** (own: −0.067 vs −0.075;
   symmetric full: −0.188 vs −0.237, same seeds, all within noise) while
   cutting wall-clock 1.7-2x — see §2/§4.
6. **The endgame-exactness axis stays dead** (leaf18: +0.000 ± 0.058 at
   33x the leaf budget) — the cordite win is a MID-GAME modeling effect,
   and the win-check base case alone (cap=0) is a cheap null.

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
middle of the curve — and they **revise the priors' scope**. What survived:
paranoid distortion is real and measured again (+0.105 vs handwritten —
against opponents weaker than the search model, searching them is harmful).
What did NOT survive: "only exact truth and variance ever paid." That
summary was drawn from homeopathic doses (one searched defender reply, late
stages only, mixed tables) — at full dose against a strong determinized-MC
opponent, in-world search is worth −0.237, the largest honest paired win
this repo has recorded against cordite tables. Hunt 4's nulls were
dose-limited and table-averaged, not wrong, but their generalization was.

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

## 5. So is it the strongest bot possible? Not universally — but gated, it is the strongest bot measured here

**The sign is the opponent's property, not the policy's.** Measured: a
~4σ win against the strongest honest opponent (cordite), a ~3σ loss against
the weak one whose true decision function the stock rollout already IS, a
saturated null against random. "Octogen as rollout policy" is therefore
not *the* strongest bot — there is no table-independent strongest rollout
policy — but it IS the strongest measured configuration against strong
opposition, and the behavioral gate (`OG_SELF_TELL`) packages that into a
bot that is never worse anywhere (350/350 decision-identical deals on
weak/random tables) and −0.113 ± 0.033 better against cordite. That
satisfies the repo's own successor bar (never worse, strictly better
somewhere) at ~5-70x decision cost when engaged.

**On compute.** These wins were measured at unequal compute (the hero
spends 20-70x per decision; both arms keep their full world budgets) — the
right frame for the "essentially limitless compute" premise. A
fixed-compute version (fewer worlds to pay for search) was not measured
and would face the variance argument: MC error grows as 1/√worlds and the
world budget is already at its tuned plateau. Depth is monotone so far vs
cordite (d0 −0.000 → p8 −0.080 → full −0.237), and full depth-2
(~5,000x, or less with the TT's compounding 1.7-2x savings) is the next
untested point — feasible as a batch job or on a big dedicated box.

**The far limit remains suspect.** With depth → ∞ the estimate converges
to "value under perfect full-information play of belief-sampled worlds" —
the truth of a different, easier game where everyone sees everything after
the deal. Two failure modes still bound that end of the curve:

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
probe at 18 cards / 100k nodes: +0.000 ± 0.058). Outside the solver's
window the win-check adds a 1-ply tactical floor at near-zero cost —
measured a directionally-positive null (pc2 −0.015 ± 0.019, pc3
−0.033 ± 0.042 over 400 pairs each): harmless to keep as a knob, not a
strength lever on its own.

**Where compute should go, by product:**

- **Production/premium bot ("strongest legit bot today")**: octogen +
  `OG_SELF_TELL=1` (+`OG_SELF_TT` when memory allows). Never worse
  anywhere, −0.113 vs cordite-class opposition, engages (and pays its
  5-70x) only after behavioral evidence. Against humans the gate reads
  behavior, not identity — a strategic human trips `mc_tell` the same way
  cordite does, and a casual one leaves the bot exactly octogen. Cheaper
  intermediate doses (OG_SELF_PLIES, OG_SELF_STAGE=2 duel-only) remain
  untuned headroom, as does always-on own-seat search (safe everywhere:
  −0.075/−0.020/−0.087, at ~35x).
- **The infinite-oracle replay analyzer**: the unconditional home for this
  hunt. Unbounded compute is its premise, memory is cheap there (the
  self-TT's 30% hit rates and 1.7-2x compounding speedup fit the browser's
  regime; scale table bits to the worker budget), and its subject is
  usually strong play. Ship the searching rollout as the analyzer's
  deliberation upgrade, with the §3 caveat surfaced honestly: a verdict is
  "vs a strong opponent" — moves that are only mistakes against strong
  opponents are exactly what a coach should label, not hide.
- **The architectural frontier**: OCTOGEN.md's closing line still stands
  for the ceiling above all of this — learned information-set values or
  true information-set search, where compute buys belief-structure rather
  than sharper play inside sampled worlds. Novichok's ceiling numbers
  (perfect information ≈ +0 heads-up, negative multiplayer vs MC tables)
  bound how much any within-architecture lever can still find; hunt 5
  found most of what that bound allows heads-up.

## 6. Why the sign flips: the handwritten harm and the cordite win, explained

The natural objection: "it would make sense that it hurts if we swapped the
OPPONENT's rollout policy — but we changed OUR OWN. What gives?"

**First, the factual answer: in the symmetric configs we changed both.**
`d1p8`/`d1full` — the cells that measured +0.105 vs handwritten and −0.237
vs cordite — replace the in-world policy for EVERY seat of every sampled
world: our future self AND the in-world stand-in for the opponent. A
rollout policy in a determinized MC bot plays two roles at once: it is the
model of our own continuation on our plies, and the model of the
opponent's responses on theirs. The symmetric lever moves both, so vs
handwritten we really did swap the opponent's model away from the one that
is *literally the true generative model* of that opponent (the stock
rollout IS handwritten's decision function — zero model error by
construction). The `OG_SELF_OWN` configs exist precisely to split this:
they search only our own plies and leave the opponent model untouched.
Decomposition results — both predictions (registered before the numbers
landed) confirmed:

- own-only @ handwritten: **−0.020 ± 0.022** (12/8/180) — a clean null.
  The ENTIRE +0.105 symmetric harm was the opponent-model half; searching
  only our own plies is safe even against the opponent the stock rollout
  models perfectly. The belief-fed variant matches (−0.033 ± 0.026),
  confirming no clairvoyance residual needed shaving.
- own-only @ cordite: **−0.075 ± 0.045** (48/33/119) vs symmetric
  −0.237 ± 0.060 on the same seeds — own-seat search captures roughly a
  third of the cordite win; the other two-thirds is the better opponent
  model. (Belief-fed: −0.073 ± 0.056 — identical; pc3: −0.087 ± 0.091,
  same direction inside noise.)

**Why the wrong opponent model costs real points vs handwritten** (two
channels):

1. *Phantom punishment (paranoia).* The searching in-world opponent finds
   refutations handwritten will never play — handwritten covers cheapest
   mechanically, never strategically picks up, attacks lowest-first.
   Candidates that exploit those mechanical habits get devalued by the
   search and we stop playing them: forfeited exploitation, the classic
   paranoid-search failure. NOVICHOK.md finding 4 measured the identical
   sign flip with TRUE-hand worlds (searched replies: −0.175 → −0.085 vs
   handwritten pc3) — so this is not about world-sampling error at all;
   it is about answering "what if they punish me?" when they provably
   won't.
2. *Phantom competence.* Our plans are graded against an in-world defender
   that saves trumps and takes strategic pickups; handwritten never does.
   Move ranking optimizes against a fiction strictly farther from the true
   opponent than the stock model (whose distance is zero).

And a saturation asymmetry amplifies the asymmetry of outcomes: against
handwritten the control already scores 1.060 mean / 94% wins — there is
almost no headroom, so any injected bias converts directly into losses;
against cordite the control sits at 1.320-1.375 with plenty of decidable
deals left.

**Why the big win vs cordite** (the same two channels, signs reversed):

1. *The opponent model got closer, not farther.* Cordite actually does
   what the searching rollout does: weighs alternatives, protects trumps
   with strategic pickups, refuses doomed covers, punishes overextension.
   The entire `mc_tell`/`OG_MCDEF` machinery exists because handwritten
   "NEVER picks up while holding a full cover, but MC bots and thinking
   humans do it constantly" — i.e., the stock opponent model is known-wrong
   against MC opponents, and the 1-ply-search model is much closer. Value
   estimates against cordite become less biased; move ranking improves.
2. *Our own continuation model got truer too.* The real octogen at future
   decisions plays strongly; the stock rollout models our future self as
   handwritten, so lines whose payoff needs competent follow-up (keeping a
   trump for a later squeeze, covering now to set up a pass) are
   systematically undervalued. The in-repo smoking gun: the trump-keep tax
   exists because "the weak handwritten rollout policy undervalues keeping
   trumps... (measured: 52.5% -> 36.7% under a stronger rollout)" — a
   hand-written patch for ONE instance of exactly the bias the self-rollout
   removes wholesale. Against a strong opponent those follow-up-sensitive
   lines are where games are decided.

The leaf18 null pins the location: none of the cordite gain comes from
endgame truth (already exact and saturated) — it is mid-game modeling,
where hands are big, choices are discretionary, and the two policies
disagree most.

## 7. Reproduce

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
# decomposition: own-seat only / belief-fed / cached / TELL-gated
OG_SELF_OWN=1 ./build/cnitro_eval --strategy=octogen_self --control=octogen \
    --opp=cordite --players=2 --games=200 --seed-start=910001
OG_SELF_OWN=1 OG_SELF_HONEST=2 ./build/cnitro_eval --strategy=octogen_self \
    --control=octogen --opp=cordite --players=2 --games=150 --seed-start=910001
OG_SELF_TT=22 ./build/cnitro_eval --strategy=octogen_self --control=octogen \
    --opp=cordite --players=2 --games=80 --seed-start=910001
OG_SELF_TELL=1 ./build/cnitro_eval --strategy=octogen_self --control=octogen \
    --opp=cordite --players=2 --games=150 --seed-start=910001
# NOTE: build with make OMP=1 all for the parallel harness; long paired
# sweeps are best run as short --games chunks with --dump for restart-safe
# resume (count dump lines, continue at seed-start + lines).
```

Knobs (`octogen_self` only; octogen itself is decision-identical with the
lever off, verified over 80 games): `OG_SELF_DEPTH` (1) recursion depth;
`OG_SELF_CAP` (6) moves searched per in-world decision, 0 =
base-case-only; `OG_SELF_PLIES` (0=all) searched plies per playout level;
`OG_SELF_WIN` (1) immediate-win base case; `OG_SELF_STAGE` (0) first MC
stage that self-rolls; `OG_SELF_OWN` (0) search only our own seat's
in-world decisions; `OG_SELF_HONEST` (0, needs OWN) belief-fed
future-self, M re-determinizations per decision; `OG_SELF_TT` (0) bits of
the searched-decision transposition cache, `OG_SELF_TT_STATS` (0) hit-rate
report at exit; `OG_SELF_TELL` (0) engage only on proven-mc_tell
opponents; `OG_SELF_LEAF_CARDS`/`OG_SELF_LEAF_BUDGET` (-1 = inherit)
recursion-limit probe via the in-rollout exact leaf.
