# C5 (`CD_TT_BOUNDS`) — what happened, why, and what to do with it

**Status: store machinery landed and validated safe; bound *reuse* is a
characterized negative with a precise root cause and a specified fix (C5-v2).**
This doc is the execution handoff for that fix. Read §3 (root cause) until it
clicks — every design decision in §4 follows from it, and the naive fixes all
fail for the same reason the first attempt did.

Code state on branch (commit `09fa2ed`, file `cnitro/src/cordite_sim.c`):

| flag | state | meaning |
|---|---|---|
| `CD_TT_BOUNDS` | off by default, **safe** | entry gains a `bound` field {EXACT=0, LOWER=1, UPPER=2}; fail-high/fail-low results are *stored* as bounds (cards ≥ `CD_TT_BOUND_MINCARDS`=5) with EXACT-priority replacement. Probe treats bound entries as misses. Validated SIG-identical to std at TT22. |
| `CD_TT_BOUNDS_USE` | off, **known-broken** | enables sign-guarded cutoff/narrowing reuse of bound entries. ~30% outcome flips at TT22. Kept as a documented reference; do not ship. |
| `CD_TT_BOUND_MINCARDS` | 5 | bounds only cached for the deep (expensive) layer |

The 8-byte packed entry (`CD_TT_PACK8`, shipped) already reserves 2 bound bits,
so any v2 composes with the production 32 KiB table at zero extra bytes.

---

## 1. Why C5 exists — the prize

The solver is exact, node-budget-limited alpha-beta over a mate-value lattice
(`±(1000−d)` win/loss-in-d, 0 draw). The store census (`-DCD_TT_STATS`,
counters `CD_TT_STATS2 failhi/faillo`) measured that **completed refutations —
fail-high/fail-low subtree results — outnumber storable exact results by ~3,000×**
(hundreds of millions of discarded node-completions across a few dozen games).
Every one is thrown away today; the win-hunt re-derives the same refutations
over and over, which burns the shared node budget, which causes **aborts** —
and aborted solves are the mechanism behind every hard divergence we chased
this session (500459, 720958, 700910). Caching refutations attacks the abort
rate directly. That is a *strength* lever, not a memory lever.

## 2. What was measured (all deterministic, `CD_RACE=0` reproduces exactly)

Build pattern (from `cnitro/`, `CORE=$(make -s print-core)`):

```sh
cc -O2 -ffast-math -Isrc -Wno-deprecated-declarations \
   -DCD_TT_BOUNDS [-DCD_TT_BOUNDS_USE] -DCD_TT_2WAY -DCD_TT_BITS=22 \
   $CORE src/main_eval.c -o eval_bounds -lm
GAME_SIG=1 CD_BUDGET=prod CD_RACE=0 ./eval_bounds \
   --strategy=octogen --opp=espresso --players=2 --games=1 --seed-start=700003
```

| variant | vs std @TT22 | verdict |
|---|---|---|
| store-only (`CD_TT_BOUNDS`) | SIG-identical (all previously-flipped seeds + V0) | store is value-safe |
| full reuse (cutoff + window-narrow) | 18/60 espresso outcome flips, 4/60 hw; both w→l and l→w | broken |
| cutoff-only (no narrowing) | identical to full reuse | narrowing is NOT the culprit |
| sign-guarded cutoffs (LOWER only if v>0, UPPER only if v<0) | still flips (seeds 700003, 700006, 700008, 700012) | guarding the cutoff node is insufficient |
| 1-way vs 2-way | identical failures | not an associativity interaction |

Flip seeds for regression testing: `700003` (TT22-win → bounds-loss), `700008`
and `700012` (TT22-loss → bounds-win), `700006` (move-div, both lose).

## 3. Root cause — the exact-in-window invariant

The whole design rests on one theorem of classical fail-soft alpha-beta:

> If a node's fully-searched result `best` lands **strictly inside** its
> original window (`alpha0 < best < beta0`), then `best` is the **true game
> value** — because every child was either fully resolved or provably unable
> to affect an in-window result.

That is what licenses `store EXACT` and what makes the persistent TT sound
across the solver's **three different probe windows** — win-hunts
`(alpha, 2000)`, loss-probes `(-1, 0)`, in-sim hunts `(-1, 1)`.

A TT-bound **cutoff return breaks the theorem's precondition**. A `LOWER v`
cutoff is *sound as a classification* (true ≥ v ≥ beta really is a fail-high)
but the returned **magnitude** is only a bound — the true value may be higher.
The corruption then happens **at the ancestor, not at the cutoff node**:

1. Ancestor `P` (say a minimizing node) computes `best = min(children)` where
   one child's value is an understated LOWER-cutoff return.
2. `P`'s computed `best` can now land strictly inside `P`'s window while the
   TRUE value lies outside it, or vice versa.
3. `P` certifies this contaminated value as **EXACT**, stores it in the
   persistent table, and every later probe — under *any* window — consumes it
   as truth. The poison compounds across the whole game (the table persists
   per-game for octogen).

Why the damage is so large (30%, not a tail effect): **two of the three probe
windows are one unit wide** — `(-1, 0)` and `(-1, 1)`. The win/loss/draw
classification boundaries sit at ±1 around zero, so *any* magnitude distortion
that drifts a computed value across zero flips an outcome classification
outright. A chess engine tolerates search instability because it consumes
values only relative to the current window and re-searches on fail; this
solver consumes values as **mate-value truth** (sign = outcome, magnitude =
distance) and shares them across windows. Sign-guarding the cutoff node cannot
help because the ancestor mixes magnitudes regardless of their sign.

**One-line statement for the next agent: bounds are sound for *pruning
decisions*, unsound as *returned magnitudes*; the bug is certifying
magnitude-contaminated ancestors as EXACT.**

## 4. C5-v2 — the specified fix: certified-exactness (taint) propagation

Thread two taint bits through the recursion that record whether a returned
value may understate or overstate the truth. Prune with bounds freely (sound);
never certify a contaminated value as EXACT; never let a contaminated root
classification stand.

### 4.1 Plumbing

Add to `SimSolver` (like `aborted`): `uint8_t t_lo, t_hi;` — meaning for the
value just returned: `t_lo=1` ⇒ value may be BELOW the true value (an
understatement is possible), `t_hi=1` ⇒ may be ABOVE. `(0,0)` = certified
exact. Parent reads `S->t_lo/t_hi` immediately after each child call and
resets before its own return (same discipline as `S->aborted`).

### 4.2 Rules at the probe site (replaces the `CD_TT_BOUNDS_USE` block)

```
EXACT hit          -> return v            with (t_lo,t_hi) = (0,0)
LOWER hit, v >= beta -> return v          with (1,0)        // may understate
UPPER hit, v <= alpha -> return v         with (0,1)        // may overstate
LOWER hit, v > alpha  -> alpha = v        // narrowing is sound; taints nothing
UPPER hit, v < beta   -> beta  = v
```

(Keep the raise/lower of alpha/beta: a valid bound tightening the window only
prunes lines the bound *proves* irrelevant — it never changes the returned
value's provenance.)

### 4.3 Rules at the node (accumulation)

For a **maximizing** node (`actor == S->me`); mirror-image for minimizing:

- Track `acc_lo, acc_hi` for the node's own result, starting `(0,0)`.
- After each child returns `(v_i, lo_i, hi_i)`:
  - if `v_i > best`: `best = v_i`; the node's result inherits `(lo_i, hi_i)`
    as its *primary* taint.
  - if `v_i <= best` **and** `lo_i == 1`: the child's TRUE value might exceed
    `best` (its report understates) ⇒ set `acc_lo = 1` (our max may
    understate). A non-best child with `hi_i = 1` cannot raise the max ⇒
    ignore.
- Node's final taint = primary taint OR'd with `acc_*`.
- Beta-cutoff exits keep whatever taint `best` carries at that point
  (fail-highs are stored as bounds anyway, see 4.4).

Minimizing node: swap the roles (`hi_i` of non-best children can lower the
true min ⇒ `acc_hi = 1`).

### 4.4 Rules at the store site

```
in-window  (alpha0 < best < beta0) and taint (0,0)  -> store EXACT   (as today)
fail-high  (best >= beta0)         and t_hi == 0    -> store LOWER   (true >= best holds)
fail-low   (best <= alpha0)        and t_lo == 0    -> store UPPER
anything else                                        -> store NOTHING
```

The EXACT-priority replacement policy already written for C5 (a bound never
evicts or downgrades an EXACT entry; 2-way victim prefers dropping a bound)
stays exactly as-is — it was validated.

### 4.5 Rule at the root

`cd_sim_solve_d` returns `(v, taint)` to its internal callers. If the root
result's classification could move under its taint — i.e. `t_lo=1` and
`v <= alpha+1`, or `t_hi=1` and `v >= beta-1` (the value sits within taint
range of a window boundary) — **re-solve that root call once with bound-reuse
disabled** (add `S->use_bounds` gate; budget still shared). Untainted or
boundary-safe results stand. Expected to be rare (most solves resolve well
inside or well outside); instrument a counter to verify. This makes root
classifications provably identical to a bounds-free solver — behavior can
then change ONLY through the node-budget channel (fewer nodes burned), which
is the same channel as any speedup and is what V4 gates.

### 4.6 What NOT to do (each was considered or tried)

- **Don't** return bound values without taint and hope sign-guards save you —
  that is exactly the measured 30%-flip failure (§2, §3).
- **Don't** use bounds only for move ordering as a "safe" alternative: this
  session measured ordering changes (`CD_TT_ORDER2/3`, `CD_TT_ADAPT`) and all
  REGRESSED — ordering shifts which lines resolve within budget. It is not
  lower-risk than v2; it is the same risk with less upside.
- **Don't** reduce `CD_TT_BOUND_MINCARDS` below 5 until v2 is green end-to-end
  — near-leaf bounds add churn for subtrees that are cheap to recompute anyway
  (the census: ~91% of distinct keys are ≤6-card positions).
- **Don't** store bounds in the exact table's slots if capacity gets tight at
  small `CD_TT_BITS`: the clean alternative is a **separate side table** for
  bounds (a 4,096-entry packed table = 32 KiB pairs with the freed L1 budget —
  see `docs/L1_SPEND_PLAN.md` §S2). Same probe/store rules; zero interference
  with exact retention.

## 5. Validation ladder for v2 (all must pass, in order)

1. **V0** — default build (no flags): SIG-identical to HEAD (20 seeds, the
   standard check).
2. **V-flip** — the four flip seeds (`700003/700006/700008/700012`),
   `CD_RACE=0`, v2 @TT22 vs std @TT22: outcomes MUST match std (moves may
   differ only via the root-re-solve budget channel; expect identical at
   TT22 where budget is plentiful).
3. **V1** — 100 hw + 100 espresso seeds @TT22: **0 outcome flips**; move
   divergence only at knife-edge budget boundaries (expect ≤ 2/200, matching
   the RANKSYM-class budget artifact rate; investigate anything above that).
4. **V4** — `tools/tt_divergence_viz/outcome_pair.sh`, 3,000 espresso seeds,
   v2 at the SHIPPED config (`EXTRA='-DCD_TT_2WAY -DCD_TT_PACK8 -DCD_TT_BOUNDS -DCD_TT_BOUNDS_V2'`,
   `CAND=12`): **0 win→loss flips**; total flips ≤ the shipped config's 0/3000
   baseline plus at most budget-channel noise (any w2l = stop and bisect).
5. **V-strength** (the payoff gate): abort-rate counter (instrument
   `S->aborted` completions per game) must DROP vs shipped; then 2,000-game
   win-rate vs espresso at pc2 must be ≥ shipped octogen's (measure both arms
   same seeds). If aborts drop but win-rate doesn't move, the prize was
   overestimated — document the numbers and stop; the store scaffolding stays
   for the side-table variant.
6. **V-latency** — CPU-time probe (`CD_LAT=1`, 40 games): within ±5% of
   shipped. Taint plumbing is a few ALU ops per node; anything visible means a
   mistake (e.g. accidentally disabling the TT fast path).

## 6. Effort and priority

~80–120 lines in `sim_solve_rec` + the probe/store sites, plus counters.
Medium complexity, high care (the taint algebra in §4.3 is where bugs will
live — write it as a truth-table comment above the code). Priority: run it
AFTER the L1-spend candidates S0/S1 (see the companion doc) — those are
cheaper and independent; v2's payoff (abort reduction) compounds with theirs
(fewer nodes per subtree). If v2 fails its gates twice, close C5 permanently
with the numbers in this file's style — a documented negative is a valid
deliverable.
