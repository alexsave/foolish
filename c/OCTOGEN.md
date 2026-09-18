# Octogen — semtex's successor

Octogen (HMX, one rung above semtex's RDX) is the hunt-4 bot: the successor
that is provably never worse than semtex and strictly better where exact
truth still exists. Registered as `octogen`/`og` (C; `octogen_oracle`/`ogo`
is the 6x-worlds audit variant). All numbers are paired same-deal deltas vs
a **semtex** control (`--control=semtex`).

## The one lever that survived

**Extended exact root-solve window**: the heads-up deck-empty solver
engages at <= 28 total cards with 400k/250k node budgets (semtex: 24,
150k/100k). At pc2 deck-empty the opponent-hand deduction is exact, so a
resolved claim is a genuine certainty; the extension only adds proven-win
taking and proven-loss avoidance in a region where semtex still samples.
Measured on the clean harness: **never worse in any cell**; strictly
better in ~0.25-0.5% of deals — @ cordite tables pc2 1/0/399, pc3 2/0/398;
@ semtex tables pc2 0/0/200. Cost: ~1.8x pc2 decision wall-clock, which is
why octogen stays **C-only** — on Supabase that CPU price for that
frequency fails the compute bar, so production keeps `semtex` (base cost)
and `semtex_max` (full measured world budgets). Since octogen is
decision-identical to semtex outside the window, semtex's entire validated
dominance matrix (SEMTEX.md) transfers verbatim, plus the strict extra wins.

## The biggest discovery of hunt 4: a measurement bug

Mid-hunt, the extended window appeared to be FALSIFIED at semtex tables
(0 better / 5 worse / 195), and even semtex-identical defaults failed the
identity check (0/4-5 worse). Root cause — in the harness, not the bot:
leaf solving persists the sim solver TT across the worlds of a decision
(sound), but it also persisted **across games**, coupling the two games of
a --control pair. Game B inherited game A's TT warmth; budget-dependent
solves resolved differently; bit-identical strategies diverged in 2.5% of
pc2 pairs, always against the hero (who plays the colder-TT game). Proven
by a leaf-off identity test (0/0/200) and fixed by resetting the TT per
game in every play_one variant. Consequences: the falsification reversed
on the clean harness, and every earlier pc2 paired result had carried a
small anti-hero bias — i.e. semtex's shipped pc2 edges were understated.

## The nulls (measured, so nobody retreads them)

The hunt targeted the model-limited mirror losses (deals that survive even
a 6x-worlds oracle) with opponent-model levers. All null or worse vs semtex:

1. **Reply tournament, any opponent** (`OG_REPLY`): first opponent reply in
   final-duel worlds chosen by search over their full legal reply set.
   pc4 +0.150+-0.128 — in-world best response uses sampled hidden cards the
   real opponent cannot see. Paranoid distortion (the CD_LEAF lesson).
2. **Reply tournament, defender-only**: restricted to the reply the real
   defender could actually compute (own hand + visible attack). pc4
   +0.028+-0.051, pc3 -0.068+-0.055 — searching a reply under a handwritten
   continuation is not a better model of an MC defender than the policy
   itself. (Real bug found: PICKUP/GOOD rank last cheap-first and must
   never be pruned from a searched reply set.)
3. **MC-defender rollout model** (`OG_MCDEF`, `CD_POL_MCDEF`): mc_tell'd
   seats pick up rather than burn a trump while the deck lives (50%
   mixed). pc3 +0.055+-0.039 — over-models how often MC defenders actually
   pick up.
4. **Deeper heads-up rollout leaves** (10 cards / 8k nodes): flat at 4x the
   pc2 cost.

With the semtex hunts included, the pattern over ~10 levers held **at the
doses tried here** (one searched opponent reply, late stages only, mixed
tables): only exact truth and variance ever paid, and opponent-model
refinements of an already-close policy washed out. **Hunt 5 overturned the
generalization, not the measurements**: at full dose (every in-world seat,
every ply) against a strong determinized-MC opponent, a searching rollout
policy is worth -0.237+-0.060 at pc2. These nulls were dose- and
table-limited, so do not cite them as "opponent models never pay"; see
below. A qualitatively stronger successor still needs a different
architecture (learned information-set values or true information-set
search).

## Hunt 5: octogen as its own rollout policy

Use octogen itself as the rollout policy inside its own sampled worlds,
with a cheap "does this move let me win?" base case, on effectively
unlimited compute. The strongest bot possible? **The universal form of
that claim is disproven, and the gated form is measured, parked, and
unbuilt.** None of it is on main. The implementation (`octogen_self`/`ogs`,
`cd_sim_playout_self`, the `OG_SELF_*` knobs) and the long-form write-up
with every paired cell, the cost table, the decomposition and the
reproduction commands live on branch
`claude/octogen-rollout-policy-test-lip40z`
(`518a6bd8391892cef4decd82b2d9e06d9e2a253b`,
`docs/OCTOGEN_SELF_ROLLOUT.md`), held on purpose; read it there before
re-deriving anything below.

**The headline: the sign is a property of the OPPONENT, not of the
policy.** Paired same-deal vs an octogen control at pc2, the full symmetric
self-rollout (every in-world seat, every ply, ~70x decision cost) is
**-0.237+-0.060 vs cordite** (23/4/53, dose-responsive: -0.080+-0.045 at 8
searched plies) and **+0.105+-0.032 WORSE vs handwritten** (11/32/157),
with a saturated null vs random. A rollout policy is an opponent model, so
making it "smarter" helps exactly when the real opponent is search-like and
hurts when it is not: against handwritten the stock policy IS that
opponent's decision function, zero model error by construction, and
replacing it buys only phantom punishment and phantom competence. That is
the paranoid-distortion prior confirmed, and it is not a world-sampling
artifact: `NOVICHOK.md` finding 4 measures the same sign flip on TRUE
worlds. There is no table-independent strongest rollout policy.

**The decomposition is what makes that precise.** Own-seat-only search
(honest opponent model kept, only our own in-world plies searched) is
-0.075+-0.045 vs cordite and a clean null vs handwritten
(-0.020+-0.022, 12/8/180). So the ENTIRE handwritten harm came from the
opponent-model half, and about two thirds of the cordite win did too; the
rest is that the stock rollout models our own future self as handwritten
and therefore undervalues lines needing competent follow-up (the trump-keep
tax is a hand-written patch for one instance of exactly that bias).

**Nulls measured on the way, so nobody re-runs these axes:**

- **Belief-feeding the future self** (choose on the carried-forward root
  belief over M re-determinizations instead of on this world's truth) is
  strength-neutral at 2x cost: -0.073 vs -0.075 at cordite, -0.033 vs
  -0.020 at handwritten, same seeds. The strategy-fusion leak it closes
  was not biting at depth 1.
- **The exact-leaf axis is dead.** Extending the in-rollout exact leaf to
  18 cards / 100k nodes: +0.000+-0.058 at 33x the leaf budget. With null #4
  above, endgame truth saturates almost immediately, and this also pins
  the cordite win as a MID-GAME modeling effect.
- **The win-check base case alone** (no tournament) is a cheap directional
  null: -0.015+-0.019 pc2, -0.033+-0.042 pc3, 400 pairs each. Inside the
  solver's window the exact leaf already IS that base case.
- **A transposition cache over searched in-world decisions** buys wall
  clock, not strength: ~30% hit rate, 1.7-2.0x faster at depth 1/2,
  strength preserved on the same seeds, at 128 MB/thread against a 4.3 MB
  baseline. Entries must die at every root decision, or it recreates the
  cross-game TT coupling that corrupted hunt 4's first harness. Memory is
  otherwise a non-issue at any depth; wall clock is the whole bill and it
  compounds as ~(cap x plies)^depth (full depth 2 is ~5,000x).

**The parked option: the `mc_tell` gate.** Engaging the searching rollout
only against seats that have behaviorally proven strategic play (a
strategic pickup while holding a full cover, read from the public log like
everything else the belief reads, no table knowledge) measured **never
worse in any cell**: decision-identical to stock octogen on 200/200
handwritten and 150/150 random deals with zero false fires, and
-0.113+-0.033 vs cordite (22/5/123). That is exactly the
never-worse/strictly-better bar octogen itself cleared over semtex. It is
better read as a human-experience lever than a strength lever: the bot
spends real thought only on opponents who have demonstrably behaved like
thinkers, so a strategic human trips it the same way cordite does and a
casual one faces plain octogen for free.

It is unbuilt pending a product decision, not because it failed. What would
have to be decided: it costs ~70x per decision on the subset where it
fires, which is the same compute bar that already keeps octogen C-only; it
needs a `STRAT` id (24 is next free, and cl20 wants it too); and the
standing position is that foolish is a people-to-people game where bots are
secondary, so a small proven gain does not automatically earn its code.

**Do not propose literal recursion.** Octogen calling
`octogen_strategy_choose` at every rollout ply is infeasible (one
bot-family deliberation per decision is an invariant: the world scratch,
solver TT and solve scratch are single shared slots, `cordite_sim.h`), and
it is degenerate anyway, because
inside a sampled world there is no hidden information left, so an inner
octogen's belief collapses to the identity and its deliberation collapses
to full-information search. Recursive octogen inside a determinized world
IS full-information search, and `NOVICHOK.md` already measures what that
regime is worth: about zero heads-up and negative at 3+ players.

## Knobs (`OG_*`, octogen only)

- `OG_SOLVE_CARDS` (28) / `OG_BB_WIN` (400k) / `OG_BB_AVOID` (250k) — the
  enabled lever. `OG_AVOID_CARDS` (24) gates the loss-avoidance pass
  separately (kept from the artifact investigation; at 24 the avoidance
  behavior matches semtex, the extension is win-hunt only beyond it —
  measured equivalent on the clean harness, kept conservative).
- `OG_REPLY` (0) / `OG_REPLY_CAP` (6) / `OG_REPLY_STAGE` (2), `OG_MCDEF`
  (0) — the measured-null research levers.
- Everything else is inherited from semtex under the `OG_` prefix.
