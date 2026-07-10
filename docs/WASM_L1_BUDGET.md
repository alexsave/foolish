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
| `bots.wasm`   | 5.13 MiB | **~1.375 MiB** | ~3.7× (−73%) | **yes** — see below |

`bots.wasm` is the one that grows. Its **initial** memory dropped 64 → 18 pages
(4 MiB → 1.13 MiB) from the shared buffer caps. On first play its Monte-Carlo
endgame solver then bump-allocates a transposition table (+2 slack pages) and
stays flat there. That table was 1 MiB (`CD_TT_BITS=16`); the divergence study
below shrank it to 128 KiB (`CD_TT_BITS=13`, 8,192 × 16 B), and a 2-way
associativity change (`CD_TT_2WAY`, candidate C4 in
`SOLVER_TT_WORKING_SET_PLAN.md`) then let it drop again to **64 KiB**
(`CD_TT_BITS=12`, 4,096 × 16 B = exactly one wasm page, L1d-resident) at
strength ≥ the TT13 table. The runtime peak went `18 + 18 = 36 pages`
(2.25 MiB) → `18 + 4 = 22 pages` (~1.375 MiB) → `18 + 3 = 21 pages` (~1.31 MiB).
The CI `metrics` job reports the canonical bots-only figure. That footprint —
not the 1.13 MiB static floor — is the honest number.

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
CD_RACE_C=75`) with a candidate table and with a reference table, and compare a
per-game hash of the bot's move sequence (`GAME_SIG`, `main_eval.c`).

**There is no clean cliff-to-zero — there is a floor.** Measured against an
effectively-infinite table (`TT22`), octogen diverges at a residual rate of
~3–6 per 10,000 games *even at `TT16`–`TT19`* — i.e. today's 1 MiB production
table already differs from an infinite one at that rate. This is octogen's
**inherent** table-size sensitivity: it persists the table across a whole game,
and rare huge-endgame games collide at any practical size. Below that floor a
collision **knee** appears at ~`TT10`–`TT11`; from `TT11` up through `TT19` the
rate is flat at the floor (measured, all player counts, tens of thousands of
games). cordite/fulminate/semtex diverge one bit lower still (knee ~`TT9`) —
octogen is the binding bot.

So the infinite table is the wrong yardstick; **`TT16` — what ships today — is
the decision-relevant baseline.** Measured directly against `TT16`, octogen at
`CD_TT_BITS=13` diverges in **0 of 1,720 games** (pc 2/4/7, `BASE=16`). TT13
sits at octogen's inherent floor, 2–3 bits above the collision knee, and is
statistically **indistinguishable from the production table** — the shrink does
not change how any bot plays relative to today. That is the shipped value:
table 1 MiB → 128 KiB, runtime peak 36 → 22 pages. Also confirmed by
`bot_parity` against the TS oracle (7/7). (A further 2-way associativity change,
`CD_TT_2WAY`, then took the shipped table to **64 KiB** — `CD_TT_BITS=12`, one
wasm page — at strength ≥ TT13; see "The 2-way follow-on" below.)

(An earlier draft of this doc claimed `p ≈ 2.5e-5` from a `p(M) ≈ C/M`
extrapolation off a noisy 1-in-600 anchor against the infinite-table baseline.
The scaled runs showed that baseline overstates the cost — the real vs-infinite
rate at TT13 is the ~1e-3 floor, and the vs-production rate is ~0. The
extrapolation is dropped in favor of the direct `BASE=16` measurement.)

**The working-set model — an interactive view.** `docs/tt-divergence.html`
(regenerate with `tools/tt_divergence_viz/generate.sh`) plots all of this: a
divergence-vs-table-size curve per bot and player count, with the raw measured
points overlaid and a log-scale toggle. The birthday argument has a cleaner,
baseline-free restatement there. Instrument the solver to count the distinct
keys it inserts per game — its working set `W` (`-DCD_TT_STATS`). A direct-mapped
table of `M = 2^bits` slots can only force an eviction cascade once `W > M`, so
the per-game divergence rate is bounded by the survival function of the working
set:

    P(diverge | table = 2^bits)  ≤  P(W > 2^bits)

This is an upper bound, not an identity — an overflow only flips a move if the
evicted entry is reused before the node budget runs out — but it needs **no
reference table**: `W` is a property of the game and the solver, measurable at
every size. It also gives per-game certainty: any game whose whole `W` fits under
`M` is bit-identical to an infinite table. Measured max `W` climbs as the endgame
deepens (fewer players → longer solves), and only the deepest two-player games
push past a few thousand keys — every shipped player count from 3 up stays under
TT13's 8,192. That is why TT13 matches production: the working-set tail is
exhausted well before the table fills. And it is why *no* finite table is provably
perfect — the octogen sweep's `0 / 35,000` at TT22 only bounds the true rate below
`3/35,000 ≈ 8.6e-5` (rule of three), it does not prove zero.

### The 2-way follow-on (`CD_TT_2WAY`, `CD_TT_BITS=12`)

The birthday model above says the cost is *conflict* misses on the reused subset —
two live keys colliding in a direct-mapped table. Set associativity attacks exactly
that: make each aligned slot pair a 2-entry bucket (both entries share one 64-byte
cache line), probe both halves, and on store keep the shallower-ply entry (larger
subtree = costlier to recompute) while always storing the new one. This roughly
halves conflict misses — worth ≈ **+1 effective bit** — so a 64 KiB `TT12` table
behaves like the 128 KiB `TT13` above. Full design + safety analysis: candidate
**C4** in `SOLVER_TT_WORKING_SET_PLAN.md`.

It is **value-safe** (a hit returns exactly what a recompute would; behavior changes
only through the halved collision channel), so it climbs the same measurement ladder:

- `CD_TT_2WAY`@`TT22` is **SIG-identical** to std@`TT22` over 200 mixed seeds (100
  handwritten + 100 espresso) — no value drift.
- On the tricky panel it **fixes** the two known TT13 win→loss outcome flips — seeds
  **720958** and **700910** now win at TT13/TT12/TT11 where the direct-mapped table
  lost — and plays the infinite-table move on the perennial diverger 500459 at
  TT12/TT13. Zero outcome regressions on the panel.
- At scale, `TT12`+2WAY vs `TT22` over **3,000 espresso games**: **0 outcome flips**
  (0.000%) and 0.10% move-divergence — both *below* direct-mapped TT13's 0.14% /
  0.04% floor. Half the bytes, strictly better fidelity.

Shipped value: runtime peak 22 → 21 pages (table 128 → 64 KiB, one wasm page,
L1d-resident), strength ≥ the TT13 table it replaces. Native builds keep the large
direct-mapped table; the flag is wasm-only.

### The 8-byte-entry follow-on (`CD_TT_PACK8`, candidate C6)

The entry was 16 bytes (`{key:64, value:16, depth:8, valid:8}`) but only ~52 bits
of it carry information. `CD_TT_PACK8` packs it to **8 bytes**: a 40-bit key TAG
(the slot index supplies the low `CD_TT_BITS`, for a 40+bits effective key), a
12-bit signed value (`|v| ≤ 1000`), a 6-bit depth (`≤ 48`), and valid + 2-bit bound
flags. So the shipped `TT12 + 2WAY` table halves again, **64 KiB → 32 KiB**, at the
same 4,096 slots — SIG-identical to the 16-byte table on the full tricky panel and
at TT22. The one new cost is a tag alias: two positions sharing a slot *and* a
40-bit tag return a silently-wrong value, at ~1e-5/game — an order of magnitude
below octogen's ~4e-4 inherent table-size floor, so it is invisible against the
noise the table already carries. A uniform `CD_TT_KEYTAG` macro (identity when the
flag is off) keeps the full-key path byte-identical.

Net solver-table trajectory: **1 MiB (TT16) → 128 KiB (TT13) → 64 KiB (TT12+2WAY) →
32 KiB (TT12+2WAY+PACK8)** — a 32× shrink from the historical table, strength
unchanged, now a quarter of a wasm page.

**Next round:** with the TT down to 32 KiB it is no longer a dominant block —
the shadow stack (256 KiB), `solve_ws` (272 KiB), and `g_moves` (232 KiB) now
lead. The measured map and an executable candidate-by-candidate plan for the
next shrink round live in `docs/BOTS_WASM_MEMORY_PLAN.md`.

The remaining static core is the Monte-Carlo solver scratch (`solve_ws` 272 KiB,
`solve_child_scratch` 55 KiB, the world/trial/diff slots) — a per-search working
set inherently larger than L1, **designed** around bitboard `SimState`s that *are*
L1-resident during the hot rollout loop. Fitting bots in L1 is not a memory-layout
problem; it's a different solver.

Reproduce the floor vs infinite: `cnitro/tools/tt_divergence.sh octogen handwritten 2,4,6,8 4000 99 16 13 11 9 7`.
Reproduce vs production: `BASE=16 cnitro/tools/tt_divergence.sh octogen handwritten 4 800 99001 13` (expect 0).
Tip: keep runs focused (one player count, few sizes) — octogen at pc2 is the slow one.

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
  five shipped solver bots: TT13 vs today's production TT16 diverges in 0/1720
  octogen games (the binding bot), i.e. indistinguishable from the shipped table;
  the collision knee is ~TT10-11, well below 13. `bot_parity` (7/7) confirms the
  TT13 wasm still matches the TS oracle; `test:mem` (4/4) confirms bounded, flat
  memory at the smaller table.
- **2-way table (`CD_TT_BITS=12 -DCD_TT_2WAY`)** — value-safe by construction and
  measured so: SIG-identical to std at TT22 (200 seeds); on the tricky panel it
  fixes the TT13 win→loss flips (720958, 700910) with no regressions; at scale,
  TT12+2WAY vs TT22 over 3,000 espresso games gives 0 outcome flips and 0.10%
  move-divergence, below TT13's floor. (`tools/tt_divergence_viz/outcome_pair.sh`
  with `EXTRA=-DCD_TT_2WAY CAND=12`; full ladder in `SOLVER_TT_WORKING_SET_PLAN.md`.)

(The research-only `solver_difftest` mismatches, but it mismatches identically on
clean `main` and touches neither replay nor the wasm buffers changed here. The
native/arena builds keep `CD_TT_BITS=16` — the shrink is wasm-only.)
