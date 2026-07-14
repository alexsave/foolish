# endgame_retro — cycle-correct deck-empty endgame solver (offline analysis)

Offline tools for exactly solving deck-empty heads-up endgames that the bitboard
alpha-beta solver (`cd_sim_solve`) cannot, because the endgame graph is **cyclic**
(pickups recycle cards, so positions repeat → the search tree is unbounded in depth).
See `../../ENDGAME_CYCLES.md` for the full write-up and the worked 12-card example.

These are analysis-only tools. They `#include` `cordite_sim.c` to reach its static
`sim_*` helpers, and never touch any shipped module.

| file | what it does |
|---|---|
| `solve.c` | Enumerate the reachable position set and run a 3-valued (WIN/LOSS/DRAW) retrograde fixpoint → the position's exact game value. Cycle-safe (no GHI). |
| `validate.c` | Cross-check retrograde vs `cd_sim_solve` on random small endgames alpha-beta fully resolves (must be 0 disagreements). |
| `alphabeta_probe.c` | Replicate octogen's per-move `cd_sim_solve_d` verdict probe on one position; shows every move aborting (why the oracle falls back to MC). |

Build/run commands are in `../../ENDGAME_CYCLES.md` ("Reproduce"). Edit the `A[]`/`D[]`/
`power` arrays in `solve.c` / `alphabeta_probe.c` to point at a different endgame.
