# A common C policy interface and a shared arena - consolidation pass over five products

Investigated in the `shared-consolidation` worktree, read-only.
Products: foolish (`foolish/c`, the card game, shipped, the bot lineage cordite/blackpowder/robusta/firecracker/fulminate/octogen/distilled), uttt (`uttt/c`, ultimate tic-tac-toe, near-shipped), pickemup (`pickemup/c`), chuiniu (`chuiniu/c`), tallybones (`tallybones/c`).
werewolf is out of scope and nothing here touches it.
Every finding below is a grep result, a diff, a line citation or a build/run command, not a guess; file paths are given so the claim can be re-run.

The question: does a common C interface, implemented as a struct of function pointers, make sense anywhere in this repo.
Two levels were evaluated separately: Part A, a shared abstract game-kernel interface (new/step/legal-moves/encode); Part B, a shared "run N games between two policies, report a win rate" arena/statistics module.

---

## Verdict

**Part A - a shared abstract game-kernel interface: fights the pattern, do not build it.**
`docs/ARCHITECTURE_AS_A_PATTERN.md` states the rule directly: "No host language declares the domain's types, and no hand-written host code knows a byte layout" and "the kernel owns the SHAPE, not just the rules" (lines 33-39).
The evidence below shows the five kernels' "new game" signatures already differ in exactly the ways an abstraction would have to erase - player count as an explicit argument in one (`fio_new_game`, `foolish/c/ios/include/ios_api.h:82`), fixed at two with no argument at all in another (`uti_new`, `uttt/c/ios/include/uttt_api.h:21`), and a 32-byte seed plus an opaque `dm` parameter with players added later by `_join` calls in three more (`pk_api_new`/`cn_api_new`/`tb_api_new`) - and the export surfaces range from 35 to 120 flat functions (chuiniu to foolish) because each kernel exposes exactly what its own game and its own Messages flow need, including UI-shaped entry points (`tb_api_read`/`tb_api_check`/`tb_api_text`, typed-text chat commands) that a `new/step/legal-moves/encode` abstraction has no slot for.
The one place a cross-product abstraction could plausibly have lived - the Swift bridges - was not built five times over by omission: each bridge calls its product's flat C names directly with no protocol or vtable in between (`chuiniu/ios/ChuiniuKit/Kernel/BridgeKernel.swift:33,83,97,104`).
The one gate that inspects export tables at all, `foolish/e2e/validation/wasm_exports_validation.test.ts` and `foolish/e2e/wasm_web_link.test.ts`, exists only for foolish and works by comparing a literal linked module's literal named exports and signatures against generated bindings - the opposite operation from a vtable, which would need to erase the exact per-function signature that test pins.
The one real code-sharing at this layer is `shared/tools/structgen`, a generator each product drives against its own headers into its own generated bindings (`STRUCTGEN_DIR := ../../shared/tools/structgen` in `pickemup/c/Makefile:145`, `chuiniu/c/Makefile:100`, `tallybones/c/Makefile:102`) - a bindgen, not a shared shape, exactly the distinction Part 1 piece 2 of the architecture doc draws.

**Part B - a shared arena/statistics module: build the small one (statistics + seed derivation), not the full policy interface.**
All five products already dispatch "a policy" through a closed enum plus a switch or an `if`/struct-of-config, never a function pointer (`foolish/c/src/main_eval.c:36-66`, `uttt/c/src/uttt_bots.c:1456-1477`, `pickemup/c/tools/pk_arena.c:35-48`, `chuiniu/c/bot/cn_arena.c:32-48`) - a struct-of-function-pointers "policy" interface would be a new abstraction none of the five currently want, contradicting the same flat-dispatch ethos Part A found at the kernel level one layer down.
The games differ on every axis a shared game loop would have to parametrize: player count (2-8 variable in foolish and pickemup/chuiniu, fixed at 2 in uttt, effectively solitaire in tallybones), win condition (elimination finish-position in foolish, X/O/draw in uttt, last-with-dice in chuiniu, a raw expected-value score with no opponent at all in tallybones), and information hiding (chuiniu and pickemup carry belief-model bots reading a masked view, uttt and tallybones do not).
Even foolish alone has three separate, structurally different arena programs (`main_eval.c` paired-control/multi-pc/legacy-2p, `cordite_sim.c` the sim engine under it, `main_elo.c` round-robin ELO with its own independently re-typed copy of the strategy switch at `foolish/c/src/main_elo.c:53-66` next to `main_eval.c`'s at `main_eval.c:36-66`) - "the game loop" is not one shape even inside one product.
What genuinely is the same shape five times over, and is worth lifting, is the small arithmetic at the bottom: seeding a per-game seed from `(identity, game index)` with a local hash, paired/CRN seat assignment, and the variance-to-stderr and proportion-confidence-interval math, which is proven below to be the *same formula* reimplemented with a *provable discrepancy* between two of the reimplementations (`chuiniu/c/bot/cn_arena.c:177-178` omits the Bessel correction that `foolish/c/src/main_eval.c:510` and `tallybones/c/bot/tb_solve.c:56` both apply) and a genuine methodology split on the proportion interval (`pickemup/c/tools/pk_arena.c:157-160` normal approximation, whose own header comment at line 15 calls it "the normal one" as if aware a better one exists, versus `chuiniu/c/bot/cn_arena.c:96-103`'s true Wilson score interval) - while `uttt/c/tests/uttt_arena.c` prints no interval at all.
Recommendation: build `shared/c` pure-math statistics functions (mean/stderr with Bessel correction, Wilson interval, normal-approximation proportion interval) and, separately, a shared per-game seed-hash helper.
Do not build a shared policy interface or a shared game loop.
Risk containment: this lift touches none of the per-game loop, RNG or dispatch code that produces foolish's regression-baseline numbers (the GRPO SFT-vs-GRPO comparison and cordite's ELO ranking) - it only replaces the last few lines of arithmetic, and the proof command below is a byte-diff of arena stdout at a fixed seed, before and after.

---

## Part A evidence

### Export surface per product

| product | header | lines | flat exported functions (approx) | wasm export validation test |
| --- | --- | --- | --- | --- |
| foolish | `foolish/c/ios/include/ios_api.h` | 1309 | 120 | `foolish/e2e/validation/wasm_exports_validation.test.ts` (Makefile export list vs. C definitions vs. hand-written TS binder, run on every PR) and `foolish/e2e/wasm_web_link.test.ts` (subset + one layout hash + one answer between `bots.wasm` and `web.wasm`) |
| uttt | `uttt/c/ios/include/uttt_api.h` | 499 | 103 | none - uttt does link and ship a real wasm module, `wasm-web` (`uttt/c/Makefile:253`, building `uttt/web/public/uttt.wasm`, the browser replay's drawing kernel), but nothing compares its export list, its signatures or a layout hash against anything, and no test under `uttt/` was found that does |
| pickemup | `pickemup/c/ios/include/pk_api.h` | 449 | 79 | none found; `pickemup/c/Makefile`'s `wasm:` target (line 91) only compiles freestanding objects (proof of no libc reach, per its own comment), never links a module; a separate `cross` target (`pickemup/c/Makefile:104-113`) does link a wasm module with explicit `-Wl,--export=pk_cross_run -Wl,--export=pk_cross_hashes`, but that is a native-vs-wasm parity test (`pk_cross.c`), not an export-surface validation test, and it does not touch `pk_api.h`'s 79 exports |
| chuiniu | `chuiniu/c/ios/include/cn_api.h` | 208 | 35 | none found; `chuiniu/c/Makefile`'s `wasm:` target (line 69) is the same objects-only shape as pickemup's - it compiles, it does not link or export, and nothing validates an export list |
| tallybones | `tallybones/c/ios/include/tb_api.h` | 230 | 47 | none found; `tallybones/c/Makefile`'s `wasm:` target (line 67) is again objects-only, same shape as chuiniu's - it does not link a module, so there is no export list to validate |

Sample export names, foolish (`ios_api.h:82-...`): `fio_new_game`, `fio_reseat_game`, `fio_set_passing`, `fio_view_ptr`, `fio_view_of_resident`, `fio_push_open`, `fio_push_next`, `fio_state_packed`, `fio_legal_packed`, `fio_legal_from_view`, `fio_play_probe`, `fio_layout_hash`.
Sample export names, uttt (`uttt_api.h:21-92`): `uti_new`, `uti_play`, `uti_legal`, `uti_undo`, `uti_over`, `uti_turn`, `uti_encode`, `uti_decode`, `uti_msg_open`, `uti_msg_read`, `uti_msg_check`.
Sample export names, pickemup (`pk_api.h:33-93`): `pk_api_layout_hash`, `pk_api_seats_load`, `pk_api_seats_save`, `pk_api_new`, `pk_api_read`, `pk_api_check`, `pk_api_commit`, `pk_api_join`, `pk_api_start`, `pk_api_draw`, `pk_api_play`, `pk_api_say_it`.
Sample export names, chuiniu (`cn_api.h:47-117`): `cn_api_layout_hash`, `cn_api_seats_load`, `cn_api_new`, `cn_api_read`, `cn_api_adopt`, `cn_api_commit`, `cn_api_join`, `cn_api_start`, `cn_api_raise`, `cn_api_call`.
Sample export names, tallybones (`tb_api.h:41-105`): `tb_api_layout_hash`, `tb_api_seats_load`, `tb_api_new`, `tb_api_read`, `tb_api_mark_sent`, `tb_api_adopt`, `tb_api_stage_join`, `tb_api_stage_start`, `tb_api_stage_keep`, `tb_api_stage_score`.

### "New game" signature divergence

| product | signature | file:line | what it must carry that the others do not |
| --- | --- | --- | --- |
| foolish | `int fio_new_game(const uint8_t *seed, int seed_len, int n_players);` | `foolish/c/ios/include/ios_api.h:82` | variable-length seed bytes, explicit `n_players` 2..8 (comment at line 92 spells out the range) |
| uttt | `void uti_new(int32_t seed);` | `uttt/c/ios/include/uttt_api.h:21` | a scalar seed, no player-count argument at all - the game is always exactly two seats, X and O |
| pickemup | `int pk_api_new(const uint8_t seed[32], int dm);` | `pickemup/c/ios/include/pk_api.h:60` | a fixed 32-byte seed, a `dm` flag, no player count - players are added afterward via `pk_api_join` |
| chuiniu | `int cn_api_new(const uint8_t seed[32], int dm);` | `chuiniu/c/ios/include/cn_api.h:75` | same shape as pickemup, different game (dice, bid/call rounds, not cards) |
| tallybones | `int tb_api_new(const uint8_t seed[32], int dm);` | `tallybones/c/ios/include/tb_api.h:64` | same shape again, but the underlying game (below) is fundamentally single-seat scoring, not a competition |

No two of the five agree on how many arguments "start a game" takes or what they mean, before even reaching what "step" or "legal moves" or "encode" would have to mean per game.

### structgen is driven per product, never shared as a shape

`pickemup/c/Makefile:145`, `chuiniu/c/Makefile:100`, `tallybones/c/Makefile:102` all set `STRUCTGEN_DIR := ../../shared/tools/structgen` and build `$(STRUCTGEN_DIR)/build/structgen`, then run it against that product's own `ios/include/*.h` to emit that product's own `ios/Generated/` Swift readers.
`foolish/tools/structgen/gen.sh` is foolish's own driver script for the same shared binary, over foolish's own headers, per `shared/README.md`'s own description of the split: "what stays product-side is only configuration: `tools/structgen/gen.sh` is this product's driver, its specs and its fixtures" (foolish's copy of that sentence, `docs/ARCHITECTURE_AS_A_PATTERN.md:126-128`).
This is the actual, working instance of cross-product reuse at the kernel-binding layer, and it is a generator over shape, not a runtime interface over behavior - which is exactly why it does not contradict the flat-export finding above: the generator reads each product's own, different flat names and emits each product's own, different typed accessors.

### Swift bridges call flat names directly

`chuiniu/ios/ChuiniuKit/Kernel/BridgeKernel.swift:33` - `public static let layoutMatches: Bool = cn_api_layout_hash() == SG_LAYOUT_HASH`.
`chuiniu/ios/ChuiniuKit/Kernel/BridgeKernel.swift:83` - `cn_api_seats_load(b.baseAddress, Int32(b.count))`.
`chuiniu/ios/ChuiniuKit/Kernel/BridgeKernel.swift:97` - `let n = cn_api_seats_save(&buf, Int32(buf.count))`.
`chuiniu/ios/ChuiniuKit/Kernel/BridgeKernel.swift:104` - `b.withUnsafeBufferPointer { cn_api_nickname($0.baseAddress, Int32(b.count)) }`.
No protocol, no closure table, no `switch` over a kernel enum - a straight extern-C call by name, per product, matching `tallybones/ios/TallybonesKit/Kernel/BridgeKernel.swift` (`tb_api_can_keep`, `tb_api_can_score`, `tb_api_mark_sent`, referenced in its own header comment lines 9-13) in the same shape.

### Part A conclusion

A struct-of-function-pointers game-kernel interface would have to either (a) force every product's `new` to take the union of all five signatures' worst case (a variable-length seed, an explicit player count even where the game has none, a `dm` flag foolish and uttt do not have), which every current caller would carry and ignore most of the time, or (b) become a thin, storage-only interface (`void *new(bytes seed)`, `void step(void *state, bytes move)`, `bytes encode(void *state)`) that pushes every real distinction back into per-product opaque byte blobs on both sides of the call - precisely the "host holds a pointer... and a hand-written function that walks the kernel's bytes" shape `docs/ARCHITECTURE_AS_A_PATTERN.md:39` names as the thing this repo already measured and moved away from.
Either way it fights the pattern rather than fitting it.
No further evidence changes that; the verdict is do not build it.

---

## Part B evidence

### Per-product arena table

| product | file(s) | total lines | policy representation | per-game loop | RNG seeding | paired/CRN + seat swap | statistic reported | arg parsing |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| foolish | `foolish/c/src/main_eval.c` (596) + `cordite_sim.c`/`.h` (1886+276, the sim engine `play_one` calls into) | 2758 | `enum STRAT_*` int, `switch` dispatch, `foolish/c/src/main_eval.c:36-66` | `play_one`, `main_eval.c:270-331`, eligibility scan + Fisher-Yates tie order + per-mover legal-move dispatch, ~62 lines | `game_set_seed(seed)` at `main_eval.c:282` calls into `shared/c/deal_rng.h` via `foolish/c/src/game.c:14,106` (ChaCha) | paired: `--control` path plays the SAME seed for hero and control back to back, `main_eval.c:465-466`; no seat swap (opponent always fills seats 1..N-1) | 3 different statistics depending on CLI path: plain win% (`main_eval.c:540-541`), paired mean-diff +- SE (`main_eval.c:509-516`), and mean-finish + histogram with no interval (`main_eval.c:585-591`); plus a 4th tool, `main_elo.c` (271 lines), computing round-robin ELO with a logistic expected-score formula, `main_elo.c:71-73` | `get_arg`/`parse_int` from the product-local `cli_util.h` (44 lines, itself already lifted "Replaces the get_arg / parse_int copies that used to live in every main_*.c", `cli_util.h:1-3`), ~15 lines of dispatch across 5 CLI paths |
| pickemup | `pickemup/c/tools/pk_arena.c` | 279 | `struct Agent { name, strategy, flags }`, indexed by `enum { A_RANDOM, A_GREEDY, A_MC, ... }`, `pk_arena.c:35-48` | `play()`, `pk_arena.c:106-124`, ~19 lines (delegates the whole game to `pk_bot_round` in a loop) | local seed: splitmix64-derived `seed_of(n, l, i)`, `pk_arena.c:74-85`, feeding a 32-byte seed to `pk_new`, which (confirmed via `pickemup/c/src/pk_deck.c:10,36`) calls `shared/c/deal_rng.h`'s `deal_rng_seed_at` for the deck shuffle | seat swap: `side[s] = (s + i) & 1` alternates A/B seating every game, `pk_arena.c:136`; header comment (lines 11-15) states this explicitly as luck-cancelling | normal-approximation 95% proportion interval, `ci()`, `pk_arena.c:157-163`; separately a plain mean for cards-remaining with no interval | manual positional `argv[1..4]` (games, jobs, size mask, lineup mask), `pk_arena.c:165-185`, ~20 lines |
| chuiniu | `chuiniu/c/bot/cn_arena.c` | 184 | `enum { P_BOT, P_RANDOM, P_PRIOR, P_NOREAD, P_READERS, P_FITBETA }` + a `CnBotCfg` struct per policy, `cn_arena.c:32-38,134-143` | `play()`, `cn_arena.c:50-81`, ~31 lines, one seat's move chosen per iteration by an `if`/`else` on the policy enum | local `cn_splitmix`-derived per-game seed, `cn_arena.c:52-57`; feeds a 32-byte seed to `cn_new`, which (confirmed via `chuiniu/c/src/cn_dice.c:23,56`) calls `shared/c/deal_rng.h`'s `deal_rng_seed_at` for the dice rolls | seat rotation: `A_mix[(s + gi) % A_seats]`, `cn_arena.c:61`, cycles every policy through every seat position, not just a 2-way swap, because `--mix` can name more than 2 policies at once | true Wilson score interval for win rate, `wilson()`, `cn_arena.c:96-103`; normal-approximation SE (no Bessel correction) for mean dice lost, `cn_arena.c:177-178` | manual `--key=value` prefix scan, `cn_arena.c:109-119`, ~11 lines |
| tallybones | `tallybones/c/bot/tb_solve.c` (75) + `tb_bot_test.c` (400) | 475 | not a policy-vs-policy arena at all (see below); the one "policy" is the solved-optimal decision table itself, no dispatch | `tb_bot_simulate`'s inner loop (in `tb_bot.c`, not shown here) drives one seat with one fixed policy against no opponent | `deal_rng.h` directly, confirmed `tallybones/c/bot/tb_bot.c:3,238,243` (ChaCha), seeded from a fixed string plus a numeric arg in `tb_solve.c:50-52` | not applicable - there is no second policy and no opponent to pair against or swap seats with | mean, sample variance (Bessel-corrected), stderr, and a z-score against the EXACT solved expected value, `tb_solve.c:55-59` and `tb_bot_test.c:329-333` (`z = (mean - EV) / se`) - a solver-validation statistic, not a win-rate statistic | `tb_solve.c:32-39`, manual positional `argv` with a `-t` flag, ~8 lines |
| uttt | `uttt/c/tests/uttt_arena.c` | 209 | `enum UtttBot` (`BOT_RANDOM`, `BOT_BIRO`, `BOT_ROLLER`, ... `BOT_QUILL`, `BOT_FOUNTAIN`), `switch` dispatch in `uttt_bot_move`, `uttt/c/src/uttt_bots.c:1450-1477` (not in the arena file itself - the arena calls the kernel's own dispatcher) | `duel()`, `uttt_arena.c:46-57`, ~12 lines, strict alternating turns, no eligibility scan needed (the game is inherently sequential) | local seed: `0x9E3779B97F4A7C15ull ^ ...`, `uttt_arena.c:39-43`, XOR-hash of `(a, b, g)`; uttt has no `deal_rng` usage anywhere in `uttt/c/src` - there is no deal, the game has no hidden information or randomness in its rules, the seed only drives Monte-Carlo rollout randomness inside the bots | paired: "THE SAME SEED FOR BOTH COLOURS... a lucky opening cannot favour one of them", `uttt_arena.c:142-147`; round-robin over every bot pair, not just two policies | none - a plain win-share percentage per pairing, `uttt_arena.c:183-202`, no confidence interval printed at all despite game counts as low as 200/pairing | manual positional `argv[1..3]` (games, budget, jobs), `uttt_arena.c:113-120`, ~8 lines |

### tallybones has no "two policies, win rate" arena

The task's premise was that tallybones has an arena-equivalent; it does not, and that absence is itself evidence.
Tallybones' bot is a solved-optimal-play table for a solitaire dice game: one seat, no opponent, the "final score" is compared against the exact expected value the table itself computed, not against another policy.
`tb_solve.c play N [SEED]` (comment, `tb_solve.c:6-9`) plays N games of the SAME policy against nobody and reports whether simulation matches the exact solver within a few standard errors - a solver self-test, not a competitive statistic.
`tb_bot_test.c`'s `test_kernel` (`tb_bot_test.c:344-381`) plays two-seat games through the real kernel, but both seats run the identical policy; it exists to prove the kernel accepts every move the policy proposes, not to compare two different policies.
`grep -rln "win_rate|elo|confidence|wilson|games_won" tallybones/c --include=*.c` returns nothing.
If tallybones ever gets a competitive win-rate arena, the game itself would need a two-policy competitive mode first; that is a product decision, not something this pass should assume into existence.

### The statistics math is the same formula, reimplemented, with a real discrepancy

Bessel-corrected sample variance to standard error, computed identically in two products:

`foolish/c/src/main_eval.c:510-511`
```c
double var_d  = (dsum2 - dsum * dsum / valid) / (valid > 1 ? valid - 1 : 1);
double se_d   = sqrt(var_d / valid);
```

`tallybones/c/bot/tb_solve.c:56-57`
```c
double var = (sim.sumsq - n * mean * mean) / (n - 1);
double se = sqrt(var / n);
```

The same quantity, computed WITHOUT the Bessel correction, in chuiniu:

`chuiniu/c/bot/cn_arena.c:177-178`
```c
double mu = lost[p] / seats[p], var = lost2[p] / seats[p] - mu * mu;
double se = sqrt(var > 0 ? var / seats[p] : 0);
```

At the game counts these arenas run (hundreds to low thousands), the `n` vs `n-1` difference is small but is a real, provable divergence between three call sites computing what is meant to be the identical statistic - not a design choice recorded anywhere, just drift.

Proportion confidence interval, two different methods for the same 95% claim:

`pickemup/c/tools/pk_arena.c:156-160`
```c
/* The normal interval, as p and its half-width; the printer clamps it to [0, 1]. */
static void ci(double won, double n, double *p, double *h) {
    *p = n > 0 ? won / n : 0;
    *h = n > 0 ? 1.96 * sqrt(*p * (1 - *p) / n) : 0;
}
```

`chuiniu/c/bot/cn_arena.c:96-103`
```c
static void wilson(double k, double n, double *lo, double *hi) {
    const double z = 1.959964;
    double p = k / n, d = 1 + z * z / n;
    double c = (p + z * z / (2 * n)) / d, h = z * sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d;
    *lo = c - h;
    *hi = c + h;
}
```

pickemup's own header comment (`pk_arena.c:14-15`) says "the 95% interval is the normal one over the games played" - phrasing that reads as aware a non-normal (Wilson) option exists, without adopting it.
The two methods diverge most exactly where an arena result matters most: small samples or a win rate near 0% or 100%, which is precisely when a normal approximation can produce an interval outside `[0, 1]` (pickemup's own `lo()`/`hi()` clamp functions, `pk_arena.c:162-163`, exist only because the normal approximation needs the clamp; Wilson does not).
`uttt/c/tests/uttt_arena.c:183-202` prints a plain win share with no interval at all, so the same underlying question ("is this bot actually better") gets three different answers across three products: nothing, a normal approximation that needs clamping, and a correct Wilson interval.

### Per-game seed derivation is nearly the same hash, three times

`pickemup/c/tools/pk_arena.c:74-85` - splitmix64-style mixing of `(n, l, i)` into a 32-byte seed.
`uttt/c/tests/uttt_arena.c:39-43` - a simpler XOR/multiply hash of `(a, b, g)` into a 64-bit seed.
`chuiniu/c/bot/cn_arena.c:50-57` - `cn_splitmix` (chuiniu's own copy of the same splitmix64 algorithm) mixing `(A_seed, gi)`.
None of the three calls `shared/c/deal_rng.h` for this outer hash (deal_rng is used only inside the per-product game kernels, for the deal/roll itself, once a 32-byte or scalar seed has already been produced by one of these three near-identical local hashes).
This is a small, pure, game-agnostic function duplicated three times with no game knowledge in it at all - the cleanest possible lift candidate.

### uttt also has a second arena tool, with its own independently invented Elo math

The table above only lists `uttt_arena.c`, but `uttt/c/tests/uttt_ladder.c` (168 lines) is a second, separate arena tool in the same directory, structurally parallel to foolish's `main_eval.c`/`main_elo.c` split.
It wraps a policy in an `Entrant` struct (`bot`, `budget`, `noise`, `label`, `uttt_ladder.c:24`) rather than the bare `UtttBot` enum `uttt_arena.c` uses, adds a uniform-noise ablation knob, and fits Elo to the whole round-robin table by Bradley-Terry MM iteration:

```c
// uttt_ladder.c:143-154
double s[NMAX]; for (int i = 0; i < N; i++) s[i] = 1;
for (int it = 0; it < 5000; it++) {
    double ns[NMAX];
    for (int i = 0; i < N; i++) {
        double w = 0, d = 0;
        for (int j = 0; j < N; j++) if (j != i) {
            w += sum[i][j] + 0.5;                  /* a half-point prior keeps 100% finite */
            d += (games + 1.0) / (s[i] + s[j]);
        }
        ns[i] = w / d;
    }
    for (int i = 0; i < N; i++) s[i] = ns[i] / ns[0];
}
```
printed as `400 * log10(s[i])` (`uttt_ladder.c:163`).
This is a genuinely different Elo formula from foolish's pairwise incremental update (`main_elo.c:71-73`, logistic expected score plus a K-factor update applied game by game) - Bradley-Terry MM converges to a maximum-likelihood fit over the whole table at once, foolish's is an online rating that depends on match order.
Both compute "an Elo number" and neither cites the other; this is the same drift the mean/stderr and proportion-interval sections above document, one layer up, and it means the claim "only foolish has Elo" is false - two products independently invented two different Elo estimators, and that is itself evidence for, not against, a later shared Elo module (see Ranked list, corrected below).
`uttt_ladder.c` reuses `uttt_arena.c`'s `seed_of` shape but reimplements it locally with an added `LADDER_SEED`-derived salt (`uttt_ladder.c:66-69`), and reimplements the fork/pipe worker fan-out independently rather than sharing it with `uttt_arena.c` in the same directory - within-product duplication exactly like foolish's, confirmed here rather than assumed.

### Within-product duplication, found along the way

`foolish/c/src/main_elo.c:53-66` re-declares its own copy of the strategy dispatch switch, a subset of `foolish/c/src/main_eval.c:36-66`'s `dispatch_choose` - two hand-written copies of the same dispatch inside one product, before any cross-product question is even asked.
This is not itself a shared-consolidation target (STRAT_* is a foolish-only enum, out of scope for `shared/`), but it is evidence that even foolish has not fully deduplicated its own three arena tools, which bears on how much appetite there is for maintaining a fourth, shared abstraction layer on top.

### Parallelism strategy differs for real reasons, not by accident

Foolish uses OpenMP (`#pragma omp parallel for`, `main_eval.c:477-478,570`) because its solver's transposition table is process-shared state that must warm up once and then must NOT carry across a `--control` pair (comment, `main_eval.c:88-93`, "letting it persist ACROSS GAMES couples the two games... measured as 5/200 phantom divergences").
Chuiniu uses `pthread` (`cn_arena.c:25,148-151`) for the same shared-memory reason, over its belief-model config structs.
Pickemup and uttt both use `fork()` + a pipe (`pk_arena.c:32-33,197-227`; `uttt_arena.c:30-31,126-181`) specifically because their bots keep state in mutable globals that must NOT be shared across workers - uttt's own comment states this outright: "the bots keep state in globals - the weights, the solver's table, the tree pool - and fork gives each worker its own copy of all of it for nothing... do not reach for threads here later" (`uttt_arena.c:16-19`).
A shared parallel-worker harness would have to accommodate both "must share" and "must not share" as first-class cases, which is a second reason (beyond the loop-shape argument above) to leave the loop and its parallelism out of any lift.

### Risk to foolish's regression baselines

`foolish/c/bench_results/elo_arena.txt` is `main_elo.c`'s literal output - the round-robin ELO ranking (cordite, handwritten, gunpowder, blackpowder, robusta, espresso, firecracker, random) this repo's own memory calls load-bearing for bot-strength decisions.
`main_eval.c`'s paired-control path is the tool that produced the GRPO-vs-SFT regression numbers (mean finish position deltas with standard errors) referenced as a regression baseline.
None of the lift proposed here touches `game_set_seed`, `dispatch_choose`, `play_one`, or the OpenMP reduction - it only replaces the final `var_d`/`se_d` arithmetic at `main_eval.c:509-511` with a call to a shared, behavior-identical function, and only if that call is proven byte-identical first (proof command below).
`main_elo.c`'s ELO formula (`elo_change`, `main_elo.c:71-73`) is a different statistic (round-robin, sequential rating updates) from the per-pair win-rate/mean+CI family the other four products compute, and is explicitly excluded from this lift (see Ranked list).

### Proof command per product (arena output must be byte-identical, same seed, before and after)

- foolish: `make -C foolish/c build/cnitro_eval && ./foolish/c/build/cnitro_eval --strategy=cordite --opp=espresso --from=1 --to=200 > /tmp/foolish_before.txt` (the `build/cnitro_eval` target is `foolish/c/Makefile:116`; there is no phony `cnitro_eval` alias, `foolish/c/Makefile:88`'s `.PHONY` list does not include one; the flag names are confirmed against `get_arg(argc, argv, "strategy"/"opp"/"from"/"to", ...)` at `foolish/c/src/main_eval.c:335,339,524-525`), repeat after the lift into `/tmp/foolish_after.txt`, `diff /tmp/foolish_before.txt /tmp/foolish_after.txt`.
- pickemup: `make -C pickemup/c arena ARENA_GAMES=400 ARENA_JOBS=1 > /tmp/pk_before.txt` (per `pickemup/c/Makefile:225-231`), repeat after, diff. `ARENA_JOBS=1` avoids process-scheduling nondeterminism in the diff, since the stats math is the only thing changing.
- chuiniu: `make -C chuiniu/c/bot arena ARENA_GAMES=400 ARENA_SEED=1 > /tmp/cn_before.txt` (the arena lives in `chuiniu/c/bot/Makefile`, not `chuiniu/c/Makefile`; target and overridable `?=` variables at `chuiniu/c/bot/Makefile:19-20,45-47`), repeat, diff.
- tallybones: `make -C tallybones/c/bot build/tb_solve && ./tallybones/c/bot/build/tb_solve play 2000 1 > /tmp/tb_before.txt` (target and `play N [SEED]` syntax confirmed at `tallybones/c/bot/Makefile:27,36-37` and `tallybones/c/bot/tb_solve.c:32-39`), repeat, diff. (This proof only applies if a future lift touches `tb_solve.c`'s own mean/stderr math; today's recommendation includes it as a consumer of the shared stderr function.)
- uttt: `make -C uttt/c build/uttt_arena && ./uttt/c/build/uttt_arena 200 200 1 > /tmp/uttt_before.txt` (build rule at `uttt/c/Makefile:43-44`, the `arena` convenience target at `:91-92` runs the same binary with `200 200`), repeat, diff. (uttt currently prints no interval; adopting the shared module changes its OUTPUT by adding one, which is the one product where "byte-identical" does not apply - the proof there is "adds a Wilson interval column, changes nothing else," verified by diffing everything except the new column. If `uttt_ladder.c` is also migrated to a shared Elo module, per its own build rule at `uttt/c/Makefile:221-224` and run as `./uttt/c/build/uttt_ladder 200 8`, the same non-identical-output caveat does not apply there - Bradley-Terry MM is deterministic for a given seed and game count regardless of which formula computes it, so that proof CAN require a byte-identical diff.)

---

## Ranked list

**Lift now:**
- `shared/c` pure-math statistics functions: a Bessel-corrected mean/variance/stderr function and a Wilson score interval function, with no game type in their signature (plain `double`/`uint64_t` in, `double` out).
  Consumers: foolish's `main_eval.c:509-511` (paired-delta SE), chuiniu's `cn_arena.c:96-103,177-178` (replacing its own Wilson implementation with the shared one, and picking up the Bessel correction it currently lacks for dice-lost SE, a named and deliberate behavior change, not silent drift), pickemup's `pk_arena.c:156-163` (switching from its normal approximation to the shared Wilson interval, a deliberate improvement pickemup's own comment already gestures at), tallybones' `tb_solve.c:55-59` and `tb_bot_test.c:329-333` (already Bessel-corrected, becomes a pure dedup), and uttt's `uttt_arena.c:183-202` (gains a confidence interval it does not have today).
  Proof: the byte-diff commands above for foolish/pickemup/chuiniu/tallybones (unchanged output required); for uttt, a diff of everything except the new interval column, plus a manual check that the interval's numbers match a hand computation for one row.
- A shared per-game seed-hash helper (`shared/c`, pure function, `(uint64_t identity_bits, uint64_t index) -> byte[32]` or similar), replacing the near-identical splitmix64 implementations at `pickemup/c/tools/pk_arena.c:74-85`, `uttt/c/tests/uttt_arena.c:39-43`, and chuiniu's `cn_splitmix` call site at `cn_arena.c:52-57`.
  This is pure hashing with zero game coupling, the lowest-risk item on this list.

**Lift later:**
- A shared parallel-worker harness for the fork+pipe half of this (pickemup and uttt only - their reason for forking, not sharing, is identical: mutable bot-global state, `uttt_arena.c:16-19`).
  Do this only after the statistics lift has shipped and proven that a `shared/c` file touching an arena's build is safe in practice; chuiniu (pthread) and foolish (OpenMP) have real, different reasons not to join this one and should stay out of it.
- Standardizing CLI flag style (`--key=value` vs. positional `argv`) across the five arenas.
  Cosmetic and low urgency; each product's existing CLI shape is already baked into scripts and muscle memory (`make -C X/c arena` invocations, CI if any), and unifying it is a breaking change to those call sites for no behavior change, so it should trail well behind the statistics lift rather than ride with it.
- Folding the Elo computations into a shared module, as a second, separate lift once the win-rate/mean statistics module has shipped and been trusted for a cycle.
  Two products have independently invented Elo math today, not one: foolish's `main_elo.c` (`elo_change`, `main_elo.c:71-73`, a sequential pairwise logistic update with a K-factor, order-dependent) and uttt's `uttt_ladder.c` (Bradley-Terry by MM iteration over the whole round-robin table at once, `uttt_ladder.c:143-154`, order-independent).
  These are two different estimators for the same underlying question, not one statistic reused twice, so this lift is a genuine design decision (pick one estimator, or expose both as named options) rather than a mechanical dedup; do not conflate it with the mean/stderr and proportion-interval lift above, which has no such choice to make.

**Do not lift:**
- A struct-of-function-pointers game-kernel interface (Part A) - fights the flat-export, kernel-owns-the-shape pattern this repo already chose and gated with tests, and the five "new game" signatures prove the abstraction would have to erase real per-game differences to exist at all.
- A struct-of-function-pointers "policy" interface for the arena (Part B) - none of the five products dispatch a policy through a function pointer today, all five use a closed enum plus a switch or an `if`/config-struct, and inventing the vtable would be new architecture with no existing caller asking for it.
- A shared game loop / "step" abstraction - turn structure, win condition, and information-hiding diverge on every axis across the five games, and even foolish alone has three non-identical loop shapes (`main_eval.c`'s three CLI paths plus `main_elo.c`), so there is no single shape to extract yet, let alone one that spans five products.
- Forcing tallybones into the "two policies, win rate" mold - it is a solitaire game with a solved-optimal policy and no opponent; it does not have this shape today, and giving it one is a product decision about adding a competitive mode, not a code-reuse question this pass should answer.
- The parallelism mechanism itself (fork vs. pthread vs. OpenMP) as a single shared abstraction - three different, real constraints (must-not-share bot globals, must-share belief config, must-not-persist-across-pairs solver TT) currently drive three different choices, and unifying the mechanism would have to satisfy all three at once.
