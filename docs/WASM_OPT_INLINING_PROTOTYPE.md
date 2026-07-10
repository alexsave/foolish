# Post-link `wasm-opt` inlining — prototype & measurements

*Prototype on branch `claude/wasm-opt-inlining-prototype`. Question investigated:
can a post-hoc Binaryen (`wasm-opt`) inlining pass, run on the linked `.wasm`,
recover cross-object inlining that the mandatory no-LTO build leaves on the
table — and is it worth shipping?*

## Why this is the only stage that *can* cross-inline here

`cnitro`'s wasm modules compile each `.c` in isolation and link with `wasm-ld`.
LTO is banned (`cnitro/Makefile`, `WASM_FLAGS`): `-flto` corrupts the
indirect-function table for the address-taken `StrategyFn`/hook pointers —
`call_indirect` traps with "table index out of bounds", caught by the fuzz
suite. Verified on this branch: `-flto` *links* fine and even shrinks the
module, so the breakage is a silent runtime trap, not a build error.

That leaves no in-toolchain stage that inlines across the object boundary
(e.g. `cordite_sim.c` helpers into `cordite_strategy.c` callers). `wasm-opt`
runs on the already-linked binary, so it is the one pass that can.

## The mechanism (opt-in, off by default)

`WASM_POSTOPT` in `cnitro/Makefile` appends a `wasm-opt` invocation after each
module links. Empty by default ⇒ the recipe step is a literal no-op and the
default build is byte-identical to what ships (verified with `cmp`). Enable it
per build:

```
make wasm-bots WASM_POSTOPT="-O2 --inlining-optimizing"
```

`WASM_OPT_FEATURES` pre-enables `bulk-memory` (every module builds with
`-mbulk-memory`) plus the stock LLVM output features so `wasm-opt` will accept
the binary.

## Size (gzip is what ships — base64 embed / `.gz` asset)

| module | baseline gz | `-Oz --inlining` | `-Oz` | **`-O2 --inlining-optimizing`** | `-O3 --inlining-optimizing` |
| --- | --- | --- | --- | --- | --- |
| rules  | 13,681 | 13,690 | 13,666 | 13,714 | 13,693 |
| guards |  4,292 |  4,324 |  4,301 |  4,316 |  4,324 |
| bots   | 60,440 | 61,008 | 60,694 | **59,289** | 60,735 |

- **rules / guards are already at clang `-Oz`'s floor** — `wasm-opt` is
  no-op-to-worse. Plain `--inlining` *grows* gzip everywhere (inlining trades
  size for speed; gzip punishes the duplication).
- **`bots` is the only module that moves.** `wasm-opt` cuts ~20 KB of *raw*
  size, but gzip eats almost all of it. Only `-O2 --inlining-optimizing` beats
  the shipped gzip: **60,440 → 59,289 B (−1.9%)**. `-O3` and bare `--inlining`
  both regress it.

## Correctness — the same gate that caught LTO

Rebuilt + re-embedded `rules.wasm` and `bots.wasm` with
`-O2 --inlining-optimizing`, then ran the pure-kernel suites (no Postgres):

- `e2e/wasm_kernel_fuzz.test.ts` — asserts no `call_indirect` trap / memory
  corruption on malformed states (the exact failure mode LTO produced)
- `e2e/bot_parity.test.ts` — move-for-move C-vs-TS parity through the
  `StrategyFn` dispatch

**10/10 pass.** `wasm-opt`'s inlining is *sound* on the indirect-function table
where `wasm-ld` LTO was not. Bot parity holding ⇒ **zero strength change**
(moves are byte-identical). (`e2e/pass_parity` needs a local Postgres and is
unrelated — it fails the same way on the baseline assets in this sandbox.)

## Runtime (warm)

Deterministic micro-bench (`e2e/bench_wasm_inlining.ts`): drive full games
with the heavy MC bot **octogen**, timing only `wasmChooseMove`. Both modules
do byte-identical work (2,200 decisions); only codegen differs. 3 reps each,
Node 22 / V8:

| module | median `choose_ms` | ns / decision |
| --- | --- | --- |
| baseline (shipped) | ~24,120 | ~10.9M |
| `-O2 --inlining-optimizing` | ~23,096 | ~10.5M |

**~4% faster warm**, ~1 point above run-to-run variance. Caveat: this is V8
fully tiered up to TurboFan (**warm**). Production edge workers are the
short-lived **Liftoff** regime the Makefile's `-Oz` rationale targets; heavy
bots "self-tier-up mid-search" within a 2 s decision, so *some* of this is
reachable in prod, but confirming it needs the real `bench:bot-e2e` p50 harness
under the edge runtime, not this warm Node number.

## Verdict

Feasible and *safe* — it clears the correctness gate LTO failed — but the
payoff is **marginal**: −1.9% gzip and ~4% warm latency, on `bots.wasm` only,
and only with `-O2 --inlining-optimizing` (the intuitive `--inlining` / `-O3`
choices regress size). It adds a Binaryen build dependency for that. Left
**off by default**; the opt-in hook is in place so it can be A/B'd against the
real edge p50 harness before any decision to ship. rules/guards should never
enable it — they're already at the floor.
