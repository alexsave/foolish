# Infinite Oracle - Mode B, as built

Mode B is the Infinite Oracle's second engine: the same octogen brain, rebuilt for **shared-memory wasm threads**, with the coordination in C instead of TypeScript.
It is specified in full in `docs/INFINITE_ORACLE_DESIGN.md` §8b.
This document records what was actually built, what it measures, and the two findings that change how you should think about shipping it.

Mode A (the instance fleet) still ships and is still the default.
Mode B is selected at runtime, per visitor, and only where the page is cross-origin isolated.

## What changes between the modes

| Concern | Mode A (fleet) | Mode B (threads) |
|---|---|---|
| Memory | N private linear memories | ONE shared `WebAssembly.Memory` |
| Marshal / logs / env | per worker, per batch | ONCE, by the control instance on the main thread |
| Seeding | TS posts a seed per batch | C: `mix3(seed_base, tid, batch_no)` into a per-thread LCG |
| Score merge | TS sums batch records | C: `__atomic_fetch_add` into a shared per-candidate table |
| UI feed | one `postMessage` per batch | the main thread polls the C accumulator each frame |
| Worker code | bridge + batch loop | a trampoline: set stack, init TLS, enter the C loop, never return |
| Explain dump | JSONL per batch, parsed in JS | not built - the C accumulator replaces it |

The overlay, `replayOracleInput`, `logsWire` and the candidate rows are shared verbatim.
`src/oracle/oracleControllerFactory.ts` picks the engine; nothing above it knows which one is running.

## Where it lives

```
c/src/oracle_mt.h              the shared control block (MT4)
c/wasm/wasm_oracle_mt.c        the thread loop + the control-side exports (MT7/MT8)
c/src/octogen_strategy.c       the accumulate + verdict-capture seams (MT5/MT6), all #ifdef FOOLISH_ORACLE_MT
c/wasm/wasm_bots_api.c         the bump allocator's spinlock (MT1)
c/Makefile                     the `wasm-oracle-mt` target
public/oracle-mt.wasm.gz       the committed second artifact
src/oracle/oracleMtSession.ts  everything that is not "spawn a thread"
src/oracle/oracleMtWorker.ts   the browser trampoline
src/oracle/OracleModeBController.ts   the replay-screen controller
src/oracle/oracleControllerFactory.ts mode selection
e2e/oracle_mode_b.test.ts      the §8b.7 suite, over node:worker_threads
scripts/oracle_bench.mts       Mode A vs Mode B, measured
```

Build (needs Homebrew LLVM - plain clang cannot target wasm32):

```
cd c && WASM_CC=/opt/homebrew/opt/llvm/bin/clang make wasm-oracle-mt
```

Run the suite and the bench:

```
TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx --test --test-concurrency=1 e2e/oracle_mode_b.test.ts
BENCH_THREADS=6 BENCH_SECONDS=4 TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx scripts/oracle_bench.mts
```

## Finding 1: the speedup is about 10%, not a multiple

Measured on an 8-core Apple M-series, octogen choose-calls per second at `OG_W1=24`, same decision, same thread count in both modes:

| Threads | Mode A | Mode B | Mode B / Mode A |
|---|---|---|---|
| 1 | 9,380 /s | 10,133 /s | 1.08x |
| 3 | 26,509 /s | 29,231 /s | 1.10x |
| 6 | 37,570 /s | 41,328 /s | 1.10x |

Both modes scale with thread count the same way, because Mode A's fleet is already N OS threads.
What Mode B removes is the **per-batch** overhead - the marshal, the strategy-key write, the log import, the JSON dump and its parse - and at a ~40 ms batch that is worth roughly a tenth of the time.
§8b.8 predicted exactly this ("unlikely at ~25 Hz total").
The number is the number: Mode B is a real but modest throughput win, not a step change.

## Finding 2: it does not save memory either

At 6 threads, on the endgame decision where every thread grows a transposition table:

- Mode B: **69.7 MiB**, one shared memory.
- Mode A: **11.7 MiB** per instance, so **70.1 MiB** across a 6-worker fleet.

A wash.
The reason is that the 8 MiB transposition table dominates, and `cd_tt` is `_Thread_local` - deliberately, see below - so Mode B grows one per thread exactly as Mode A does.
What Mode B saves is N copies of the ~2 MiB of statics; what it adds is the control instance's own table.

So of §8b.8's three reasons to build Mode B, the first two (fleet memory, per-batch overhead) do not pay for themselves on this hardware.
The third one - "the team simply wants the coordination surface in C" - is the one that stands up, plus roughly 10% more sampled worlds per second for free once the page is isolated.

## Determinism

**Neither mode is bit-reproducible, and Mode B is no less reproducible than Mode A.**

Both are wall-clock-bounded Monte-Carlo estimators: the run stops on a timer, so the number of batches that landed is whatever the machine managed.
Mode B additionally interleaves N threads' atomic adds, but the folded quantity is an integer sum of finish positions, so the ORDER of the adds cannot change the total - only how many arrived before the timer did.

Mode B's seed base is derived from the job (`oracleSeedBase`, FNV-1a over the decision id), not from `Date.now()`, so repeating a decision draws the same seed *stream*; only its length varies.
Mode A already worked this way (`seedSalt = i * 0x9e3779b1 + 1`).

Measured, three runs of the same decision at 3 threads, 2 s each, worst per-candidate spread in expected finish:

| | run-to-run spread |
|---|---|
| Mode B | 0.0004 - 0.0007 |
| Mode A | 0.0003 - 0.0013 |

Row ORDER was identical across every run in both modes.
That is well inside the +- the overlay renders, and far inside anything a user could act on differently.

Making Mode B exactly reproducible would mean fixing the batch count instead of the wall clock, and joining on it - which is the one thing an interactive "come into focus" panel must not do, since the whole design is that the answer sharpens while you watch.
It is not worth doing for the replay screen.
A future offline post-game analyser, which is batch work and not interactive, *should* run to a fixed batch count instead of a fixed time; then it is exactly reproducible in both modes.

## The transposition table is per-thread, on purpose

The obvious way to build a threaded solver is to share the transposition table.
This build does not, and that is load-bearing.

`cd_sim_solve` returns a position's value from ONE seat's point of view.
The table keys on the position alone, and until `fix(solver): a solved endgame now carries the seat it was solved for` it also *stored* one seat's perspective - so the second seat to ask read the first seat's proof unflipped and was told the loser wins.
The fix stores the canonical value (always the lower-indexed IN player's side) and flips it on probe, which also swaps LOWER and UPPER on a fail-soft bound.

Under `-DFOOLISH_ORACLE_MT` the `-D_Thread_local=` strip is removed, so `cd_tt`, its tail cache, both LCGs, `solve_ws` and the recursion scratch are all genuinely per-thread again - the proven native OMP model.
No thread ever reads another thread's entries, so the seat-perspective invariant stays a single-threaded property and concurrency cannot reintroduce the bug.
`e2e/oracle_mode_b.test.ts` §8b.7-5 pins this: on a deck-empty decision with the solver engaged, every verdict Mode B proves under three threads must equal the verdict Mode A proves single-threaded.

Sharing one table across threads would need the canonicalisation to survive concurrent torn stores as well, which this build does not attempt.
It would also save 8 MiB per thread, which is the only thing that would make Finding 2 come out differently - a real future option, and a much more delicate one than it looks.

## The candidate-set guard

Threads fold their scores into `sum_fp[i]` / `nsim[i]` by candidate INDEX.
Candidate enumeration is deterministic (`og_pick_candidates`, no RNG), but its input is not: `forced_loss` comes from the budget-bounded root endgame solve, run off a per-thread table, so two threads can legitimately arrive with different candidate SETS.
Summing those under one index would average two different moves into one bar.

So the first batch of a generation CAS-publishes the descriptor table, and every later batch is compared against it byte for byte; a batch that disagrees is dropped and counted in `desc_mismatch`, which rides in the snapshot.
On the fixtures the suite runs, the counter stays 0 - the guard is defensive, not load-bearing today.
The test proves the counter is wired and that a clean run does not trip it; it does not prove the guard is currently preventing anything, because the fixtures do not produce disagreement.

## Thread stacks and the canary

Mode A inherits `--stack-first`, where a stack overflow traps loudly.
Mode B's threads run on heap-region stacks, where it would silently smash whatever is next door.
So (§8b.6): 512 KiB per thread, about 36x the measured 14.3 KiB production worst case; stacks placed at the LOW end of the reserved region so thread k's overflow walks into thread k's own TLS block rather than a neighbour's stack; and a canary word at each stack's low end, checked by that thread after every batch.
`canary_trips` rides in the snapshot and the controller kills the run if it ever ticks.
It stays 0 across the suite.

## Cross-origin isolation: Mode B cannot run on the deployed site today

`SharedArrayBuffer` requires the document to be cross-origin isolated, which requires
`Cross-Origin-Opener-Policy: same-origin` + `Cross-Origin-Embedder-Policy: credentialless`.
**The site does not send those headers.** Until it does, `oracleMode()` returns `'A'` for every visitor and Mode B is dead code in the browser (it still runs headlessly in Node, which needs no isolation - that is what the suite and the bench use).

`next.config.mjs` carries the headers behind `FOOLISH_CROSS_ORIGIN_ISOLATION=1`, default off.
A build-time flag rather than a code change, because the headers apply to the whole origin and rolling them back should not need a revert and a review:

- **Google Fonts.** `src/app/layout.tsx` loads a stylesheet from `fonts.googleapis.com` and its faces from `fonts.gstatic.com`. §8b.2's audit says this app has "no CDN fonts". That is no longer true. `credentialless` loads no-credential cross-origin subresources without CORP, and Google serves both with permissive CORS, so it should hold - but it is the live site's typography and it deserves a real check on a real deployment.
- **Supabase** REST and realtime are CORS fetches and websockets, which COEP permits.
- **COOP: same-origin severs `window.opener`.** Redirect-based auth is unaffected; any popup flow is not.
- `@vercel/analytics` is served same-origin.

To turn it on: set `FOOLISH_CROSS_ORIGIN_ISOLATION=1` in the deployment environment, redeploy, then open a replay and confirm `crossOriginIsolated === true` and `window.__oracleMode === 'B'`, and that fonts, sign-in and a live game are all unharmed.
To turn it off: unset it and redeploy.

## MT3 static audit, as found today

§8b.5's audit list, re-walked against the current kernel:

1. **Bitboard mask tables** (`SUIT_MASK` / `VALUE_MASK` / `HIGHER_MASK` + `g_masks_ready`, `cordite_sim.c`) are plain statics, lazily first-touch initialised, with no acquire/release on the guard. Handled: `wasm_mt_warmup` runs one full control-thread deliberation before any worker is armed, so threads only ever read them.
2. **The snapshot ring.** `wasm_init()` installs `snap_cb` into the shared `engine_snap_hook`, and SNAP() fires inside `game.c`'s `handle_*`, reachable from a thread with no `wasm_*` call involved. Handled: `wasm_mt_warmup` clears the hook. The oracle never reads snapshots.
3. **`engine_last_reject`** - rewritten at the top of every `handle_*`, read only by `wasm_reject_reason`. A benign diagnostic race; classified and left.
4. **`log_alloc`'s static drop-sink scratch** - `solve_clone_prefix` routes solver-child log appends into it and nothing ever reads them. A formal data race with no observable effect; classified and left.

Items 3 and 4 are only reachable through the STRUCT solver / rollout paths, which the default fast bitboard path never enters. The env knobs that would route a thread there (`OG_NO_BBSOLVE`, `OG_NO_FASTROLL`, `OG_LEAF`, `OG_DIFFTEST`, `OG_NO_WORLDSIM`) are therefore forbidden in Mode B env sets; `ORACLE_MT_ENV` in `oracleMtSession.ts` sets none of them.

## What is deliberately not built

- **No `OG_EXPLAIN` dump in Mode B.** Its wasm sink is `#ifdef CD_WASM_OVERLAY`, and the overlay aliases fixed static addresses into `solve_ws`, which is only legal when TLS is stripped. With real TLS it would be the native data race, so both defines are dropped from the Mode B target and the C accumulator replaces the dump.
- **No shared transposition table** (see above).
- **No per-batch standard error.** Mode B has no per-batch stream by design, so the `+-` text uses an n-based proxy at the finish-position spread Mode A measures rather than a running Welford estimate.
