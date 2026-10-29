# Fusion: octogen plus rollout noise

Fusion is octogen plus kestrel's rollout noise: in its heuristic rollouts, each seat's decision is, with probability 30%, a uniform pick from a small sensible candidate set instead of the rollout policy's move.
The noise is on at tables of two to five players and off at six and above, where fusion is octogen move for move, with no extra draw (the measurements below).
Everything else is octogen's: the same belief, sampled worlds, world budgets, root endgame solve, exact leaf, LEAFBOOK, candidate race and trump-keep tax.
It also carries a second lever for experiments, sapper's firmer pickup inference: a seat that picked up is assumed to hold nothing beating those cards in (m-1) of m sampled worlds (`og_void_mod` = m).
Fusion's default is octogen's 4 (3 of 4), because sapper's 7 (6 of 7, about 85%) measured flat in the league (see the first measurement below).
Registered as `fusion` / `fu`, `STRAT_FUSION = 24` (`src/strategy.h`), by its row in the bot registry (`src/bots.c`), and it is live on the website as Moscow, the top rung, since 2026-10-29 ("Live" below).
It is the first bot on the registry: its brain stays in `src/octogen_strategy.c` (it is octogen with two levers set, not a file of its own), and its row is its whole registration.

Both levers are set by `fusion_strategy_choose` for the length of one decision and put back after it, so an octogen seat on the same thread keeps noise 0 and mixture 4.
`og_void_mod` is octogen's thread-local, latched from `OG_VOID_MOD` on a flag load, so fusion loads octogen's flags first and then swaps its own value in; otherwise the first decision's own flag load would overwrite it.

### The defaults and the knobs

Each setting is a C default in `src/octogen_strategy.c` and a knob (env over roster spec, `bot_knobs.h`) read by fusion alone, so a paired fusion-vs-octogen run can change one without touching the control.
No knob is needed for the production behaviour.

| Knob | Default | Meaning |
| --- | --- | --- |
| `FUSION_NOISE` | 300 | per-mille chance that a rollout decision plays the noise |
| `FUSION_NOISE_MAX_PC` | 5 | the largest table (the game's seat count) the noise is on at; above it fusion sets 0 and is octogen |
| `FUSION_NOISE_MODE` | 0 | 0: uniform over the sensible candidates; 1: uniform over the seat's whole rollout menu (research only) |
| `FUSION_VOID_MOD` | 4 | the void mixture (octogen's value) |

The table size is the game's seat count, the size every measurement was taken at, not the number of seats still in.

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
  `fusion_strategy_choose` (`src/octogen_strategy.c`) sets it for the length of one octogen decision and puts 0 back.
  Octogen never touches it.
- **The bitboard rollouts** (the default path): every place a playout asks a seat for its policy move now asks the noise first.
  `sim_seat_move` is that one place for `cd_sim_playout_pol` and the reply playout's policy steps; `sim_hw_noisy_move` is the same for `cd_sim_playout` and `cd_sim_playout_leaf`.
  Octogen's default path is `cd_sim_playout_pol` (OG_BBLEAF defaults to 2, so the leaf flag is always on).
- **The struct rollout** (`og_simulate`, used under `OG_NO_FASTROLL`, `OG_LEAF`, `OG_DIFFTEST` or `OG_NO_WORLDSIM`): `cd_sim_noise_legal` builds a SimState from the determinized world, draws with the same `sim_noise_draw`, and maps the picked move onto the seat's lite menu by move kind and card set.
  The pick is made on the SimState in both paths, by one function, so in either mode the two cannot disagree on the menu, its order or the move an index means; the lite menu's own order never matters.
- **The stream**: both draws (the gate, then the pick) are `game_random()`, the rollout's own stream, which `og_run_stages` reseeds to the same `sim_rng` for every root candidate in a world.
  So the common random numbers across candidates are intact: the same worlds and the same rollout streams for every root candidate.
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

- `test_fusion_is_dispatchable_and_named`: id 24, `STRAT_COUNT` 25, the names parse, its own roster row (neither seeded nor offline, octogen's knobs, linked natively), both dispatches return a legal move and leave no noise, mode or knobs behind, and `FUSION_NOISE_MODE=1` reaches the rollouts (`CdNoiseStats.any`).
- `test_rollout_noise_seam`: on one fixed world, at 0 nothing is drawn and a playout is its stream's; at 1000 the same streams play out differently, every playout ends, and the per-seat-policy playout takes the noise too.
- `test_octogen_is_fusion_at_zero`: over stretches of 2- and 3-seat games, on both rollout paths, octogen and fusion with `FUSION_NOISE=0` choose the same move and leave both RNG streams in the same state, and neither draws the noise.
- `test_fusion_rollouts_are_noisy`: with no knob set, fusion fires on both paths at 24-36% of drawn decisions (29.8% and 30.1% measured; the default 300, and 200 or 400 both fall outside), every struct-path pick is on the lite menu, octogen beside it draws nothing, and the knob is back at 0 afterwards.
- `test_fusion_noise_modes_are_legal_on_both_paths`: over every decision of 120 random games (2-6 seats, both rule sets) and 1500 crafted big-hand boards, every sensible candidate is on the seat's lite menu, the whole-menu mode's menu IS the lite menu with every move once, and at the same stream state the bitboard path and the struct path draw the same gate and pick, play the same move and leave the stream in the same state, in both modes.
  The crafted boards are what reach a thrower with more than `MAX_MOVE_CARDS` table-valued cards; random games never did, and the first mutation check of the cap stayed green until they were added.
- `test_fusion_noise_by_table_size`: with no knob set, fusion draws the noise at 2 to 5 seats, and at 6 and 7 seats it is octogen, the same move and both streams after, with no draw.
- `test_fusion_void_mixture`: over 2- and 3-seat game stretches, fusion at `FUSION_VOID_MOD=7` with the noise off is octogen at `OG_VOID_MOD=7` move for move and stream for stream, octogen right after a fusion call is octogen at 4, and somewhere the two mixtures part.

`test_bot_roster_choose_scopes_knobs` runs every linked roster row, so fusion's row must dispatch, and `test_podkidnoy_every_brain` plays fusion too.

### Octogen is unchanged against main

No test pins an octogen decision to a value, so this branch was compared with main at its fork point (a detached worktree at `1cca1f9d5`) by `cnitro_eval --inspect`, which prints every move of a seeded game:

- octogen vs octogen at 2, 4 and 6 players, seeds 200001-200003: every transcript byte-identical;
- fusion vs octogen at 6 players on the same seeds: main's octogen transcript exactly (noise off above five);
- cordite, firecracker and blackpowder at 2 and 4 players, seeds 200001-200002 (the solver's `sim_gen_pass` now goes through `sim_pass_pool`): byte-identical.

`scripts/wasm_build.sh` on both trees: `web.wasm` 101,300 -> 101,333 B, `oracle.wasm` 159,502 -> 159,440 B, `oracle-mt.wasm` 1,105,416 -> 1,105,355 B.
The named `web.wasm` has no noise or fusion symbol at all; its function list is main's, and the only symbol-level change is one `.rodata` segment fewer.
The bytes are the `fusion` roster row (its key string and row in the table every module carries) and code layout from the `sim_seat_move` / `sim_hw_noisy_move` / `sim_pass_pool` refactors, which compile to the same calls with the noise out.

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
