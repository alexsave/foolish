# WASM linear-memory L1 budget

Every static buffer in the three shipped kernels (`rules.wasm`, `guards.wasm`,
`bots.wasm`) is sized from a measured maximum, not a round guess. The goal that
drove this pass: **fit the client-guards module's entire linear memory in an L1
data cache** — and shrink the two server modules as far as their real working
set allows on the way there.

## Why 64 KiB is the target

WebAssembly linear memory is allocated in **64 KiB pages**, so one page is the
hard floor. That floor happens to line up with hardware:

| core (edge/runtime)            | L1d per core |
| ------------------------------ | ------------ |
| AWS Graviton / Arm Neoverse    | **64 KiB**   |
| Apple M-series (local dev)     | 128 KiB      |
| x86-64 (Zen 4 / recent Intel)  | 32–48 KiB    |

`guards.wasm` runs in the browser main thread on whatever the player has; a
one-page module is L1-resident on Graviton and M-series and is at worst a
2-page (still L2-trivial) footprint on x86. The two server modules can't reach
one page (they carry the move enumerator / bot scratch), but there was a large
amount of dead over-provisioning to reclaim.

## Result

Two numbers matter and they differ for one module. **Initial** is the memory
the module declares at link time (`memory.buffer.byteLength` at instantiation).
**Peak** is the high-water mark during real gameplay — and it's what the edge
external-memory budget is charged for, and what the CI `metrics` job reports.
For a module that never calls `memory.grow`, peak == initial.

| module        | before (peak) | after (peak) | reduction | grows at runtime? |
| ------------- | ------------- | ------------ | --------- | ----------------- |
| `guards.wasm` | 256 KiB | **64 KiB · 1 page** | 4× | no — pinned, 0 `memory.grow` |
| `rules.wasm`  | 3.31 MiB | **320 KiB · 5 pages** | 10.6× | no — all-static, 0 `memory.grow` |
| `bots.wasm`   | 5.13 MiB | **~1.5 MiB** | ~3.4× (−71%) | **yes** — see below |

`bots.wasm` is the one that grows. Its **initial** memory dropped 64 → 18 pages
(4 MiB → 1.13 MiB) from the shared buffer caps. On first play its Monte-Carlo
endgame solver then bump-allocates a transposition table (+2 slack pages) and
stays flat there. That table was 1 MiB (`CD_TT_BITS=16`); the divergence study
below shrank it to 256 KiB (`CD_TT_BITS=13`), so the runtime peak went
`18 + 18 = 36 pages` (2.25 MiB) → `18 + 6 = 24 pages` (~1.5 MiB). Measured
end-to-end across all MC families (rules + bots summed): **2.56 MiB → 1.69 MiB**;
the CI `metrics` job reports the canonical bots-only figure. 1.5 MiB is the
honest footprint — the 1.13 MiB static floor is not the whole story.

`guards.wasm` is pinned to exactly one page at link time
(`--initial-memory=65536 --max-memory=65536`): if any buffer ever grows past
the page, **wasm-ld refuses to link** rather than silently regressing, and the
module can never `memory.grow` (it has no allocator — verified: no `memory.grow`
opcode in the binary; `memory.grow(1)` traps at runtime).

## How the maxima were measured

`cnitro/tests/l1_measure.c` plays **~63,000 real engine games** — every player
count 2–8, both the random (degenerate-game) and handwritten (realistic)
strategies, multiple seeds — and records the peak of every quantity that sizes
a buffer. Built with the WASM cap set plus deliberately oversized headroom
(`MAX_LOGS=2048 MAX_LEGAL_MOVES=16384`) so the observations are true maxima, not
clamps. The replay coder peaks come from `-DREPLAY_STATS` counters compiled into
`replay.c` (zero cost when the flag is absent, which is every production build).

```
cd cnitro
gcc -O2 -Isrc -DMAX_LOG_PAIRS=64 -DMAX_BATTLES=64 -DMAX_LOGS=2048 \
    -DMAX_LEGAL_MOVES=16384 -DMAX_MOVE_CARDS=52 -DREPLAY_STATS \
    src/game.c src/deal_rng.c src/legal.c src/replay.c src/view.c \
    src/random_strategy.c src/handwritten_strategy.c tests/l1_measure.c \
    -o l1_measure -lm
./l1_measure 1500 1              # games-per-config, seed
```

## The budget, buffer by buffer

Observed maxima over the 63K-game sweep, the cap each build ships, the margin,
and — critically — what happens on overflow. **No cap here can corrupt memory:**
every overflow is a bounds-checked clean error or a dropped-but-safe animation
frame.

| buffer (define)                     | observed max | shipped cap | margin | overflow behavior |
| ----------------------------------- | -----------: | ----------: | -----: | ----------------- |
| replay coder choices (`REPLAY_REC_CAP`) | 692      | 4096        | 5.9×   | clean `REPLAY_ECAP`, replay just isn't saved |
| replay bignum limbs (`REPLAY_BN_CAP`)   | 51       | 2688 *(derived)* | 52× | clean `REPLAY_ECAP` |
| replay io bytes (`WASM_REPLAY_IO_CAP`)  | 3,198 in / 204 blob | 32,768 | 10×  | rejected before any work (`REPLAY_ECAP`) |
| animation snapshots (`MAX_SNAPS`)   | 12           | 24          | 1.8× *(analytic)* | ring drops extra frames (visual only) |
| io buffer, rules (`WASM_IO_CAP`)    | 16,898 (log export) | 24,576 | 1.45× | log export is the widest unchunked write; bounds-checked |
| io buffer, guards (`IO_CAP`)        | 8,450 (log export) | 8,704 | 1.03× | export not in the linker allow-list; kept clear defensively |
| shadow stack, rules                 | ~45 KiB (cover enum) | 64 KiB | 1.4× | `--stack-first` → loud trap, not corruption |
| shadow stack, guards                | <2 KiB (Game clone in BSS) | 16 KiB | >5× | `--stack-first` → loud trap |

### `REPLAY_BN_CAP` is derived, not measured

`replay.c` sets `BN_CAP = ceil(21/32 · REC_CAP)` unless overridden — each
recorded rANS choice multiplies the working integer by `M < 2²¹`, so the integer
can't need more limbs than that. At `REC_CAP=4096` that's 2,688 limbs (10.5 KiB),
which also clears the independent decode floor: a maximum
`REPLAY_MAX_INT_BYTES = 8192`-byte input integer needs 2,049 limbs.

### The `MAX_SNAPS` margin is deliberate

Snapshot-ring overflow is the only cap here whose failure is *silent* (dropped
animation frames, not an error), so it gets the most conservative treatment. The
analytic worst window is a round transition: `MAGIC + TRASH + defender-draw +
≤num_players per-player refill draws = num_players + 3 = 11` at 8 players (refill
snaps fire once **per player batch**, not per card; no OUT/DEFENDER_CHANGE snap
in that path). The deal window is `num_players + 3 = 11` too. Measured worst 12.
Shipping 24 is ~1.8× the ceiling at zero page cost.

### Two structural moves, not just smaller numbers

- **`guards.wasm` validate clone → BSS.** `validate_run` cloned the live `Game`
  (~9.6 KiB) onto the C stack for every UI gate. Moving it to a static
  (`g_validate_tmp`) is what lets the module link with a 16 KiB stack and fit
  one page. Safe: single-threaded, gates never nest.
- **`guards.wasm` snapshot ring → `MAX_SNAPS=1`.** The shipped guards module
  exports no snapshot readers (see `WASM_GUARDS_EXPORTS` in the Makefile), so the
  48-slot ring was 55.7 KiB of write-only memory. Reviving snapshots means
  re-adding the exports *and* raising the cap back.

## Where `rules.wasm`'s 5 pages go (≈260 KiB static)

The remaining weight is real working set, not slack: `g_moves` (LegalMoves menu,
59 KiB at `MAX_LEGAL_MOVES=1024`), `g_rec` (48 KiB), `g_replay_io` (32 KiB),
`g_snaps` (28 KiB), `g_io` (24 KiB), `g_comb` (the 52×52 binomial table, 21 KiB),
`g_game` (18 KiB). The replay codec's tables (`g_comb`, `g_opts`, `g_weights`)
are the next target if 4 pages is ever wanted, but they're wire-format-frozen and
were left untouched here.

## `bots.wasm` floor and the transposition-table study

`bots.wasm`'s **static** footprint dropped 4 MiB → 1.13 MiB from the shared
replay/snapshot caps. Its runtime peak is dominated by one bump allocation: the
cordite endgame solver's **transposition table** (`cordite_sim.c`), 1 MiB at the
historical `CD_TT_BITS=16` (65,536 × 16 B). This is the one buffer whose size is
a **bot-strength knob**, not free memory: the table caches *exact* endgame
values, but the solver is **node-budget-limited** (`SimSolver.budget`), so a
smaller table → more recomputation → the budget exhausts sooner → the bot can
pick a *different* move. So it can't just be shrunk; it has to be measured.

**Mechanism.** The table is **direct-mapped** (`tt[key & MASK]`), so divergence
from a collision-free table happens when two *reused* keys hash to the same slot
— a birthday collision on the reused subset, giving `p(divergence) ≈ C/M` (halves
per bit of table size). Crucially it's the *reused* subset, not total occupancy:
the working set reaches `I ≈ 1305` distinct keys per window (measured collision-
free, `-DCD_TT_STATS`), yet a 512-slot table (`TT9`) holds it with thousands of
collisions and still plays **bit-identically** — because almost none of those
keys are ever probed again, so evicting them is harmless.

**Measurement** (`tools/tt_divergence.sh`): for every shipped solver bot, play
identical seeds under the exact production env (`CD_BUDGET=prod CD_RACE=1
CD_RACE_C=75`) with a candidate table and with a collision-free `TT22`, and
compare a per-game hash of the bot's move sequence (`GAME_SIG`, `main_eval.c`).
The per-game divergence rate:

| bot | table persistence | divergence cliff | clean at |
| --- | --- | --- | --- |
| **octogen** | per **game** | **TT7** (128 entries) | **TT8+** |
| semtex | per game | TT6 | TT7+ |
| cordite / fulminate | per solve | TT6 | TT7+ |

**octogen is the binding constraint** — it persists the table across a whole
game, so its window is larger and it diverges one bit sooner than the rest. Every
shipped solver bot is **bit-identical to a collision-free table at `TT8` and
above** (0 divergences in 2,400 games each). With `p(M) ≈ C/M`, `CD_TT_BITS=13`
sits **6 bits / 64× above octogen's cliff** — extrapolated `p(a game diverges)
≈ 2.5e-5`, i.e. ≥ 99.997 % of games play identically to an infinite table. That
is the shipped value: table 1 MiB → 256 KiB, runtime peak 36 → 24 pages. It was
also independently confirmed bit-identical in the full-histogram sweep and by
`bot_parity` against the TS oracle.

The remaining static core is the Monte-Carlo solver scratch (`solve_ws` 272 KiB,
`solve_child_scratch` 55 KiB, the world/trial/diff slots) — a per-search working
set inherently larger than L1, **designed** around bitboard `SimState`s that *are*
L1-resident during the hot rollout loop. Fitting bots in L1 is not a memory-layout
problem; it's a different solver.

Reproduce: `cnitro/tools/tt_divergence.sh octogen handwritten 2,4,6,8 4000 99 12 11 10 9 8 7`.

## Validation

No cap cut shipped without a passing suite behind it:

- **native** — `make tests` (161/161), `sim_difftest` (89K steps, 0 real
  divergences), `apply_difftest` (518K states, 0 mismatches), `replay_difftest`
  rebuilt at the exact wasm caps `-DREPLAY_REC_CAP=4096 -DWASM_REPLAY_IO_CAP=32768`
  (2.3M checks, 0 failed, 0 unexpected capacity rejects on real games).
- **wasm memory** — `npm run test:mem` (4/4): the shrunken `bots.wasm` runs full
  games across all Monte-Carlo bot families with memory bounded and flat.
- **parity / server path** — `client_guards`, `client_rules_parity`,
  `replay_codec` (byte-exact vs the frozen TS oracle at the new caps),
  `awire_codec`, `action_handlers`, `fuzz`, `concurrent_games`, `cover`,
  `client_move_gates` — all green.
- **transposition table (`CD_TT_BITS=13`)** — `tools/tt_divergence.sh` across all
  five shipped solver bots (0 divergences at TT8+; octogen the binding bot at
  TT7); `bot_parity` (7/7) confirms the TT13 wasm still matches the TS oracle;
  `test:mem` (4/4) confirms bounded, flat memory at the smaller table.

(The research-only `solver_difftest` mismatches, but it mismatches identically on
clean `main` and touches neither replay nor the wasm buffers changed here. The
native/arena builds keep `CD_TT_BITS=16` — the shrink is wasm-only.)
