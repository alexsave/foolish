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

With the semtex hunts included, the pattern over ~10 levers stands: against
a strong determinized-MC opponent only **exact truth** and **variance**
ever paid; opponent-model refinements of an already-close policy wash out.
A qualitatively stronger successor needs a different architecture (learned
information-set values or true information-set search).

## Hunt 5: octogen as its own rollout policy (the "infinite oracle" recursion)

Asked and answered (docs/OCTOGEN_SELF_ROLLOUT.md has the full argument,
costs, and reproduction): replace the handwritten rollout policy with
octogen itself, recursively, base case "does this move let me win?" — is
that the strongest possible bot on unlimited compute? **No.** Implemented as
`octogen_self`/`ogs` (`cd_sim_playout_self`: every in-world seat runs a
nested cheap-first tournament over its full legal set, evaluated by
playouts one recursion level down; base cases = immediate-win check, the
exact leaf, handwritten at depth 0). Inside a determinized world a
recursive octogen has nothing left to sample, so the recursion limit is
perfect FULL-INFORMATION play of the world — the opponent-model regime
already measured harmful by CD_LEAF (10x slower AND weaker) and OG_REPLY
(paranoid distortion), and reachable far cheaper via the solver than via
nesting. Measured here: memory flat at every depth (~4.3 MB RSS — the
recursion is O(depth) SimStates); wall-clock ~(cap x plies)^depth (full
depth-1 ~70x, projected full depth-2 ~5,000x); strength XXX-HUNT5-RESULT.
Knobs: `OG_SELF_DEPTH/CAP/PLIES/WIN/STAGE`, `OG_SELF_LEAF_CARDS/BUDGET`
(engage only through the `octogen_self` strategy id, so paired runs hold
plain octogen in-process).

## Knobs (`OG_*`, octogen only)

- `OG_SOLVE_CARDS` (28) / `OG_BB_WIN` (400k) / `OG_BB_AVOID` (250k) — the
  enabled lever. `OG_AVOID_CARDS` (24) gates the loss-avoidance pass
  separately (kept from the artifact investigation; at 24 the avoidance
  behavior matches semtex, the extension is win-hunt only beyond it —
  measured equivalent on the clean harness, kept conservative).
- `OG_REPLY` (0) / `OG_REPLY_CAP` (6) / `OG_REPLY_STAGE` (2), `OG_MCDEF`
  (0) — the measured-null research levers.
- Everything else is inherited from semtex under the `OG_` prefix.
