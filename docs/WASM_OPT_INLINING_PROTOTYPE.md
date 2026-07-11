# Post-link `wasm-opt` pass on `bots.wasm` — investigation & result

*Started as: can a post-hoc Binaryen (`wasm-opt`) inlining pass, run on the
linked `.wasm`, recover cross-object inlining the mandatory no-LTO build leaves
on the table? Ended as: inlining buys no measurable runtime, but the same pass
shrinks the shipped `bots.wasm` gzip **−6.5%**, so it ships as a size/cold-start
win.* Numbers below are on the current `bots.wasm` (after the strategy-cull that
dropped 6 research bots); an earlier pre-cull pass measured a smaller −1.9%.

## Why this is the only stage that *can* cross-inline here

`cnitro`'s wasm modules compile each `.c` in isolation and link with `wasm-ld`.
LTO is banned (`cnitro/Makefile`, `WASM_FLAGS`): `-flto` corrupts the
indirect-function table for the address-taken `StrategyFn`/hook pointers —
`call_indirect` traps with "table index out of bounds", caught by the fuzz
suite. Verified during this work: `-flto` *links* fine and even shrinks the
module, so the breakage is a silent runtime trap, not a build error. That
leaves no in-toolchain stage that cleans up / inlines across the object
boundary; `wasm-opt` runs on the already-linked binary, so it is the one pass
that can.

## What ships

`cnitro/Makefile` runs `wasm-opt` on the linked `bots.wasm` by default
(`WASM_BOTS_POSTOPT ?= -O2 --inlining-optimizing`); `rules`/`guards` stay off
(they're at clang `-Oz`'s floor — see below). `WASM_POSTOPT=<flags>` is a manual
override for any module. **Requires binaryen (`wasm-opt`) on PATH to rebuild
`bots.wasm`** — CI does not rebuild the wasm (it ships the committed
`bots.wasm.gz`), so this is a build-time dep only for whoever regenerates the
asset. `WASM_OPT_FEATURES` pre-enables `bulk-memory` (every module builds with
`-mbulk-memory`) plus the stock LLVM output features so `wasm-opt` accepts the
binary.

## Size (gzip is what ships — `.gz` asset / base64 embed)

Variant sweep, `bots.wasm` gzip -9 bytes (measured before the final rebase onto
main; the *shipped* asset after rebasing is **49,711 → 46,498 B, −6.5%** — a
later main change nudged the absolute bytes, the relative picture is unchanged):

| variant | bots gz | Δ vs baseline |
| --- | --- | --- |
| baseline (no pass) | 49,446 | — |
| `-Oz --inlining` | 46,354 | −6.3% |
| `-Oz` | 46,110 | −6.7% |
| **`-O2 --inlining-optimizing`** (shipped) | **46,262** | **−6.4%** |
| `-O3 --inlining-optimizing` | 46,378 | −6.2% |

- **`bots` is the only module that moves.** `rules`/`guards` are already at
  clang `-Oz`'s floor — every `wasm-opt` variant is no-op-to-worse there, so
  they stay off.
- On this (slimmer) module *every* variant now shrinks gzip ~6–7% — the win is
  `wasm-opt`'s general cleanup, not inlining specifically.
- Plain `-Oz` is ~150 B smaller than the shipped variant but is
  **size-over-speed**; its warm latency wasn't cleanly verifiable (the sandbox
  was under load during that measurement), so the speed-oriented
  `-O2 --inlining-optimizing` is chosen instead for a negligible size cost.

## Correctness — the same gate that caught LTO

Ran the pure-kernel suites (no Postgres) against the shipped asset:

- `e2e/wasm_kernel_fuzz.test.ts` — no `call_indirect` trap / memory corruption
  on malformed states (the exact failure mode LTO produced)
- `e2e/bot_parity.test.ts` — move-for-move C-vs-TS parity through the
  `StrategyFn` dispatch

**6/6 pass.** `wasm-opt`'s inlining is *sound* on the indirect-function table
where `wasm-ld` LTO was not; bot parity holding ⇒ **zero strength change**
(moves byte-identical). (`e2e/pass_parity` needs a local Postgres and is
unrelated — it fails the same way on baseline assets in this sandbox.)

## Runtime — no measurable win

`bench:bot-e2e` p50 (full server pipeline vs real Postgres: `loadCompleteGame`
→ belief hydrate → kernel choose → apply → CAS commit), four belief/MC bots,
`BENCH_BOT_MOVES=40`, 3 reps, median p50 (ms), measured while the sandbox was
quiet:

| bot | baseline | `-O2 --inlining-optimizing` | Δ |
| --- | --- | --- | --- |
| octogen  | 64.5 | 61.9 | within noise |
| semtex   | 12.6 | 12.3 | within noise |
| cordite  | 15.5 | 16.0 | within noise |
| fulminate| 11.6 | 11.8 | within noise |

All four bots are within run-to-run variance. A warm micro-bench that isolated
`wasmChooseMove` (`e2e/bench_wasm_inlining.ts`) showed ~4% on the raw kernel,
but that washes out end-to-end: the heavy thinkers' p50 is dominated by belief
hydration + MC world allocation + the DB round-trip, not the inlinable kernel
math. **So the win is module size / cold-start (smaller module ⇒ faster gunzip
+ TurboFan compile per short-lived edge worker), not per-move latency.** An
earlier pre-cull run saw a −9% blip on cordite that did not reproduce after the
rebase — treat it as noise.

## Verdict — shipped

Enabled by default for `bots.wasm` only: **−6.5% gzip, runtime-neutral,
correctness 6/6, zero strength change.** Cost is a binaryen build dependency for
regenerating the bots asset (CI unaffected). The win is cold-start/size, not
latency — the heavy bots that approach the 2 s cap are unmoved. `rules`/`guards`
stay off (already at the floor). `WASM_POSTOPT` remains as a manual override for
experimenting with other flags/modules.
