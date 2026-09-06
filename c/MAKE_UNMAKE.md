# make/unmake in the endgame solver — a measured null

**Result: correct but not worth it. Reverted. Do not retread without a
different design.** The `-DCD_MAKE_UNMAKE` prototype was SIG-identical to the
shipped copy-make solver (proven, below) but only ~2% faster at best and a
wash-to-slightly-negative elsewhere, on ~185 lines of subtle,
correctness-critical solver code. Recorded here in the spirit of
`OCTOGEN.md`'s "The nulls".

## What was tried

`sim_solve_rec` (`cordite_sim.c`, the exact alpha-beta endgame minimax used by
cordite / semtex / octogen) clones the game state on **every node**:

```c
memcpy(child, s, offsetof(SimState, deck));   // ~258 B, per node
sim_apply_sol(child, actor, &moves[i]);
int v = sim_solve_rec(S, child, alpha, beta, depth + 1);
```

That `memcpy` showed as ~7.26% of solver time (`__memcpy_avx_unaligned_erms`)
in the T2/T3 profiles (`server/impls/native/PROFILE_HOTPATH.md`). The
prototype replaced copy-make with classic **make/unmake** behind a
default-OFF flag: mutate the one caller-provided `SimState` in place
(`sim_make_sol`), recurse on it, then reverse exactly the fields the move
touched (`sim_unmake_sol`) before the next sibling — value-identical by
construction (same nodes, same order, same values; only state management
differs). A field-level `SolUndo` captured the touched set: `hand[actor]`,
`good_mask`, `table_vmask`, `covered_mask`, `num_battles`,
`discard_pile_length`, `first_attacker`, `defender`, the elimination fields
(`status_p[]`/`in_mask`/`out_mask`/`elim_order[]`/`num_eliminated` — any
hand-empty player can be eliminated in `sim_refill`), and the specific
`atk[]`/`def[]` slots each move writes (a whole-array snapshot is
unnecessary; pickup / round-resolution only zero `num_battles`, they never
overwrite the table bytes below it).

## Correctness (the gate it DID pass)

- `solver_difftest` built with `-DCD_MAKE_UNMAKE`: **mismatches=0** at pc2/3/4
  (`./solver_difftest {2,3,4} 60 "" 300000`) — the make/unmake solver matches
  the struct reference exactly, including pickups, round transitions, and
  eliminations.
- A/B `cnitro_eval`, flag OFF vs ON, **byte-identical** results (mean_finish /
  win_rate / histogram) for every solver-using bot: octogen pc2/3/4, cordite
  pc2/4/6, semtex pc2/4.
- (During development, `solver_difftest` also caught a real bug before it could
  ship — stale `atk[]` bytes surviving a round transition being read again by
  an ancestor frame — which is exactly why the touched table slots are
  restored explicitly rather than assumed dead once `num_battles` is
  restored.)

## Why it washed out (the reason not to retread)

1. **The undo is not free.** The field-level save+restore moves roughly as
   many bytes as the 258 B trimmed copy it replaces (two `MAX_PLAYERS` arrays
   plus a dozen scalars, saved and restored per node). The copy it replaced
   was already trimmed of the dead `deck[]` tail and lands in a cache-hot BSS
   slot, so there was little slack to recover.
2. **The solver is only a fraction of a whole octogen decision.** The 7% is
   *of solver time*; across full games most plies are MC-rollout / policy work,
   not deep exact solves, so a solver-internal micro-opt barely moves the
   end-to-end number.

## Measured (Linux, 4-core Xeon, clang, `-O3 -ffast-math`, no `-flto`)

octogen self-play, serial (no CPU contention), mean of 3 runs each:

| config | OFF (copy-make) | ON (make/unmake) | delta |
|---|---|---|---|
| pc2, 30 games | 51.79 s | 50.77 s | **+2.0%** |
| pc3, 20 games | 7.48 s | 7.57 s | −1.2% (noise) |

A ~2% best case (and negative elsewhere) does not justify a second,
correctness-critical solver code path.

## If you revisit

A different regime could change the calculus: a solver-only microbenchmark
(where the solver is ~100% of the time, not a fraction), or the oracle's
big-TT build (`docs/INFINITE_ORACLE_DESIGN.md`, TT20) where the per-node copy
also churns an 8 MiB table's cache — there the win could be larger. A leaner
undo (skip the elimination-field save on non-resolving moves) or a
pure-inverse unmake (no save at all) might help, at the cost of more
branching / far more fragile round-transition reversal. Reproduce the gate
with the standalone build:

```sh
cd c
CORE=$(make -s print-core)
clang -O3 -ffast-math -Isrc -DCD_LEAFBOOK               $CORE src/main_eval.c -o /tmp/eval_off -lm
clang -O3 -ffast-math -Isrc -DCD_LEAFBOOK -DCD_MAKE_UNMAKE $CORE src/main_eval.c -o /tmp/eval_on  -lm
# A/B must be identical (strip the wall-clock line); solver_difftest must be mismatches=0.
```
