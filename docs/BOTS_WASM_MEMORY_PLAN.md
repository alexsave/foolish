# bots.wasm total-memory shrink — execution handoff

**Mission.** Shrink `bots.wasm`'s TOTAL linear memory. The transposition table is
done (1 MiB → 32 KiB, see `SOLVER_TT_WORKING_SET_PLAN.md`); it is no longer a
dominant block. The remaining giants are the **shadow stack (256 KiB)**, the
**solver workspace `solve_ws` (272 KiB)**, and the **move buffer `g_moves`
(232 KiB)** — plus a tail of loose caps the bots module kept while rules.wasm
was tightened (rules went 53 pages → 5 with the same method).

**Honest target.** Static memory today is **18 pages (1,152 KiB)**. The
main-line candidates below (M1/M7 + M2 + M4 + M8) remove ~400–410 KiB →
**12 pages**; with the gated M3 → **11**; the stretch M5 → ~10. True
L1-residency of the *whole module* is not reachable —
`solve_ws`+`solve_child_scratch` (~327 KiB) are the MC solver's inherent
working set — but the *hot* set (SimState rollouts, the 32 KiB TT, the active
stack frames) already fits L1. Sell the result as pages-and-cache-pressure,
not "fits in L1".

**Non-goal.** Zero behavior change, always. Any candidate that alters which
move a bot picks in ANY game is dead unless separately validated through the
outcome ladder (see V-gates). When in doubt, measure; when still in doubt, skip.

---

## 0. Method (read this twice)

This codebase has already been through two disciplined shrink rounds. The
method is established and documented in `cnitro/Makefile:130-181` and
`docs/WASM_L1_BUDGET.md`:

1. **Measure the real requirement** with a harness over tens of thousands of
   games (`cnitro/tests/l1_measure.c` is the existing tool; build rule at
   `cnitro/Makefile:106-110` — it compiles with huge caps and reports observed
   peaks).
2. **Cap at ~1.4–2× the measured/analytic worst case**, stated inline in the
   Makefile comment next to the flag.
3. **Overflow must be a CLEAN error** (trap, clamp, or drop) — never
   corruption. `--stack-first` makes stack overflow a loud trap.
4. **Ship only behind green gates** (§V below).

Every candidate below follows that shape: measure → cap → gate.

### Re-measuring the memory map

```sh
cd cnitro && make build/bots.wasm
# per-symbol data/BSS sizes (these + stack + heap = linear memory)
llvm-nm --print-size --size-sort --defined-only build/botobj/*.o \
  | awk '$3 ~ /[dDbB]/ {print $2, $4}' | sort -r | head -30
# page count + full layout: regenerate the anatomy page
bash tools/wasm_anatomy/generate.sh && open ../docs/wasm-anatomy.html  # Memory layout tab
```

### The measured map today (post-TT-shrink, Jul 2026)

| block | bytes | KiB | file:line | sized by |
|---|---|---|---|---|
| shadow stack | 262,144 | 256.0 | `Makefile:189` `-Wl,-z,stack-size=262144` | solver recursion depth |
| `solve_ws` (union) | 278,592 | 272.1 | `cordite_sim.c:2214` / `cordite_sim.h:145-156` | `48 × (4 + 100×58)` SolveMoves arm |
| `g_moves` | 237,572 | 232.0 | `wasm_api.c:79`, `legal.h:37-40` | `MAX_LEGAL_MOVES=4096 × 58 B` |
| `g_io` | 73,728 | 72.0 | `wasm_api.c:53` `WASM_IO_CAP` default | 512-log session export |
| `g_game` | 68,744 | 67.1 | `wasm_api.c:68`, `game.h` | `MAX_LOGS=512 × 132 B` GameLog |
| `solve_child_scratch` | 56,064 | 54.8 | `cordite_sim.c:2203` | `48 × 1,168` Game prefix |
| `g_rec` | 49,152 | 48.0 | `replay.c:132` | `REPLAY_REC_CAP=4096` (measured 6.1×) |
| `g_replay_io` | 32,768 | 32.0 | `wasm_api.c:442` | measured >10× margin |
| `g_snaps` | 27,840 | 27.2 | `wasm_api.c:75`, `-DMAX_SNAPS=24` | measured worst 12, analytic 11 |
| `g_comb` | 21,632 | 21.1 | `replay.c:201` | 52×52 binomial, wire-frozen |
| world/trial/diff slots | 19,344 | 18.9 | `cordite_sim.c:2257` | `WORLD_LOG_CAP=40` (done) |
| `g_bn` | 10,756 | 10.5 | replay bignum | derives from REC_CAP |
| `g_weights`+`g_opts` | 14,336 | 14.0 | replay codec tables | wire-frozen |
| `forced_loss_flags` | 4,096 | 4.0 | `cordite_sim.c:2258` | `MAX_LEGAL_MOVES` bools |
| misc (<4 KiB each) | ~8,000 | ~7.8 | — | — |
| **data+BSS total** | **~882 K** | **~861** | | |
| **linear memory** | | **1,152 (18 pages)** | + heap slack; TT bumps +32 KiB at runtime | |

Key struct sizes: `LegalMove` = 58 B (`type,n_cards,cards[28],attack_cards[28]`,
`legal.h:26-31` at the wasm `-DMAX_MOVE_CARDS=28`); `GameLog` = 132 B
(`4 + 64 LogPairs × 2`, `game.h:63-74` at `-DMAX_LOG_PAIRS=64`);
Game log-free prefix = 1,168 B (`offsetof(Game, logs)` rounded to 16).

---

## 1. Interface constraints — memorize before touching anything

These are the walls. Violating any of them breaks production in ways the C
test suite alone won't catch.

**W1 — bots.wasm IS the engine in bot games.** When bots.wasm loads, it *adopts
the engine slot* (`engine.ts:181 __adoptEngine`) and serves the ENTIRE rules
API — actions, views, snapshots, log import/export, **and the replay codec** —
on one instance (this enables the resident-state fast path). Consequence: you
cannot strip the replay codec, snaps, or any rules-API buffer out of bots.wasm
without an architectural routing change (see M6, "blocked").

**W2 — the move-index contract.** The bot's chosen move is returned as an
INDEX into the LegalMove list; server and bot must enumerate the identical
list (`bots.ts` header comment, "maps 1:1"). rules.wasm enumerates the menu at
`MAX_LEGAL_MOVES=1024`; bots at 4096. Harmonizing bots down to 1024 makes the
kernels MORE consistent, not less — but only if no real state enumerates
>1024 moves under *bot* strategies (M2 measures exactly this).

**W3 — TS mirrors.** `MAX_KERNEL_LOGS=512` in `bots.ts:105` mirrors
`MAX_LOGS`. Any change to `MAX_LOGS` for the bots build must change the TS
mirror **in the same commit**, and you must first read the `bots.ts:121/149`
import loops to understand which end of the log the clamp drops (belief
correctness depends on it).

**W4 — wire formats are frozen.** `state_serialize`, the replay codec
(`g_comb`/`g_opts`/`g_weights` tables, `REPLAY_REC_CAP`-derived `g_bn`), and
the log export format are byte-compared against TS oracles by the parity
suites. Do not touch `replay.c` capacities — they are already measured at
6–10× margins (`Makefile:166-176`).

**W5 — Game layout is load-bearing.** `solve_clone_prefix`, snap slots, and
`SOLVE_CHILD_STRIDE` all use `offsetof(Game, logs)`. Do not reorder Game
fields. Cap changes that shrink arrays *inside* the prefix (battles, hands)
also change the serialized state format — off-limits.

**W6 — native builds keep their caps.** Every change goes in `WASM_FLAGS`
(`Makefile:182-190`) or the bots link rule only — exactly like the TT flags.
If you change the shared `-DMAX_LEGAL_MOVES=4096` string, update the
`filter-out` in `WASM_RULES_FLAGS` (`Makefile:280`) which matches it verbatim.

**W7 — solver-behavior tripwires.** `SOLVE_SCRATCH_MOVES=100` and the
96-move abort (`cordite_sim.h:146-152`, `cordite_sim.c:1258`) define WHICH
nodes the solvers abort — that is bot behavior. `SOLVE_SCRATCH_DEPTH=48` and
`CD_SIM_SOLVE_MAX_MOVES=160` likewise. **Never reduce these.** They are not
memory caps; they are play semantics.

---

## 2. Candidates, in execution order

Do them one at a time, each in its own commit, each through the gates.

### M1 — shadow stack 256 KiB → 128 KiB (target: −128 KiB, low risk)

*Why it's 256:* `Makefile:151-153` — deepest real path is the bitboard
solver's 48-frame recursion at ~1.5 KiB/frame (frame = `SimState` child
~330 B + `SolMove moves[160]` at 18 B each ≈ 2.9 KiB by struct math, ~1.5 KiB
measured after register allocation), so 256 KiB is stated as "~3× worst case".
The rules module already ships a 64 KiB stack at 1.4× its measured worst
(`Makefile:268-273`) — the precedent and the trap mechanism (`--stack-first`)
are proven.

*Measure first (stack canary):* with `--stack-first` the stack occupies
addresses `[0, 262144)` and grows DOWN from 262144; between exported calls no
wasm frames are live. Add a canary pass to the mem harness
(`e2e/mem_harness.mts` loads the module; write a small standalone script or
extend it):

```ts
// after instantiate + one warmup call:
const mem = new Uint8Array(memory.buffer);
mem.fill(0xa5, 64, 262144 - 64);           // paint dead stack
// ... run the heavy corpus (below) via the normal bridge calls ...
let low = 64; while (low < 262144 && mem[low] === 0xa5) low++;
console.log('stack high-water =', 262144 - low, 'bytes');
```

Heavy corpus = the worst known stack drivers, all already scripted:
(a) octogen pc2 tricky-panel seeds (deep endgame solves) — replay via the
bot bridge; (b) `npm run test:mem` games; (c) the fuzz + cover e2e suites
(they drive cover enumeration hardest, per `Makefile:270-272`); (d) an
8-player game (widest states).

*Change:* add a trailing override to the bots link, exactly like rules does
(`Makefile:281` pattern): `-Wl,-z,stack-size=131072` (or 1.5× measured
high-water rounded up to a 64 KiB multiple, whichever is larger). Trailing
`-z stack-size` wins in wasm-ld.

*Overflow story:* `--stack-first` → overflow traps loudly at address 0; the
fuzz/cover/mem suites are the regression net. This is the same deal rules
shipped with.

*Decision tree — the canary number picks between M1 and M7:* there is a real
conflict in the existing evidence. The Makefile comment claims ~1.5 KiB/frame
for the 48-frame solver recursion (≈72 KiB high-water), but the struct math
says ~3.3 KiB/frame (`sim_solve_rec` keeps `SolMove moves[160]` = 2,880 B +
`SimState child` ≈ 330 B as locals, `cordite_sim.c:1470,1504`) ≈ 158 KiB
high-water. The canary settles it:

- **high-water ≤ ~90 KiB** → M1 as written: stack 128 KiB, −128 KiB, done.
  Skip M7 entirely (its net saving is smaller — see M7's arithmetic).
- **high-water ≥ ~120 KiB** → M1 alone can only reach 192 KiB (thin margin)
  or nothing. Do **M7a first** to collapse the high-water, then set the stack
  to 64 KiB like rules.wasm.

### M2 — `MAX_LEGAL_MOVES` 4096 → 1024 for bots (target: −181 KiB, medium risk, big win)

Removes 178 KiB from `g_moves` (4 + 1024×58 = 59.4 KiB) and 3 KiB from
`forced_loss_flags`. Also shrinks the `solve_ws.rollout` union arm (232 K →
59.4 K) — no direct saving there because the `mv` arm (272 K) still dominates
the union, but it unblocks M5.

*Why 4096 today:* `Makefile:141` — "matches every native build". It was
never *measured* for the wasm bot path; rules was measured and ships 1024.
The `legal.c` scoped-cap machinery (`legal.h:44+`, `legal_set_move_cap`)
already guarantees enumeration saturates cleanly at the cap — overflow is a
clean clamp, not corruption.

*Measure first:* instrument the enumerator. Add to `legal.c` under
`#ifdef LEGAL_STATS`: a global `int legal_stat_max_n` updated at the end of
`calculate_legal_moves`, plus a getter. Build `cnitro_eval` with
`-DLEGAL_STATS` and run every shipped bot family × player counts 2–8 ×
≥2,000 games each (use the eval commands from
`SOLVER_TT_WORKING_SET_PLAN.md` §"Ground rules"; iterate `--strategy` over
octogen, semtex, cordite, fulminate, novichok/torpex if built, espresso,
handwritten), plus the fuzz suite corpus. Report the global max.

*Decision rule:* max ≤ 700 → ship 1024 (≥1.4× margin). 700 < max ≤ 2,900 →
ship the next power of two ≥1.4× max. max > 2,900 → **stop, keep 4096**, and
record the number in the Makefile comment (negative results get documented,
same as the TT plan's failed-approaches appendix).

*Also verify the contract (W2):* the chosen-index parity is covered by
`bot_parity` (7/7) — it replays real decisions through both TS and wasm and
compares the *move*, which catches any enumeration divergence. Run it plus
`e2e/wasm_kernel_fuzz.test.ts`.

*Change:* `Makefile:184`: `-DMAX_LEGAL_MOVES=4096` → `=1024` in `WASM_FLAGS`,
and fix the now-stale `filter-out` + re-add in `WASM_RULES_FLAGS:280` (rules
keeps 1024 either way; after this they simply agree — you can delete the
override and the filter-out entirely).

### M4 — `MAX_SNAPS` 24 → 16 (target: −9 KiB, trivial risk) *(do before M3; it's a warm-up)*

`Makefile:177-181`: analytic worst 11, measured worst 12 over 63K games,
overflow = silently dropped animation frames (visual only). 24 is 1.8× —
16 is still 1.33× the *measured* worst. Change `-DMAX_SNAPS=24` → `=16`,
run the e2e animation/marshal suites (`e2e/player_views.test.ts`,
`test:validate`) + `test:mem`. If any suite flags a dropped-frame diff, revert
to 24 and record.

### M3 — `MAX_LOGS` 512 → 256 for bots + derived `WASM_IO_CAP` (target: −66 KiB, HIGH risk — gated)

`g_game` is 98% logs: 512 × 132 B. rules.wasm ships `MAX_LOGS=128`; bots kept
512 because **espresso/handwritten beliefs import the whole session log**
(`Makefile:267` comment, `wasm_import_logs`), and `bots.ts:105` mirrors 512.
`g_io`'s 72 KiB default exists solely for the 512-log export
(`Makefile:275-276`: at 128 logs the widest write is 16,898 B).

*Measure first:* the real question is the longest log a REAL session reaches.
Instrument or query: (a) run `build/l1_measure` (it plays 28K engine games
with `MAX_LOGS=2048` native and reports peaks — check whether it already
prints a num_logs peak; if not, add it — one counter); (b) longest games are
8-player — bias the corpus there. Separately, read the `bots.ts:121-149`
import loops and determine **which end gets dropped** when a session exceeds
the clamp (oldest-first vs newest-first) — write the answer into the commit
message; belief code cares about *recent* history most, but verify.

*Decision rule:* measured peak ≤ 180 → ship 256 (1.4×). Peak > 180 → ship
384 or keep 512. **Regardless of the number**: this changes what espresso's
belief can see in ultra-long games, so it must ALSO pass the behavior gates
(V4/V5 below) with long-game seeds specifically, and the TS mirror
(`MAX_KERNEL_LOGS`) changes in the same commit (W3).

*Derived:* set `WASM_IO_CAP` for the bots build to
`2 + MAX_LOGS×(4 + 64×2) × 1.4` rounded to 4 KiB (at 256 logs: 2+256×132 =
33,794 → 48 KiB cap; −24 KiB). The chunked io writers already bounds-check
(`Makefile:174`, "io bounds-checks — clean error").

### M5 — packed solver move slots (stretch: −86 KiB, invasive — attempt only after M1–M4 are green)

`solve_ws.mv` = 48 depths × (4 + 100 × 58 B LegalMove) = 272 KiB, and after
M2 it dominates the union outright. The 58 B LegalMove spends 56 B on
`cards[28] + attack_cards[28]`; solver moves could pack each card as 6 bits
and cover-pairing as (battle_idx, card) pairs — ~40 B/move → 48×(4+100×40) =
188 KiB (−86 KiB). This means a solver-private move struct + pack/unpack at
the `solve_scratch_mv()` boundary. Behavior-identical by construction
(same move set, different storage), but it touches the struct solver used by
all three MC families — full V-ladder mandatory, plus native `cnitro_tests`
and `sim_difftest`. **Do not** instead reduce `SOLVE_SCRATCH_MOVES` or depth
(W7). If the pack/unpack shows up in the latency gate (>2%), revert; the
candidate is optional.

### M6 — split the replay codec out of bots.wasm (BLOCKED — do not attempt)

Would save ~118 KiB (g_rec, g_bn, g_comb, g_opts, g_weights, g_replay_io) plus
code bytes, but W1 forbids it: bots.wasm adopts the engine slot and must serve
replay. Unblocking requires a TS routing change (keep the rules instance
resident alongside; route replay/codec calls to it) — an architecture decision
for the maintainer, not this task. Recorded here so the next agent doesn't
re-derive it.

### M7 — de-fatten the solver recursion (conditional branch of M1; net −90 KiB when it applies)

The idea "replace recursion with a while loop + a table" is right in spirit
but the honest arithmetic says: **the table is not free**. An explicit frame
table must hold, per depth level, exactly what the recursion holds per frame —
the move list and the child state — so it trades stack bytes for BSS bytes
roughly 1:1. The win comes from two places only: (a) C stack frames carry
compiler overhead (spills, alignment, the worst-case-sized 160-slot move
buffer) that a hand-packed table avoids, and (b) once the solver stops being
the deepest stack consumer, the shadow stack can drop to the rules-module
bound (64 KiB, measured precedent). Two forms, strongly asymmetric in risk:

**M7a — hoist the fat locals into depth-indexed BSS, KEEP the recursion.**
This captures ~all of the benefit at ~30 lines of change. The struct solver
already works exactly this way (`solve_scratch_child(depth)` /
`solve_ws.mv[depth]`, `cordite_sim.c:2225,2236`) — `sim_solve_rec` is the
outlier that kept its buffers as locals. Change:

- `static _Thread_local SolMove sim_moves_d[48][100];` — 48 levels × 100
  slots × 18 B = 86.4 KiB. **100, not 160**: generation saturates at the
  buffer size, and any true count > 96 still reads as `nm > 96` → the
  `movecap` abort fires identically (this is the exact argument
  `SOLVE_SCRATCH_MOVES=100` already documents at `cordite_sim.h:146-152` —
  the abort SET is unchanged, so play is unchanged by construction).
- `static _Thread_local SimState sim_child_d[48];` — 15.7 KiB.
- `sim_solve_rec` indexes both by `depth` (depth ≥ 48 already aborts before
  touching either, `CD_SIM_SOLVE_MAX_DEPTH` check). Frame locals shrink to
  scalars (~150 B/frame → ~8 KiB total stack for the solver path).
- Then set the bots stack to **64 KiB** (the binding path becomes the rules
  cover-enumeration, ~45 KiB measured, same 1.4× story as rules.wasm).

Arithmetic: −192 KiB stack, +102 KiB BSS = **net −90 KiB**. That is LESS than
M1's −128 KiB — which is why this is a *branch*, not an addition: do M7a only
if the M1 canary shows the high-water is solver-fat (≥ ~120 KiB), where M1
alone is stuck. Wasm-only is not required (the hoist is behavior-neutral and
`_Thread_local` keeps native OMP safe — same pattern as the existing scratch),
but gate it V0-style anyway: SIG-identical over the 20-seed check + tricky
panel, plus V-latency (index arithmetic replaces stack bumps; expect noise).

**M7b — full iterative rewrite (while-loop + explicit stack) — NOT
recommended.** After M7a the recursion frames are ~150 B of scalars; an
iterative rewrite eliminates at most those ~8 KiB and the call overhead, in
exchange for hand-managing alpha-beta unwinding, the TT store-on-return path,
abort propagation, and the trace instrumentation in the hottest, most
heavily-validated function in the module. Every future maintainer pays the
readability tax. Only revisit if some future target can't afford a 64 KiB
stack at all.

### M8 — overlay non-concurrent scratch: replay ↔ solver (−90.5 KiB, wasm-only)

The module has two large *per-call scratch* families that can never be live
at the same time — the wasm instance is single-threaded and both are private
working state of top-level export calls that never nest
(`wasm_choose_move` vs `wasm_replay_encode/decode`):

- **Replay-call scratch:** `g_rec` (48.0 KiB, indexed from a per-call `Coder`
  counter that starts at 0 — `replay.c:140-190`), `g_bn` (10.5 KiB),
  `g_replay_io` (32.0 KiB). Total **90.5 KiB**.
- **Choose-call scratch:** `solve_ws` (272 KiB) — alone big enough to host
  all of the above.

Aliasing the replay scratch INTO `solve_ws` makes those 90.5 KiB disappear —
no behavior change, pure address reuse. The in-tree precedent is `solve_ws`
itself (already a union of two never-concurrent arms, `cordite_sim.c:2205-2216`).

**Exactly what may NOT be overlaid** (and why — the executor must not
"improve" on this list): the persistent codec tables `g_comb`/`g_opts`/
`g_weights` (initialized once, read forever); `cd_tt` (persists per game for
octogen); `g_game`/`g_snaps` (marshal state that persists across call
boundaries — the resident-state fast path depends on it, W1); `g_moves` and
`g_io` (interface buffers TS reads *after* a call returns — overlay-eligible
in principle since the TS bridge reads synchronously, but verify the bridge
call-by-call before touching; treat as a possible extension, not part of M8).

**Mechanism — in source, not the linker** (wasm-ld has no overlay/section
placement facility): expose the arena from `cordite_sim.c`
(`unsigned char *const cd_overlay = (unsigned char *)&solve_ws;` behind a
getter or extern), and in `replay.c`/`wasm_api.c` redefine the three blocks
under the flag:

```c
#ifdef CD_WASM_OVERLAY            /* set ONLY in WASM_FLAGS */
#define g_rec       ((RecChoice *)(cd_overlay + 0))
#define g_bn_limbs  ((uint32_t  *)(cd_overlay + 49152))   /* 16-aligned offsets */
#define g_replay_io ((unsigned char *)(cd_overlay + 60416))
_Static_assert(60416 + REPLAY_IO_CAP <= sizeof(solve_ws), "overlay fits");
#else
/* existing static definitions */
#endif
```

**Why wasm-only is a hard requirement:** natively the solver scratch is
`_Thread_local` (OMP eval runs games in parallel) while the replay buffers are
process-global — overlaying them on native is a data race by construction.
The flag lives in `WASM_FLAGS` and nowhere else.

**Verify before shipping (each is one read, do them all):**
1. `g_rec`/`g_bn` are fully written-before-read within a single
   encode/decode call (the `Coder`/`Bn` lifecycle — counters start at 0 per
   call; no cross-call reads). Confirmed for `g_rec` by inspection
   (`replay.c:152-156` writes at `c->n_rec` from 0); repeat for `g_bn`.
2. The TS replay bridge is write→call→read within one synchronous function
   (`engine.ts:~1380` `kernelReplay`) — no choose call can interleave between
   a replay input write and the codec call.
3. The overlaid blocks are zero-init BSS with no static initializers (a data
   segment would actively write into solver scratch at instantiation — check
   `llvm-nm` section letters / the anatomy Data tab).

**Gates:** `replay_difftest` (2.3M checks), e2e `replay_codec` +
`bot_parity` + fuzz + `test:mem`, plus one NEW test this candidate uniquely
needs — an **interleave check**: encode a game → run a bot `choose` on the
same instance → encode the same game again → byte-identical outputs (proves
no replay state survived that choose needed to preserve, and vice versa).

### M9 — other overlay pairs (survey, smaller)

Same analysis applied to the rest of the map turns up only small fry:
`world/trial/diff` slots vs replay scratch (already only 19 KiB, and they'd
land inside the same arena anyway — no extra saving beyond M8);
`solve_child_scratch` (55 KiB) is choose-scratch like `solve_ws`, so it's on
the wrong side of the overlay to help. If M3 lands and `g_io` shrinks to
~40 KiB, folding `g_io` into the arena (after the verify-first caveat above)
is worth another ~40 KiB — record it as M8-ext and do it only with the same
interleave gate extended to export-path calls (`wasm_export_logs` →
choose → re-export, byte-identical).

---

## 3. V-gates (run per candidate; all must be green)

Build/refresh everything first: `cd cnitro && make build/bots.wasm && make wasm-bots`.

| gate | command | pass bar |
|---|---|---|
| V-native | `make tests && build/cnitro_tests` | 161/161 |
| V-diff | `build/sim_difftest`, `build/apply_difftest`, `build/replay_difftest` | 0 real / 0 / 0 failed (solver_difftest is a known `-flto` false alarm — issue #56) |
| V-mem | `npm run test:mem` | 4/4, bounded & flat |
| V-parity | `bot_parity.test.ts` | 7/7 exact-move match |
| V-fuzz | `e2e/wasm_kernel_fuzz.test.ts` + cover/guards e2e | green (stack + enumeration net) |
| V4-outcome | only for M3/M5: `cnitro/tools/tt_divergence_viz/outcome_pair.sh` 2,000+ espresso seeds, flag-on vs flag-off at SAME TT config | 0 outcome flips |
| V-latency | 40-game CPU-time probe (`CD_LAT=1`, see `SOLVER_TT_WORKING_SET_PLAN.md`) before/after | within ±5% |
| V-size | `llvm-nm` map + anatomy regen + page count | saving matches prediction; no other block grew |

Ablation rule (from the maintainer): if latency regresses, bisect by building
with each flag/cap change alone and re-running V-latency — the harness and
binaries pattern are in `/tmp/l1/latab.sh` form in the session logs; rebuild
from the commands in `SOLVER_TT_WORKING_SET_PLAN.md`.

## 4. Bookkeeping

- One candidate per commit; Makefile comment updated with the measured number
  and margin (match the existing style at `Makefile:130-181`).
- Regenerate `docs/wasm-anatomy.html` (`tools/wasm_anatomy/generate.sh`) and
  `make wasm-bots` (refreshes the shipped `.gz`) in each landing commit.
- Update the running total in `docs/WASM_L1_BUDGET.md` (the "bots.wasm floor"
  section) when a candidate lands.
- Negative results (a cap that measured too close to its limit) get a short
  entry appended to this file — the number is the deliverable, same as the TT
  plan's appendix.

## 5. Expected ledger

Pages = ceil(KiB/64); runtime adds the 32 KiB TT + heap slack on first play.
The M1/M7 row is a BRANCH decided by the canary measurement (see M1's
decision tree): take M1's −128 KiB if the high-water is low, else M7a's
−90 KiB (which also unlocks the 64 KiB stack).

| step | saving | running total | pages |
|---|---|---|---|
| today | — | 1,152 KiB | 18 |
| M1 stack →128 KiB **or** M7a hoist + stack →64 KiB | −128 / −90 KiB | 1,024 / 1,062 | 16 / 17 |
| M2 moves →1024 | −181 KiB | 843 / 881 | 14 |
| M4 snaps →16 | −9 KiB | 834 / 872 | 14 |
| M8 replay↔solver overlay | −90.5 KiB | 744 / 782 | 12 / 13 |
| M3 logs →256 (gated) | −66 KiB | 678 / 716 | 11 / 12 |
| M5 packed solver moves (stretch) | −86 KiB | 592 / 630 | 10 |

Execution order: M1-measure first (its canary number picks the M1/M7a
branch), then M2 → M4 → M8 → M3 → M5. M8 is independent of the others and
can be pulled earlier if its interleave test is written first.

The floor under all of this is the solver working set (`solve_ws` +
`solve_child_scratch` + world slots ≈ 350 KiB — though after M8 the replay
scratch lives INSIDE it rather than beside it) plus the rules-engine state
the module must host because it IS the engine (W1). Getting below ~8 pages
means either M6's architecture change or the "different solver" the L1
budget doc already names as the real frontier.
