# Cyclic deck-empty endgames: why the exact solver bails to MC, and why more depth can't fix it

**TL;DR.** In some deck-empty heads-up endgames the bitboard exact solver
(`cd_sim_solve`) returns *unknown* on every move, so the Infinite Oracle shows the
**MC** regime instead of an exact verdict. This is **not** a memory or depth limit —
the endgame *graph contains cycles* (a player can keep picking up, recycling cards, so
positions repeat), which makes the alpha-beta tree unbounded in depth. No finite depth
cap resolves it; raising the cap only explodes the node count. The correct method is
**retrograde (backward-induction) analysis** over the finite reachable position set, which
this note implements and validates offline. For a 12-card endgame the reachable set is
~37 M positions / ~110 M edges (multiple GB) — feasible on a server, **infeasible in a
browser tab**, which is where the oracle runs. So the MC fallback is working as designed,
and no shipped module changes. All investigation was oracle/offline-only; the server
`bots.wasm` / `rules.wasm` / `guards.wasm` and committed `public/oracle.wasm.gz` are
untouched and byte-identical.

## The position

Reproduced from replay seed `4f2707c6…7889ad0`, at octogen's move on log 88 (its `Q♣`
attack). Deck empty, table empty (a round boundary), heads-up:

| seat | role | hand | card ids |
|---|---|---|---|
| 1 (octogen) | attacker, to move | `10♦ Q♣ 10♠ 8♥* Q♥* 8♣ 9♣ K♠ K♥*` (9) | `47 36 8 19 23 32 33 11 24` |
| 0 (opponent) | defender | `10♣ J♥* 10♥*` (3) | `34 22 21` |

Trump = ♥ (`power_suit=1`). `*` marks trumps. Card id = `suit*13 + (value-1)`,
suits `S,H,C,D = 0,1,2,3`.

## What the oracle actually does here (and why it's MC)

The oracle worker (`src/oracle/oracleWorker.ts`) picks a regime from octogen's
per-decision dump:

- **11a EXACT** — `solver.applied && hasWinLoss` → a proven win/loss exists → show *exact*.
- **11c UNPROVEN-SOLVER DEFUSE** — `solver.applied && !hasWinLoss` → the solver ran but
  proved nothing → set the probe budget to 0 and keep batching as **MC**.

Driving the *deployed* octogen brain to this decision (OG_EXPLAIN dump) shows:

```
solver.applied=1  result=eval  proven win/loss verdicts = 0/12   (all "unknown")
   seat 1  deck 0  opp_counts [3,9]
```

So the endgame-solver gate **passes** (deck empty, heads-up, opponent hand deduced,
12 ≤ 28 cards) — the solver *does* run — but **every** one of the 12 root moves comes
back *unknown*, even at 100× the normal probe budget (2e6 → 2e8 nodes). No win/loss ⇒
11c defuse ⇒ MC. That is the entire answer to "why doesn't the infinite oracle do the
exact solver here?"

## Why it's unknown: cycles, not resources

`cd_sim_solve` aborts a node when it hits the ply cap (`CD_SIM_SOLVE_MAX_DEPTH`, 48),
the node budget, or the move cap. Re-running octogen's exact per-move solve
(`alphabeta_probe.c`, the same `cd_sim_solve_d(child, me, -2000, 2000, …, depth0=1)` call
octogen makes) across raised depth caps:

| depth cap | result | nodes / move | wall |
|---|---|---|---|
| 48  | all 12 abort | 10²–10⁴ | 0.03 s |
| 63  | all 12 abort | 10⁴–10⁵ | 0.11 s |
| 96  | all abort (2 resolve to **LOSS** at ~2e8 nodes) | 10⁵–10⁸ | 60 s+ |
| 160 | same | same | 60 s+ |
| 200 | all 12 abort | up to 3e7 | 24 s |

The node count grows ~exponentially with the cap instead of resolving. A cycle
*measurement* (record each node's fingerprint on the search path, count revisits)
fires **150–460 times per move** at depth 200 with `maxpath = 199` (lines run to the
cap). A finite state space with unbounded path depth **must** contain a cycle
(pigeonhole) — so the alpha-beta tree is infinite along those branches. **More depth
cannot help**; it only postpones the same abort at a deeper, exponentially wider wall.

(Naïve "treat a repeated position as a draw" is *wrong* here — the Graph-History-
Interaction problem — and was measured to corrupt otherwise-proven verdicts. It is not
a valid fix.)

## The correct method: retrograde analysis (and the true value)

Cyclic finite games are solved by **backward induction over the reachable set**, not by
depth-first search: enumerate every distinct reachable position, seed the terminals, and
propagate WIN/LOSS to a fixpoint; whatever is never proven WIN or LOSS is a **DRAW**
(perpetual play). `solve.c` does exactly this for the position above:

```
distinct non-terminal positions : 36,809,642
edges                           : 110,524,465
retrograde fixpoint             : WIN 18,402,401  LOSS 18,402,829  DRAW 4,412
ROOT (octogen to move)          : LOSS
```

**The position is a forced LOSS for octogen under perfect play** — every legal move
loses to correct defense (the defender's `10♣ J♥ 10♥` — two trumps — force octogen to be
the fool despite octogen's 9-vs-3 card lead). Alpha-beta can only ever confirm the two
`9♠/9♦` attacks as losses (after ~200 M nodes); retrograde proves all 12 lose.

This means octogen's flagged `Q♣` is **not an avoidable blunder** — the game was already
lost by this node, so no move changes the outcome. The oracle's MC-based "inaccuracy"
flag is noise here: it had no exact ground truth to rank against, because it could not run
the exact solver.

### Correctness of the retrograde solver

`validate.c` cross-checks retrograde against `cd_sim_solve` on random small endgames that
alpha-beta *fully* resolves (aborted=0): **60,000 cases at 3/4/5 cards, 0 disagreements.**
The 37 M-position result is identical under both a compact 44-byte encoding and the exact
258-byte `SimState`-prefix encoding, so the enumeration is faithful.

> Note: a stale earlier probe reported this position as an instant "draw." That was a bug —
> the probe never called `ensure_masks()`, so `VALUE_MASK` was zero and the solver
> generated no moves. With masks initialised, alpha-beta aborts exactly as octogen does.
> Always initialise engine masks before calling the sim directly.

## Feasibility for the oracle

| environment | retrograde memory (≈) | verdict |
|---|---|---|
| server / native | 37 M states + 110 M edges → ~3–15 GB | feasible (~35 s) |
| **browser tab (wasm32)** | same | **infeasible** (~2 GB practical ceiling) |

The oracle runs in the user's browser. Even the *correct* method does not fit its memory
budget at 12 cards, and a precomputed tablebase (LEAFBOOK) for 12-card positions is far
too large to store. So the practical answer is: **leave the MC fallback in place** — it is
the right behaviour for cyclic endgames the browser cannot exactly solve. The offline
retrograde solver here is the tool to consult when you want the *exact* truth of such a
position (as done above to clear octogen of the "inaccuracy").

## Reproduce

```sh
cd cnitro
# core sources minus cordite_sim.c (the tools #include it for the static sim_* helpers)
CORE="src/game.c src/deal_rng.c src/legal.c src/replay.c src/view.c src/awire.c \
  src/evwire.c src/random_strategy.c src/espresso_strategy.c src/handwritten_strategy.c \
  src/robusta_strategy.c src/firecracker_strategy.c src/gunpowder_strategy.c \
  src/blackpowder_strategy.c src/cordite_strategy.c src/astrolite_strategy.c \
  src/simple_heuristic_strategy.c src/champion_strategy.c src/ultimate_champion_strategy.c \
  src/hacker_strategy.c src/fulminate_strategy.c src/espresso_prod_strategy.c \
  src/handwritten_prod_strategy.c src/distill_feat.c src/distilled_strategy.c \
  src/semtex_strategy.c src/octogen_strategy.c src/torpex_strategy.c src/torpex_value.c \
  src/novichok_strategy.c"
FLAGS="-O3 -ffast-math -Isrc -Wno-deprecated-declarations -DCD_TT_BITS=20"

clang $FLAGS tools/endgame_retro/solve.c    $CORE -o build/eg_solve    -lm && ./build/eg_solve 60000000
clang $FLAGS tools/endgame_retro/validate.c $CORE -o build/eg_validate -lm && ./build/eg_validate 20000 4
clang $FLAGS tools/endgame_retro/alphabeta_probe.c $CORE -o build/eg_probe -lm && ./build/eg_probe 200000000
```

`solve.c` needs ~10 GB RAM for the 12-card case (it allocates lazily; touch is ~10 GB).
Edit the `A[]`/`D[]`/`power` arrays to solve a different endgame.
