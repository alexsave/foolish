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
method is established and documented in `sdk/c/Makefile:130-181` and
`docs/WASM_L1_BUDGET.md`:

1. **Measure the real requirement** with a harness over tens of thousands of
   games (`sdk/c/tests/l1_measure.c` is the existing tool; build rule at
   `sdk/c/Makefile:106-110` — it compiles with huge caps and reports observed
   peaks).
2. **Cap at ~1.4–2× the measured/analytic worst case**, stated inline in the
   Makefile comment next to the flag.
3. **Overflow must be a CLEAN error** (trap, clamp, or drop) — never
   corruption. `--stack-first` makes stack overflow a loud trap.
4. **Ship only behind green gates** (§V below).

Every candidate below follows that shape: measure → cap → gate.

### Re-measuring the memory map

```sh
cd sdk/c && make build/bots.wasm
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

Build/refresh everything first: `cd sdk/c && make build/bots.wasm && make wasm-bots`.

| gate | command | pass bar |
|---|---|---|
| V-native | `make tests && build/cnitro_tests` | 161/161 |
| V-diff | `build/sim_difftest`, `build/apply_difftest`, `build/replay_difftest` | 0 real / 0 / 0 failed (solver_difftest is a known `-flto` false alarm — issue #56) |
| V-mem | `npm run test:mem` | 4/4, bounded & flat |
| V-parity | `bot_parity.test.ts` | 7/7 exact-move match |
| V-fuzz | `e2e/wasm_kernel_fuzz.test.ts` + cover/guards e2e | green (stack + enumeration net) |
| V4-outcome | only for M3/M5: `sdk/c/tools/tt_divergence_viz/outcome_pair.sh` 2,000+ espresso seeds, flag-on vs flag-off at SAME TT config | 0 outcome flips |
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

---

## 6. Execution log & measured results (Jul 2026)

Executed in the order M1-measure → M2 → M4 → M8. Each entry records the
measured number (the deliverable), not the estimate.

### M1 / M7 — shadow stack: NEGATIVE (no change; 256 KiB stays)

The canary (`e2e/stack_canary.mts` — paint the shadow stack 0xA5, drive the
heavy corpus through the production bot bridge, scan the high-water) settled
the M1/M7a branch and then killed both arms:

- **Base high-water = 179 KiB.** That makes today's 256 KiB stack only
  **~1.43×** — the *same* margin rules.wasm ships, NOT the "~3×" the old
  Makefile comment implied (it assumed ~1.5 KiB/frame; the real driver is
  fatter and different). So M1-as-written (256→128 KiB) is dead on arrival:
  128 KiB is *below* the measured worst case.
- **The high-water is not solver-recursion-dominated.** Disassembling the
  per-function wasm stack frames shows the tall frames are the heuristic
  choosers' `int[MAX_LEGAL_MOVES]` index arrays:
  `handwritten_strategy_choose` = 7×`int[4096]` = **112 KiB**,
  `espresso_strategy_choose` = **64.5 KiB**, `champion` = 32 KiB — plus
  `sim_solve_rec`'s 48-frame recursion (~3.2 KiB/frame fat locals).
- **M7a was built and validated** (hoist `sim_solve_rec`'s move buffer +
  child `SimState` into depth-indexed `_Thread_local` BSS; SIG-identical to
  baseline over 720 games, 4 families × pc 2/3/4 × 60). It drops the solver
  branch, but the new high-water is the untouched 112 KiB handwritten frame
  (**measured 115 KiB**). So the stack can only reach ~192 KiB (−64 KiB) at a
  cost of +102 KiB BSS — a **net loss**. Reverted.
- The heuristic frames scale with `MAX_LEGAL_MOVES`, so the *only* lever that
  shrinks them 4× is M2 — which is also negative (below). **M1 and M2 are
  coupled**; neither ships. The Makefile stack comment is corrected to the
  measured 179 KiB / 1.43× reality.

### M2 — `MAX_LEGAL_MOVES` 4096→1024: NEGATIVE (4096 stays)

Instrumented the enumerator (`-DLEGAL_STATS` in `legal.c`, records the widest
FULL-cap menu — the `g_moves` buffer, excluding the solver's lowered-cap
scratch) and swept every shipped bot family × player counts 2–8 (production
deck: 36 cards at 2–5p, 52 at 6–8p), fast heuristics at 1,500 games/config and
the MC families lighter. **The menu saturates the 16,384 buffer under every
strategy, including the shipped MC bot octogen and the handwritten rollout
policy.** The wide states are ordinary large-hand 8-player *defenders*: a
20–23 card hand over 4 uncovered battles enumerates **5,616 → 16,384+** cover
combinations (the documented cover-combination blow-up, `legal.h`). This is
real reachable bot play, not a corrupt state. Per the plan's rule
(max > 2,900 → keep 4096) and because truncating a **bot's** cover set at 1024
drops legal options it would autonomously choose from (a behavior change —
unlike rules.wasm at 1024, which only bounds the human UI's cover menu),
**4096 stays.** The rules/bots cap asymmetry is deliberate. (bots already
truncates the widest 8p states at 4096; 1024 would truncate more.)

**Consequence for the ledger:** the plan's two largest projected wins
(M1/M7 −90…−128 KiB and M2 −181 KiB) do not materialize *as written*. The
realistic remaining static-cap budget is M8 (−90.5 KiB) + M4 (−9 KiB) +
M3 (−66 KiB, gated). But the M1/M2 dead end pointed at a better lever:

### M2-stream — streaming (visitor) enumeration instead of truncation

The reason M2 (blunt cap → 1024) changes behavior is that it *drops legal
moves*. But bot move-evaluation is a **reduction** — score every move, keep the
best (heuristics) or the top-K (the MC bots' candidate stage, `og_pick_candidates`
already keeps ≤27 via `og_ranked_insert`). A reduction never needs the whole list
resident: enumerate → score → discard. So the honest fix is to make the
enumerator **push each move to a visitor** instead of filling the shared 232 KiB
`g_moves` buffer. That shrinks `g_moves` to a tiny top-K window **and** collapses
the heuristic choosers' `int[MAX_LEGAL_MOVES]` frames — behavior-neutral (same
moves, same order, same tie-breaks), unlike truncation. Evidence it's the right
model: octogen-vs-octogen at 6 players enumerates **7,593** moves as the actor —
already above today's 4096 — yet only ever searches its top ~27, so even 4096 is
wasted materialization.

**UPDATE — the stack cut landed (256 → 64 KiB, −2 pages).** M1/M7's "negative"
above was only true *before its blockers cleared*. Once M2-stream removed
handwritten's 112 KiB frame **and** the ship-set trim dropped semtex/fulminate
(the only bots that reached the struct-rollout espresso frame), the shipped MC
bots (octogen/cordite) use the bitboard rollout exclusively — so applying **M7a**
(hoist `sim_solve_rec`'s fat locals to BSS) drops the canary high-water to
**13.2 KiB** over the shipped-bot corpus. The recursion is depth-capped at 48, so
~15 KiB is a near-hard ceiling; the bots stack now ships **64 KiB** (4.8×,
wasm-only). Net −192 KiB stack at +102 KiB BSS = **−90 KiB**, bots.wasm
**17 → 15 pages**. So the plan's M7a −90 KiB *did* materialize — it just needed
M2-stream + the ship-trim first. Full step-1 detail:

**Step 1: `handwritten_strategy_choose` streamed.** It bucketed move
indices into five `int[MAX_LEGAL_MOVES]` arrays (+2 in the attack branch) = a
**112 KiB** stack frame at MAX_LEGAL_MOVES=4096, purely to run per-category
argmax/argmin. Those are now one streaming pass over the list with scalars
(`hw_mcl`). SIG-identical over 2,760 games, bot_parity green, and **~11% faster**
on handwritten self-play (0.373s → 0.331s / 4,000 games) — no array
materialization. The 112 KiB frame is gone from the module's top frames; the
shadow-stack high-water dropped 179 → **154 KiB** with this alone (still
solver-recursion-bound). handwritten is the MC bots' rollout policy, so this is a
hot-path latency win too. Remaining to reach an actual stack-size cut:
stream espresso (its 66 KiB rollout frame) + apply M7a (solver-locals hoist),
then the stack falls under a page boundary. The full `g_moves` win needs the
visitor enumerator + all consumers converted — the biggest single BSS prize
(−232 KiB) and the natural next step.

### Ship-set trim — drop unshipped bots from the wasm build

Orthogonal to the caps: the wasm module only needs the deployed ladder bots
(Durak Bot Ordnance Chart). Dropped champion, ultimate_champion, hacker,
fulminate, espresso_prod and semtex from `WASM_BOT_SRC` entirely (no ladder slot,
no dependency of a shipped bot); espresso/handwritten (arena) stay linked as the
MC bots' rollout policies. **bots.wasm 178,743 → 127,389 bytes (−29% code)**,
gz ~60 → 49 KB. This is a module-size / cold-start win; the dropped weight is
mostly code (outside linear memory), so the initial page count is unchanged. It
also shrinks the consumer set the visitor-enumerator refactor must convert.

### M9 — overlay g_io into solve_ws (SHIPPED, −1 page)

The M8/M9 arena idea extended to `g_io` (the 72 KiB marshaling I/O buffer): a
third non-concurrent tenant of `solve_ws`, in a region disjoint from the replay
scratch. `g_io` is input (copied into `g_game` by `wasm_import_*` **before** a
choose's solve) and output (the chosen move / an export written **after** it) —
the solver reads `g_game`/SimStates, never `g_io` — so it is sequential with the
solver within a choose and never coincides with a replay call. −72 KiB, bots.wasm
**15 → 14 pages**. Gates: `test:mem` (the real marshal→choose→apply path carries
state/logs in and the move out around the solve) + `bot_parity`'s direct-move
decode (reads the move straight from `g_io` after the solve) + replay/fuzz.

### M3 — `MAX_LOGS` 512→256: NEGATIVE (512 stays)

Gating measurement (`build/l1_measure`, `mx_logs_game` over ~7,000 games/seed,
two seeds): the **session log peaks at 1,028 / 1,036 entries** — far above the
plan's 180 threshold, and above the *current* 512 (so long games already truncate
what espresso/handwritten's belief imports). Cutting to 256 removes more of that
history → a behavior change on long games. Keep 512. (Post-M9 its memory upside
was also gone: the derived `WASM_IO_CAP` shrink is moot because `g_io` is now
overlaid, and `g_game` — the only remaining beneficiary, ~34 KiB — can't be
overlaid, it persists across the solve as the resident state, W1.)

### M5 — packed solver move slots: NEGATIVE on ROI (not shipped)

`solve_ws` is a `union { SolveMoves mv[48]; LegalMoves rollout; }`. Measured
arms: `mv` = 278,592 B (dominant), `rollout` = 237,572 B, and the rollout arm is
**live** for the shipped bots (`rollout_moves_scratch()` in og_/cd_simulate's
struct rollout). So packing the `mv` LegalMove 58→~40 B shrinks `mv` to 192 KiB
but the union only falls to the rollout arm's **237 KiB = −41 KiB (no page
boundary crossed)**, while adding pack/unpack to the hottest struct-solver loop
(the plan's own >2%-latency-revert tripwire). The full −86 KiB additionally needs
the rollout arm shrunk (its own smaller cap) — a truncation risk on the struct
rollout's lite move list. And the implementation is worse than "invasive":
today the three solvers enumerate **zero-copy** by casting `SolveMoves*` to
`LegalMoves*` and letting `calculate_legal_moves` write 58 B `LegalMove`s
straight into the slot (`cordite_strategy.c:502`, "shares LegalMoves' leading
layout"). Packing to 40 B breaks that cast — it forces enumeration into a temp
+ a pack step, and an **unpack of every move in the hottest solver read loop**
(each node reads `mv[depth].moves[i]` and hands it to `cd_apply`), which is
exactly the >2%-latency tripwire the plan set for M5. Capped below a page,
latency-risky, and it dismantles a clean fast path: not worth destabilizing the
shipped MC bots. Measured and hands-on-verified, not shipped.

### M6 — split the replay codec out: BLOCKED (unchanged)

W1 stands: bots.wasm adopts the engine slot and must serve the replay codec on
the one instance. Unblocking is a TS routing change (keep a resident rules
instance and route codec calls to it) — an architecture decision, out of scope.
Not attempted. (M8/M9 reclaimed the replay+io scratch *bytes* via overlay
instead, which is the achievable part of the same idea.)

### M7b — full iterative solver rewrite: NOT RECOMMENDED (unchanged)

After M7a the `sim_solve_rec` frames are ~scalars; a hand-managed iterative
rewrite would save at most those few KiB in exchange for hand-rolling alpha-beta
unwind + the TT store-on-return + abort propagation in the module's most
heavily-validated function. The 64 KiB stack (13.2 KiB high-water) needs no more
stack headroom, so there is no motivation. Not attempted.

---

## 7. Final ledger — every candidate

Initial linear memory: **18 pages (1,179,648 B) → 14 pages (917,504 B)** this
round (−4 pages), plus bots.wasm code **−29%** (178,743 → 127,389 B) and the
runtime peak drops correspondingly (TT + heap unchanged).

| candidate | outcome | Δ | notes |
|---|---|---|---|
| M1 / M7a — shadow stack 256→64 KiB | **SHIPPED** | −90 KiB (17→15 pg) | unlocked by M2-stream + ship-trim; solver-locals hoist, canary 13.2 KiB |
| M2 — MAX_LEGAL_MOVES→1024 | **NEGATIVE** | 0 | menu saturates >16k under shipped bots; truncation = behavior change |
| M2-stream (handwritten) | **SHIPPED** | frame −112 KiB, +11% speed | streaming reduction; enabled the stack cut |
| M3 — MAX_LOGS→256 | **NEGATIVE** | 0 | session log peaks ~1,030 ≫ 180 |
| M4 — MAX_SNAPS 24→16 | **SHIPPED** | −9 KiB ×2 | 1.33× measured worst |
| M5 — packed solver moves | **NEGATIVE (ROI)** | (−41 KiB, no page) | union-capped by live rollout arm; latency risk |
| M6 — split replay codec | **BLOCKED** | — | W1 (engine-slot adoption) |
| M7b — iterative solver | **NOT REC.** | — | no stack pressure left after M7a |
| M8 — overlay replay scratch | **SHIPPED** | −90.5 KiB (18→17 pg) | non-concurrent alias into solve_ws |
| M9 — overlay g_io | **SHIPPED** | −72 KiB (15→14 pg) | third non-concurrent tenant of solve_ws |
| Ship-set trim | **SHIPPED** | code −29% | drop 6 unshipped bots |

All shipped changes are wasm-bots-only where noted; native builds keep every bot
and every large cap. Gates held throughout: native 161/161, sim/apply/replay
difftests 0 real, bot_parity, test:mem 4/4, replay_codec + interleave, fuzz.

## 8. Latency & further-tenant investigation

Follow-up questions: hottest memory, memory ordering, M5-with-latency, more
overlay tenants. Measured with the `CD_LAT=1` CPU-time probe (`main_eval.c`) —
octogen pc2 vs cordite is the latency-relevant matchup (octogen ≈ **41.5 ms/dec**,
cordite ≈ 0.82 ms/dec at the native TT).

- **The hot path is octogen's bitboard endgame solver** (`sim_solve_rec`): per
  node it does one TT probe (random-indexed → the cache-miss hotspot), one
  `sim_gen_moves`, and per child a ~264 B `SimState` clone (`memcpy` skipping the
  dead deck tail) + `sim_apply_sol`. By *access count* the SimState clone
  dominates (nodes × branching); by *cache misses* the TT does.
- **The TT is already tuned for L1**: 32 KiB (`TT12+2WAY+PACK8`, a quarter wasm
  page), `calloc`'d 16-B-aligned so every 16-B 2-way pair sits within one 64-B
  cache line. No alignment/size win left.
- **M7a was a latency *win*, not a cost.** Measured directly (same 2,092
  decisions, same seeds): pre-M7a 45.4 ms/dec → M7a **41.5 ms/dec, −8.6%**. The
  100-slot depth-indexed BSS buffers are a smaller per-level footprint than the
  old 160-slot stack frame and drop the per-frame fat-local stack traffic. So the
  memory win (−90 KiB stack) came with a latency win.
- **Memory ordering: little to gain.** The bitboard solver's per-node BSS
  (`sim_rec_moves`, `sim_rec_child`) is already adjacent (link order), and the TT
  is a separate L1-sized `calloc`. Reordering the static block order would not
  change the hot working set (TT + the depth-indexed slots + the live SimState).
- **M5 with latency: not worth it, and it saves nothing charged.** Beyond the
  latency risk (og_solve, a hot struct-solver path, would take a per-node
  pack/unpack — §6), M5 is union-capped at −41 KiB which **does not cross a page**
  (917,504 − 41 KiB = 876,484 = still 14 pages). The edge external-memory budget
  is charged per page / buffer size, so a sub-page shrink of an overlaid arena
  buys nothing at runtime while risking the 2 s-CPU-cap latency. Decisively not
  worth implementing.
- **More tenants for solve_ws: none left.** After M8 (replay) + M9 (g_io), the
  remaining big blocks can't join: `g_moves` (232 KiB) is **concurrent with the
  solver** — octogen's win-hunt reads `moves->moves[i]` across `moves->n`
  throughout the search (`octogen_strategy.c:906`), so it can't be copied to a
  small buffer and freed; `g_game`/`g_snaps` are the resident marshal state that
  must persist across the solve (W1); the codec tables `g_comb`/`g_opts`/
  `g_weights` are initialized once and read forever; and `solve_child_scratch` +
  the world/trial/diff slots are themselves choose-scratch, live *during* the
  solve. `g_io` was the last non-concurrent tenant.

**Net:** the module is well-tuned; the round's remaining upside is not in caps or
overlays but in the two large, higher-risk architectural moves already named —
the visitor-enumerator `g_moves` rework (needs the TS list-path wire change) and
M6's replay-routing split — neither of which is a quiet in-place change.
