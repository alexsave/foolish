# Fusion: octogen with a noisy second opinion

Fusion is octogen whose pick a second, noisy race may overrule.
At every decision it first runs octogen's own race, noise-free, and so knows octogen's pick.
It then runs the same race again over the same sampled worlds with kestrel's rollout noise on: in its heuristic rollouts, each seat's decision is, with probability 30%, a uniform pick from a small sensible candidate set instead of the rollout policy's move.
Octogen's pick is pinned in that second race (never culled), and the noisy race's pick replaces it only when it is better by more than two standard errors of the paired difference over those worlds ("the shadow selection" below).
It does this at every table size, 2 to 8 players.
Everything else is octogen's: the same belief, sampled worlds, world budgets, root endgame solve, exact leaf, LEAFBOOK, candidate race and trump-keep tax.
It also carries a lever for experiments, sapper's firmer pickup inference: a seat that picked up is assumed to hold nothing beating those cards in (m-1) of m sampled worlds (the void mixture m).
Fusion's default is octogen's 4 (3 of 4), because sapper's 7 (6 of 7, about 85%) measured flat in the league (see the first measurement below).
Registered as `fusion` / `fu`, `STRAT_FUSION = 24` (`src/strategy.h`), by its row in the bot registry (`src/bots.c`), and it is live on the website as Moscow, the top rung, since 2026-10-29 ("Live" below).
It is the first bot on the registry: its brain stays in `src/octogen_strategy.c` (it is octogen with one parameter block set, not a file of its own), and its row is its whole registration.

### One parameter block, and octogen never sets it

Everything fusion does differently from octogen is one struct, `OgFusion` (`src/octogen_strategy.c`): the noise level and mode, the void mixture, the shadow's bar and the noisy race's world share.
`fusion_strategy_choose` fills it from its defaults and knobs, points the thread-local `og_fu` at it for one decision and puts `NULL` back after it.
Octogen never sets `og_fu`, and every fusion branch in octogen's code is `if (og_fu)` (or `if (og_shadow)`, which only fusion's shadow race sets), so with `og_fu` `NULL` octogen runs exactly its own lines and draws exactly its own numbers.
There is no copy of octogen's search: the shadow race is `og_run_stages` itself, called a second time with a pinned candidate.
The rollout engine's noise knob (`cd_sim_set_noise`) is set from the block around the noisy race only, and put back to 0 after it.
The block, the shadow and the noise all compile only where the noise does (`CD_ROLLOUT_NOISE`): the browser's modules carry none of it, and `web.wasm` is byte-identical to main's.

### The defaults and the knobs

Each setting is a C default in `src/octogen_strategy.c` and a knob (env over roster spec, `bot_knobs.h`) read by fusion alone, so a paired fusion-vs-octogen run can change one without touching the control.
No knob is needed for the production behaviour.

| Knob | Default | Meaning |
| --- | --- | --- |
| `FUSION_NOISE` | 300 | per-mille chance that a rollout decision of the noisy race plays the noise |
| `FUSION_SHADOW` | 20 | the bar, in tenths of a standard error, the noisy race's pick must clear against octogen's; below 0, no shadow: the one race is noisy and picks alone (the fusion that went live first; research) |
| `FUSION_NOISE_MAX_PC` | 8 (`MAX_PLAYERS`) | the largest table (the game's seat count) the noise is on at; above it fusion is octogen |
| `FUSION_SHADOW_W` | 100 | the noisy race's worlds, percent of octogen's: the cost lever (research) |
| `FUSION_NOISE_MODE` | 0 | 0: uniform over the sensible candidates; 1: uniform over the seat's whole rollout menu (research) |
| `FUSION_VOID_MOD` | 4 | the void mixture (octogen's value) |

The table size is the game's seat count, the size every measurement was taken at, not the number of seats still in.

## Why the noise alone cost a fixed amount: octogen's ties

The first fusion (the one race, noisy) was better than octogen at 2 to 5 players and worse at 6, and a dose sweep showed the loss did not shrink with the dose: even at 1 per mille fusion was worse at 6 players by +0.28 places, and at 4 by +0.08.
The cause is octogen's tie-breaking.
With noise-free rollouts over shared worlds, 35-53% of decisions have their top two candidates scoring identically in every world: twins such as a three-card and a two-card attack of the same value, or a cover and a pickup that the rollout policy plays out the same way.
Octogen then picks by candidate rank: the strict `<` of its final pick and the drop-the-last rule of `og_cull` both keep the earlier candidate, and `og_pick_candidates` ranks "dump widest, spend cheapest" first, with the trump-keep tax on top.
That rank order is a prior worth about 0.15 places (the comment on `og_pick_candidates` has its measurement).
Any noise at all breaks those ties (at 6 players the tied share falls from 50% to 22% at 1 per mille), and a broken tie is a coin flip, so the noisy race played the later-ranked twin about half the time: the two-card attack, the pickup instead of the cover, good instead of the throw-in.
So the noise had a fixed cost (about +0.08 places at 4 players, +0.28 at 6) on top of its own gain (about -0.15 at every size), and the gain only won where the cost was smaller.

A second, smaller cause: the noise draws its gate from the rollout's own stream, `game_random()`, which also draws the deck (`sim_draw`).
The first version of this file said the common random numbers across candidates were intact; they are not past the first decision, because a candidate whose line asks one more decision consumes one more draw, and every later card moves with it.

## The shadow selection

Fusion keeps octogen's pick unless the noise says something about it.
`og_fusion_shadow` runs the race again with the noise on, over the same worlds (the world seed depends only on the strategy stream and the seat, so the second race samples the first race's worlds), with octogen's pick `o` pinned: `og_cull` never drops it.
Every world folds each alive candidate's finish minus `o`'s (`og_shadow_fold`), so at the end the noisy race's pick `f` can be compared with `o` paired over every world `f` played.
`f` is played only when its mean finish plus trump-keep tax beats `o`'s by more than two standard errors of that paired difference (`og_fusion_overrules`).
An exact tie never overrules, so the twins keep octogen's rank order; a real difference the noise exposes does.

The bar was measured at 1, 2 and 3 standard errors (1008 pairs, seeds 5000001): 4 players -0.072, -0.128, -0.077; 6 players -0.041, -0.057, -0.031.
Two is best at both, so the default is 20.

### What was tried and dropped

All 1008-pair cells, paired against octogen at octogen tables, against the shadow at 2 SE on the same seeds (4 players -0.128 +/- 0.062, 6 players -0.057 +/- 0.082):

| Variant | 4 players | 6 players | Verdict |
| --- | --- | --- | --- |
| a separate noise stream, keyed per seat decision, so the deck keeps its common numbers | -0.104 +/- 0.062 | -0.006 +/- 0.083 | worse at both: dropped, the noise stays on `game_random()` |
| a rollout that never finishes (0.2% at 6 players) scored the middle place, not last | -0.120 +/- 0.062 | -0.049 +/- 0.082 | level: dropped |
| the noisy race on half the worlds (`FUSION_SHADOW_W=50`) | -0.056 +/- 0.058 | -0.086 +/- 0.077 | mixed: stays a research knob |
| noise 500 per mille | -0.107 +/- 0.068 | -0.029 +/- 0.093 | no better: 300 stays |
| no shadow, a tie band of 0.02 places in the culls and the final pick | -0.054 +/- 0.091 | +0.083 +/- 0.130 | the cheap fix recovers part of the 6-player loss and nothing at 4: dropped |
| no shadow, a band of 0.05 | +0.019 +/- 0.088 | | dropped |

For reference, the first fusion on the same seeds: 4 players -0.052 +/- 0.090, 6 players +0.184 +/- 0.131.

## Measurements of the shadow (2026-10-31)

`cnitro_eval --strategy=fusion --control=octogen --opp=octogen --players=N` plays every seed twice on the same deal, fusion at seat 0 and then octogen at seat 0, against octogen tables, and reports the paired mean-place difference (negative: fusion better).
Each decisive cell is 2016 pairs: 6 workers of 336 pairs on disjoint seed ranges from base 9000001 (seeds no scan had used), `nice -n 10`, the shipped defaults with no knob set.
The first fusion is main's own binary (`ec506be0a`) on the same seeds.
The interval is 95%; better/worse/same counts the pairs.

| Players | Fusion (shadow) vs octogen | better/worse/same | The first fusion vs octogen |
| --- | --- | --- | --- |
| 2 | -0.164 +/- 0.025 | 530/201/1285 | -0.160 +/- 0.030 |
| 3 | -0.144 +/- 0.039 | 544/336/1136 | -0.125 +/- 0.047 |
| 4 | -0.109 +/- 0.043 | 478/330/1208 | +0.006 +/- 0.062 |
| 5 | -0.099 +/- 0.043 | 403/259/1353 | -0.027 +/- 0.069 |
| 6 | -0.073 +/- 0.064 | 495/398/1122 | (octogen: noise off) |
| 7 | -0.100 +/- 0.062 | 415/350/1251 | (octogen) |
| 8 | -0.009 +/- 0.062 | 352/317/1347 | (octogen) |

Fusion is better than octogen at 2 to 7 players and level at 8, where the 1008-pair scan on seeds 5000001 had it at -0.110 +/- 0.090; so the noise is on at every size and `FUSION_NOISE_MAX_PC` defaults to 8.
Against the first fusion it is level at 2 and 3 and better at 4 and 5, and at 6 to 8 it gains what the first fusion had to give up by switching the noise off.
The shadow also keeps far more of octogen's moves: "same" is 56-67% of the pairs against 33-50% for the first fusion, so the noise now changes the game only where it has evidence.

The scans that chose the design (1008 pairs, seeds 5000001 at 3, 4 and 6-8 players, 7000001 at 2, 8000001 at 5) had the shadow at 2 SE at -0.146 (2), -0.123 (3), -0.128 (4), -0.123 (5), -0.057 (6), -0.152 (7) and -0.110 (8).

Against other fields (1008 pairs, seeds 9100001; the control is still octogen at seat 0, the other seats the field):

| Field | 2 players | 4 players | 6 players |
| --- | --- | --- | --- |
| cordite | -0.121 +/- 0.031 | -0.142 +/- 0.061 | -0.182 +/- 0.092 |
| handwritten | -0.003 +/- 0.018 | -0.019 +/- 0.044 | -0.013 +/- 0.064 |

Against cordite, the strongest field below octogen, fusion gains as much as against octogen or more.
Against handwritten, the human proxy, it is level: octogen already wins 92% of heads-up games there, and the same game is played in 68-91% of the pairs.

### What it costs

The shadow runs the race twice, so a fusion decision costs about two octogen decisions, a little more where the noisy rollouts run longer.
Measured by a paired timer (octogen tables; at every seat-0 decision with a choice, octogen and then fusion on the same state and RNG state, 12 games per size, this Mac, native `-O3`):

| Players | octogen ms/move (max) | fusion ms/move (max) | ratio |
| --- | --- | --- | --- |
| 2 | 22.6 (229) | 51.9 (392) | 2.3 |
| 4 | 3.6 (30) | 8.9 (41) | 2.5 |
| 6 | 7.2 (32) | 20.3 (90) | 2.8 |
| 8 | 7.6 (38) | 21.0 (106) | 2.8 |

Octogen's budget is a count of sampled worlds (`og_params`), not a clock, and the server's bot drive has no per-move deadline: it paces the bots' moves in seconds and budgets whole drives (`foolish/server/src/app/bots.c`), so tens of milliseconds more per move are not visible to a player.
So fusion keeps octogen's world budget in both races and accepts the cost; `FUSION_SHADOW_W` halves the noisy race's worlds if the cost ever matters (about 1.9x octogen instead of 2.8x at 6 players), and measured mixed (-0.056 at 4 players, -0.086 at 6, against -0.128 and -0.057 at full worlds).

## The idea and where it came from

The idea is kestrel's, an outside bot whose search is determinized Monte Carlo like octogen's.
Kestrel measured it as its single biggest gain: 1.71 vs 1.43 points per game against itself without the noise (par 1.5).
Its explanation: a rollout policy is an opponent model, and a search that always meets the same deterministic model over-fits that model's exact habits.
A small dose of sensible alternatives in every seat's rollout play keeps the root values from leaning on them.
Kestrel applies the noise to every seat, the searching seat included; noise for opponents only measured neutral or worse there.

## Design

### Where the noise sits

- **The knob** is a thread-local per-mille in the bitboard rollout engine, `cd_sim_set_noise` / `cd_sim_noise` (`src/cordite_sim.h`, `src/cordite_sim.c`).
  It is 0 by default.
  Octogen's race code sets it from fusion's block around the noisy race only (`og_fusion_noise`) and puts 0 back after it.
  Octogen itself never touches it: every call is behind `og_fu`.
- **The bitboard rollouts** (the default path): every place a playout asks a seat for its policy move now asks the noise first.
  `sim_seat_move` is that one place for `cd_sim_playout_pol` and the reply playout's policy steps; `sim_hw_noisy_move` is the same for `cd_sim_playout` and `cd_sim_playout_leaf`.
  Octogen's default path is `cd_sim_playout_pol` (OG_BBLEAF defaults to 2, so the leaf flag is always on).
- **The struct rollout** (`og_simulate`, used under `OG_NO_FASTROLL`, `OG_LEAF`, `OG_DIFFTEST` or `OG_NO_WORLDSIM`): `cd_sim_noise_legal` builds a SimState from the determinized world, draws with the same `sim_noise_draw`, and maps the picked move onto the seat's lite menu by move kind and card set.
  The pick is made on the SimState in both paths, by one function, so in either mode the two cannot disagree on the menu, its order or the move an index means; the lite menu's own order never matters.
- **The stream**: both draws (the gate, then the pick) are `game_random()`, the rollout's own stream, which `og_run_stages` reseeds to the same `sim_rng` for every root candidate in a world.
  Every root candidate starts from the same stream, but the common random numbers do not survive the first decision: a line that asks one more decision consumes one more draw, and the deck's later draws move with it ("Why the noise alone cost a fixed amount" above).
  A separate stream keyed per seat decision kept them shared and measured worse ("What was tried and dropped"), so the stream stays.
  The two paths draw in the same order, so `OG_DIFFTEST` still compares like with like.
- **Never in an exact line**: the noise only replaces a policy choice.
  The root endgame solve, `cd_sim_solve*`, the in-rollout exact leaf and the reply tournament's enumeration never consult a policy, so solved lines are untouched.
  After a leaf solve aborts, the playout falls back to policy play, and that heuristic play takes the noise like any other.

### Why octogen at 0 stays byte-for-byte the same

Every draw site is behind `if (cd_noise_pm)`, and `cd_sim_noise_legal` returns before drawing when the knob is 0.
So at 0 not one extra `game_random()` is consumed, and octogen, cordite and every other brain on these playouts consume exactly the stream they always did.
The refactor that put the policy dispatch into `sim_seat_move` keeps the same branch order (LOOSE, MCDEF, else handwritten).
`CdNoiseStats.drawn` counts gate draws, so a test can prove a brain at 0 drew nothing.
The cost at 0 is one thread-local load per rollout ply in native builds.
The `sim_pass_pool` refactor (the pass gates shared by `sim_gen_pass` and the noise) changes no generated move: octogen, cordite, firecracker and blackpowder replay main's games exactly (below).

### The candidate set, and how kestrel's list maps

Built in `sim_noise_cands` on the engine's existing move primitives (`sim_attack_group_core`, `sim_greedy_full_cover`, `sim_gen_pass`), not a new generator.
Identical candidates collapse to one, so the pick is uniform over distinct moves.
Every attack is capped at the defender's capacity and at `MAX_MOVE_CARDS`, so every candidate is a move on the lite menu.

| Seat | Kestrel's candidate | Here |
| --- | --- | --- |
| Leader | one card, per value | the lowest-score card of that value (non-trump first) |
| Leader | the whole group, per value | every card of that value, capped, cheapest first |
| Defender | cheapest full cover | `sim_greedy_full_cover`, the policy's own cover and the lite menu's one cover |
| Defender | cheapest cover without trumps | not built: in this engine it is always either impossible or the same move as the greedy cover (see the comment on `sim_noise_cands`) |
| Defender | pickup | always |
| Defender | each distinct pass | one single-card pass per matching card, from `sim_gen_pass` (so the podkidnoy rule and the next seat's capacity are the generator's) |
| Thrower | good (stop) | always |
| Thrower | lowest matching non-trump | 1 card |
| Thrower | all matching non-trumps | capped |
| Thrower | two of them | the two lowest non-trumps |
| Thrower | a lone trump | the lowest matching trump |
| Thrower | everything | every matching card, capped |

### The whole-menu mode (`FUSION_NOISE_MODE=1`, research only)

"Actual random" against the sensible set: uniform over every move of the seat's `calculate_legal_moves_lite` menu.
That menu is the one every rollout deliberates over and the only one the struct rollout can play from, so its covers are the one greedy full cover; every attack, throw-in and pass is on it, each capped at `MAX_MOVE_CARDS` and at the receiving seat's capacity exactly as the lite enumerator caps them.
`sim_noise_fams` describes the menu as a few families (a single move, or every 1..kmax subset of a pool of cards: a leader's value group, a defender's matching cards with `sim_pass_pool`'s gates, a thrower's table-valued cards), counts it with binomials, and unranks one uniform index into its move.
Nothing is built: a thrower with a dozen table-valued cards has thousands of throw-ins.
The draws are the sensible mode's (the gate, then one pick), so the mode never changes how many numbers a decision consumes.
It measured level with the sensible set (below), so the default stays the sensible set and this mode stays a knob for further experiments.

### Where it is compiled, and where it is not

- **The native kernel** (`KERNEL_SRC`: the arena tools, the tests, the iOS xcframework, and the kernel the server links) has the noise compiled in, dormant at 0 for every other brain.
  The production server is c_base, which links `KERNEL_SRC` natively with `KERNEL_CFLAGS` (`c_base/Makefile`): `-DCD_LEAFBOOK` plus the `-DMAX_*` capacities from `print-human-caps`, which filters `WASM_FLAGS` to `-DMAX_%` and so never carries `FOOLISH_SEEDED_BOTS_ONLY` (that flag is only in `WASM_BOT_CFLAGS`).
  So the server can already run fusion with its noise: `table_add_bot` / `table_load` accept the `fusion` key (`bot_roster_linked` is 1 natively) and `bot_drive` plays it through `bot_roster_choose`.
- **The browser's modules** (`WASM_BOT_CFLAGS`: `web.wasm`, `oracle.wasm`, `oracle-mt.wasm`, and the e2e `table_host`, which is built from the same flags) define `FOOLISH_SEEDED_BOTS_ONLY`, which turns `CD_ROLLOUT_NOISE` off.
  There the noise code is compiled out, the setters are inline no-ops, and `fusion_strategy_choose` is not defined, so anything that named it would fail to link rather than run octogen under fusion's name; its `bots.c` row is `native_only` and its dispatch entry `NATIVE_ONLY(fusion_strategy_choose)`, NULL there, so `bot_roster_linked` says 0, exactly as for espresso_prod and gunpowder.
  No browser path runs a bot brain: `web.wasm` exports the client slot, the net client and the replay steps, none of which resolves a brain key, and the Oracle analyses replays with octogen, whatever brain sat in a seat.
  So the browser never needs fusion compiled in, for going live or after.
- **Its own roster row, seeded as Moscow.**
  `src/bots.c` has a `fusion` row at `STRAT_FUSION` with octogen's knobs (`OG_TRUMP_KEEP=40`), uses_logs on, seeded on, offline off, tier 11, city Moscow at 0 km, and seven server accounts.
  Seeded puts its accounts in the server's store (`bot_seed.c`) and so in every web lobby (the lobby lists the store's bot rows); offline off keeps it out of the phone's picker (`fio_strategy_count` is the offline count, and the picker resolves its ladder against it).
  The row stays `native_only`: the browser's modules never run a seat's brain, so they leave it out, and `e2e/bot_roster_parity.test.ts` no longer asks them to link a seeded bot; the server's own test does (`foolish/server/test/unit_store.c` `bot_seed_is_production`).
- **Its song is Moscow's.**
  A song belongs to a city, not a brain, and each seeded row's comment in `src/bots.c` names its city's; Moscow's is the National Anthem of the Soviet Union, so fusion carries it now and octogen took St. Petersburg's.

## Tests

All from `foolish/c`: `make build/cnitro_tests && ./build/cnitro_tests` (the whole suite), `FU_TRACE=1` prints the measured fire rates, and `make tests-wasm-caps` holds octogen under the wasm size limits.
The fusion tests (`tests/tests.c`, after `test_bot_roster_choose_scopes_knobs`), each with the mutations that turned it red written beside it:

- `test_fusion_is_dispatchable_and_named`: id 24, `STRAT_COUNT` 25, the names parse, its own roster row (seeded as Moscow, not offline, octogen's knobs, linked natively), both dispatches return a legal move and leave no noise, mode or knobs behind, and `FUSION_NOISE_MODE=1` reaches the rollouts (`CdNoiseStats.any`).
- `test_rollout_noise_seam`: on one fixed world, at 0 nothing is drawn and a playout is its stream's; at 1000 the same streams play out differently, every playout ends, and the per-seat-policy playout takes the noise too.
- `test_octogen_is_fusion_at_zero`: over stretches of 2- and 3-seat games, on both rollout paths, octogen and fusion with `FUSION_NOISE=0` choose the same move and leave both RNG streams in the same state, and neither draws the noise.
- `test_fusion_rollouts_are_noisy`: with no knob set, fusion fires on both paths at 24-36% of drawn decisions (29.8% and 30.1% measured; the default 300, and 200 or 400 both fall outside), every struct-path pick is on the lite menu, octogen beside it draws nothing, and the knob is back at 0 afterwards.
- `test_fusion_noise_modes_are_legal_on_both_paths`: over every decision of 120 random games (2-6 seats, both rule sets) and 1500 crafted big-hand boards, every sensible candidate is on the seat's lite menu, the whole-menu mode's menu IS the lite menu with every move once, and at the same stream state the bitboard path and the struct path draw the same gate and pick, play the same move and leave the stream in the same state, in both modes.
  The crafted boards are what reach a thrower with more than `MAX_MOVE_CARDS` table-valued cards; random games never did, and the first mutation check of the cap stayed green until they were added.
- `test_fusion_noise_by_table_size`: with no knob set, fusion draws the noise at every size from 2 to 7 seats; with `FUSION_NOISE_MAX_PC=5` it still draws at 2 to 5, and at 6 and 7 it is octogen, the same move and both streams after, with no draw.
- `test_fusion_void_mixture`: over 2- and 3-seat game stretches, fusion at `FUSION_VOID_MOD=7` with the noise off is octogen at `OG_VOID_MOD=7` move for move and stream for stream, octogen right after a fusion call is octogen at 4, and somewhere the two mixtures part.
- `test_fusion_shadow_rule`: `og_fusion_overrules` on its own: a pick better in every world overrules, an exact tie never does (even at bar 0), a pick about one standard error better overrules at 1 SE and not at 2, a worse pick never does, the trump-keep tax counts, and one world is not evidence.
- `test_fusion_shadow_selection`: over 2- and 3-seat game stretches with the noise on, at an unreachable bar (`FUSION_SHADOW=10000`) fusion plays octogen's own pick every decision although its rollouts drew the noise; at bar 0 the noisy race overrules octogen somewhere; and octogen right after a fusion call is octogen before it, the same move and both RNG streams, with the noise back at 0 (the octogen-identity guard: fusion leaves no parameter behind); and octogen's pinned pick played every world of every shadow race (`og_fusion_stats`). `FU_TRACE=1` prints the races and overrules (68 races, 23 overruled at bar 0).

`test_bot_roster_choose_scopes_knobs` runs every linked roster row, so fusion's row must dispatch, and `test_podkidnoy_every_brain` plays fusion too.

The shadow's mutation check (2026-10-31), each mutant built and run against the whole suite: all fifteen turned it red.
Dropping `gap < 0` from the rule, its `/ n`, the trump-keep tax or its one-world guard each fails `test_fusion_shadow_rule`; always playing the noisy pick, never running the shadow, or running the first race noisy fails `test_fusion_shadow_selection`; leaving `og_fu` set or the noise on after the race fails the identity checks across five tests; the per-size default at 5, an ignored `FUSION_NOISE_MAX_PC` and `<` for `<=` each fail `test_fusion_noise_by_table_size`; and a race that ignores fusion's void mixture, or octogen reading one, fails `test_fusion_void_mixture` and `test_octogen_is_fusion_at_zero`.
The first run left one mutant green: letting `og_cull` drop the pinned pick changed no move the tests looked at, because its stale score only feeds a rule that rarely overrules.
`og_fusion_stats` (races, overrules, and races in which the pin missed a world) was added for it, and the pin check turned it red.

### Octogen is unchanged against main

No test pins an octogen decision to a value, so this branch was compared with main (`ec506be0a`, a detached worktree) by `cnitro_eval --inspect`, which prints every move of a seeded game:

- octogen vs octogen at 2, 4 and 6 players, seeds 200001-200003: every transcript byte-identical;
- cordite, firecracker and blackpowder at 2 and 4 players, seeds 200001-200002: byte-identical (`cordite_sim.c` is main's file; only a comment in its header changed);
- fusion vs octogen at 6 players with `FUSION_NOISE_MAX_PC=5` (the noise off), seeds 200001-200003: main's octogen transcript exactly, so fusion with its noise off is octogen;
- fusion with `FUSION_SHADOW=-1` (no shadow) at 2 and 4 players, seeds 200001-200002: main's fusion transcript exactly, so the one-race fusion that went live is still there as a research setting.

`scripts/wasm_build.sh` on both trees: `web.wasm` is byte-identical to main's (102,188 B raw), so the browser's module carries none of the shadow.
`oracle.wasm` is 1 B smaller and `oracle-mt.wasm` 5 B smaller: the oracle builds run octogen, and the final pick moved into `og_best`, a function of its own that the shadow calls too; the struct rollout's loop was folded to one exit.
The oracle builds' behaviour is octogen's, whose transcripts are main's.

The earlier proof for the first fusion (against `1cca1f9d5`) is in git history; it covered the `sim_seat_move` / `sim_hw_noisy_move` / `sim_pass_pool` refactors the noise brought.

## First measurement (2026-10-28): the Bot Cup league

The serverless league (`cnitro_league_sim --accounts=7`, the gauntlet branch's 11 brains x 7 accounts, random table size 2-8) on seeds 100001..103000, with the 7 octogen accounts swapped for a variant and still labelled octogen, so every arm has the same tables, seats and deals.
Four arms ran concurrently, 2 workers each, so machine load hit every arm alike (the Bot Cup bots are time-budgeted, so a game is not reproducible run to run; the pairing is by table and deal).
The score is the octogen seats' average finish (place-1)/(n-1), lower is better; the delta is paired per seed against plain octogen, with a 95% interval.

| Table size | Games | octogen | fusion (noise + void 7) | noise only | void 7 only |
| --- | --- | --- | --- | --- | --- |
| all | 1115 | .382 | .355 (-.027 +/- .023) | .355 (-.027 +/- .023) | .376 (-.006 +/- .020) |
| 2p | 71 | .254 | .085 (-.169 +/- .104) | .155 (-.099 +/- .125) | .239 (-.014 +/- .108) |
| 3-5p | 420 | .383 | .350 (-.033 +/- .042) | .357 (-.026 +/- .041) | .382 (-.001 +/- .033) |
| 6-8p | 624 | .396 | .389 (-.007 +/- .027) | .376 (-.020 +/- .027) | .387 (-.009 +/- .025) |

The noise does the work; the void mixture at 7 is flat everywhere.
The other brains' deltas are all inside their intervals, so no single brain paid for the gain.

So fusion's default void mixture is now octogen's 4, and the noise is the whole bot.
`FUSION_VOID_MOD` stays as the lever for further experiments.
(The league arms ran the noise at 20%; the paired runs below moved the default to 30%.)

## Paired measurements (2026-10-28 and 29)

`cnitro_eval --strategy=fusion --control=octogen --opp=octogen --players=N` plays every seed twice on the same deal, fusion at seat 0 and then octogen at seat 0, against octogen tables, and reports the paired mean-place difference (negative: fusion better).
Each cell is 2016 pairs: 6 workers of 336 pairs on disjoint seed ranges from one base, `nice -n 5`, `FUSION_VOID_MOD=4`, built from this branch.
The interval is 95%.
A sanity arm at `FUSION_NOISE=0` reports every pair equal.

The dose, sensible mode (seeds 5000001 for 4 and 6 players):

| Players | 20% | 25% | 30% | 40% |
| --- | --- | --- | --- | --- |
| 2 (win% vs octogen's 50.1) | 64.1 | | 63.8 | 63.8 |
| 4 (mean place) | -0.046 +/- 0.062 | -0.060 +/- 0.062 | -0.086 +/- 0.063 | |
| 6 (mean place) | +0.104 +/- 0.092 | +0.054 +/- 0.091 | +0.136 +/- 0.093 | |

An earlier 500-pair sweep at 4 players and 30% was also significant (-0.18).
So the level is 30%: level with 20% heads-up, best at 4 players, and the noise is off at 6, where fusion was worse at every level.

The mode, at 30%, both modes on the same seeds (base 7000001):

| Players | Sensible candidates | Whole lite menu | Wall clock per cell |
| --- | --- | --- | --- |
| 2: win% (octogen 46.2) | 63.7 | 63.0 | about 18 min each |
| 2: mean place | -0.175 +/- 0.030, 698/345/973 | -0.168 +/- 0.030, 675/337/1004 | |
| 4: mean place | -0.063 +/- 0.060, 719/656/641 | -0.045 +/- 0.061, 719/659/638 | about 4 min each |

(better/worse/same pairs after each difference; the 2-player cells shared the machine with test builds, so their clocks are long.)
The two modes are inside each other's intervals at both sizes, so the default stays the sensible set, the simpler one and the one the earlier runs proved; the whole-menu mode stays as `FUSION_NOISE_MODE=1` for further experiments.

5 players, 30%, sensible mode (seeds 8000001, `FUSION_NOISE_MAX_PC=5`): -0.017 +/- 0.069, 664 better, 633 worse, 719 the same (win% 17.7 vs 19.7), 5 min.
That is flat: the point estimate is on fusion's side and the interval is wide either way.
The rule was "on unless the difference is positive", so the noise is on at 5 players, and `FUSION_NOISE_MAX_PC` is 5.

## Live (2026-10-29)

Fusion went live as Moscow, and every rung above Vienna moved down one city: octogen is St. Petersburg (635 km), cordite is Warsaw (1,151 km, new), fusion is Moscow (0 km).
A rating follows the bot: production's existing accounts keep their ids, strategies and ratings, and only their names changed; fusion's seven accounts (freshly minted v4 ids in its row) start at the default rating.

The renames needed no hand-run step on production.
The server's seed (`cb_import_seed_bots`, `foolish/server/src/store/store_import.c`) now brings every seeded account to its row: an id the store lacks is added, and a stored id whose nickname is not `%<city> <n>` gets that nickname and nothing else (`bots_rename`, a nickname-only, all-or-none write on both stores).
So the registry is the one owner of a seeded bot's name, and a city that moves is a `bots.c` edit plus a deploy.
On production's SQLite the seed runs at every store open, so the deploy that shipped this renamed cordite's 7 and octogen's 7 accounts and added fusion's 7 (21 rows) before serving a request; `seed_brings_production_to_the_ladder` in `foolish/server/test/unit_store.c` runs exactly that against production's 49 rows.
Nicknames carry no unique constraint, and games already played keep the names their seats were dealt with.
The leaderboard is edge-cached (`s-maxage=3600`), so it can show the old names for up to an hour after a deploy.

The touch points, for the next bot that goes live: its `bots.c` row (seeded, city, km, ids); its `bot.<key>` string in `i18n/keys.h` and the twenty-five `i18n/strings_*.c` (and `mayMatchEnglish` in `ios/FoolishTests/LocalizationTests.swift`); `LADDER` in `e2e/validation/bot_city_names_validation.test.ts` and the ladder in `e2e/validation/bot_ladder_order_validation.test.ts`; the seeded counts and the expected ladder in `foolish/server/test/unit_store.c` and `c_base/providers/aws/test/unit_dynamo_ops.c`; `docs/IOS_BOT_NAMING.md` and `docs/ARCHITECTURE.html` section 7.
