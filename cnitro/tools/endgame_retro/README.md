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
| `find_crawl.c` | Self-play octogen-vs-octogen over a seed range; at every small deck-empty decision, use retrograde to classify all legal moves and report nodes where exactly ONE move wins and the rest lose. Flags `NONOBV` when pure-MC (no root solver, no exact-leaf rollouts) picks a losing move — i.e. only the exact solver finds the win. Needs `-DOG_EXPLAIN_BUILD -DFOOLISH_ORACLE_BUILD` (for `og_reload_flags`). |
| `verify_crawl.c` | Replay one seed to a target ply and print, for that decision, every legal move's retrograde verdict plus octogen's real (solver-ON) pick vs its pure-MC pick. Confirms octogen actually plays the only winning move. Same build flags as `find_crawl.c`. |
| `dump_game.c` | Play one seed's octogen-vs-octogen game and emit the exact move stream as JSON — feed it to a TS driver (`reconstructSeededDeal` + `runPackedGameAction` + `encodeReplayV6`) to mint a shareable v6 replay URL for that game. |

Build/run commands are in `../../ENDGAME_CYCLES.md` ("Reproduce"). Edit the `A[]`/`D[]`/
`power` arrays in `solve.c` / `alphabeta_probe.c` to point at a different endgame.

**Finding a "crawl to victory" replay** (only one move wins, and it's non-obvious):
```sh
# NONOBV rows = octogen's exact solver finds the sole winning move that naive MC misses
clang -O3 -Isrc -DCD_TT_BITS=20 -DOG_EXPLAIN_BUILD -DFOOLISH_ORACLE_BUILD \
  tools/endgame_retro/find_crawl.c <core-minus-cordite_sim> -lm -o build/find_crawl
./build/find_crawl <seed_start> <count> <max_cards> <min_moves>   # e.g. 1 400 10 3
./build/verify_crawl <seed> <ply>     # confirm solver plays the win, MC loses
./build/dump_game    <seed> > moves.json   # then TS: drive + encodeReplayV6 -> URL
```
