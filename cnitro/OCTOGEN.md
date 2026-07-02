# Octogen — semtex's successor, and the ceiling finding

Octogen (HMX, one rung above semtex's RDX) is the hunt-4 bot: the attempt
to build a successor that beats semtex the way semtex beats cordite. The
honest headline result is negative and important: **semtex sits at the
practical ceiling of this determinized-MC architecture for strong-vs-strong
play.** Five octogen variants attacked the remaining loss surface from
different angles; every opponent-modeling variant measured flat or worse,
and the only lever that survived is a strictly-dominant but microscopic
truth extension. All numbers are paired same-deal deltas vs a **semtex**
control (the harness's `--control=semtex`).

## What was tried (hunt 4)

The target: the model-limited losses — mirror deals that survive even a
6x-worlds oracle (22 of 58 pc4 mirror losses). The classic determinized-MC
blind spot is that rollouts assume opponents play the fixed policy, never
the refutation.

1. **Reply tournament, any opponent** (`OG_REPLY=1`, `OG_REPLY_STAGE`,
   `OG_REPLY_CAP`): in final-duel worlds, the first opponent reply is
   chosen by search over their full legal reply set (solver bitboard
   move-gen; the opponent takes the reply best for their own finish; CRN
   across replies). **Worse** — pc4 +0.150±0.128: in-world best response
   uses the sampled hidden cards the real opponent cannot see. Paranoid
   distortion; the CD_LEAF lesson in another costume.
2. **Reply tournament, defender only**: restricted to the defender's
   cover/pass/pickup reply — a decision made from information the real
   defender genuinely has (own hand + visible attack). Direction fixed,
   power revealed nothing: pc4 +0.028±0.051, pc3 −0.068±0.055 at 600/400
   pairs. Searching a reply under a *handwritten continuation* is not a
   better model of an MC defender than handwritten itself. (One real bug
   found on the way: PICKUP/GOOD rank last cheap-first and must never be
   pruned from the searched set.)
3. **MC-defender rollout model** (`OG_MCDEF`, `CD_POL_MCDEF`): rollout
   defenders with a proven mc_tell pick up rather than burn a trump while
   the deck lives (mixed 50%), instead of handwritten's unconditional
   cover-if-you-can — the exact behavior the mc_tell evidence detects.
   Evidence-gated, zero extra cost. **Flat-to-harmful** — pc3 +0.055±0.039:
   50% over-models how often real MC defenders actually pick up.
4. **Deeper heads-up rollout leaves** (10 cards / 8k nodes): flat
   (+0.003±0.034) at 4x the pc2 cost. Dead.
5. **Extended exact root-solve window** (28 cards, 400k/250k node
   budgets, vs semtex's 24 / 150k/100k): **strictly dominant** — across
   ~1,500 paired games it finished better in 4, worse in 0, identical
   otherwise. It binds in ~0.25% of heads-up deals and roughly doubles
   pc2 decision wall-clock. This is octogen's only enabled diff.

## The ceiling finding

Combined with the semtex hunts, the pattern over ~10 measured levers is
unambiguous: against a strong determinized-MC opponent, the outcome is
dominated by deal luck and world-sampling variance; refinements of an
already-close opponent model wash out (fulminate vs strong fields, espresso
rollouts, reply search, MCDEF), and only two lever classes ever paid —
**exact truth** (solver windows/leaves, now mined down to 0.25%-frequency
crumbs) and **variance** (world budgets, now measured to their knees at
every player count). A genuinely stronger successor needs a different
architecture (e.g. learned value functions over information sets, or true
information-set search), not another lever on this one.

## Final form and deployment

Octogen = semtex + the 28-card window (`octogen`/`og`, C only). Because it
is decision-identical to semtex outside that window, semtex's entire
validated dominance matrix (SEMTEX.md: cordite, blackpowder, espresso,
handwritten, random fields; ELO arena) transfers to octogen verbatim, plus
the strict extra wins. It is **deliberately not ported to TS / seeded into
production**: doubling heads-up decision CPU on Supabase for a 0.25%-of-
deals improvement fails the compute-budget bar; production keeps `semtex`
(base cost) and `semtex_max` (full measured world budgets).

## Knobs (`OG_*`, octogen only)

- `OG_SOLVE_CARDS` (28) / `OG_BB_WIN` (400k) / `OG_BB_AVOID` (250k) — the
  enabled lever.
- `OG_REPLY` (0) / `OG_REPLY_CAP` (6) / `OG_REPLY_STAGE` (2) — reply
  tournament, kept for research.
- `OG_MCDEF` (0) — MC-defender rollout model, kept for research.
- Everything else is inherited from semtex under the `OG_` prefix
  (`octogen_oracle` / `ogo` = 6x-worlds audit variant).
