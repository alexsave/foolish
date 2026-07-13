# Infinite Oracle — Mode B (shared-memory threads): build + latency findings

Follow-up to `docs/INFINITE_ORACLE_DESIGN.md` §8b. Mode A (the instance fleet)
shipped in PR #84. This document records the Mode B toolchain spike, the build,
and a head-to-head throughput benchmark answering the one question that gates
§8b.8: **is Mode B actually faster?**

## What was built

- **Toolchain spike** (§8b.3 / R18): a freestanding `-nostdlib` wasm with
  `-matomics --shared-memory --import-memory`, real TLS restored. clang/wasm-ld
  18 emit `__wasm_init_tls` / `__tls_size` / `__tls_align` / `__stack_pointer`;
  Node `worker_threads` instantiate one module over one shared
  `WebAssembly.Memory`, TLS is per-thread, and `__atomic_*` accumulation is
  exact across threads. **Freestanding wasm threads work.**
- **`oracle-mt.wasm`** (`make wasm-oracle-mt`, committed `public/oracle-mt.wasm.gz`,
  57 KB gz / 833 KB raw). Real TLS (no `-D_Thread_local=`), `-matomics`, no
  `OG_EXPLAIN`, no `CD_WASM_OVERLAY`. Shipped `bots/rules/guards.wasm` and the
  Mode A `oracle.wasm` are **byte-identical** (verified) — the MT1 allocator
  lock is a true no-op outside `-DFOOLISH_ORACLE_MT`.
- **C coordination** (`cnitro/wasm/wasm_oracle_mt.c`): MT1 atomic bump-allocator
  lock, MT2 per-thread `LegalMoves` (threads call `calculate_legal_moves` +
  `octogen_strategy_choose` directly, never the shared `g_moves`), MT3 mask +
  snapshot-hook init on the control thread, MT7 the `memory.atomic.wait/notify`
  thread loop, MT8 the control exports (`wasm_mt_reserve/warmup/setup/stop/
  total`). The control instance marshals `g_game` **once**; threads deliberate
  in parallel and accumulate a choose counter with one atomic add — **no
  per-batch JSON, no postMessage.**

## The benchmark

`scripts/oracle_bench.mjs` drives both modes from Node `worker_threads` (the
§8b.7 stand-in for the browser fleet) on the same tutorial mid-game decision, at
`OG_W1=24`, and measures octogen **choose-calls/sec** (each choose = one batch
of ~W1 worlds; the compute is identical between modes, so this is the latency
metric). Mode A replays exactly its per-batch work — write state/keys/logs,
choose, then read + `JSON.parse` the dump; Mode B threads just choose + one
atomic add.

| threads | Mode A (choose/s) | Mode B (choose/s) | Mode B / Mode A |
|--------:|------------------:|------------------:|:---------------:|
| 1       | 4,994             | 5,785             | **1.16×**       |
| 2       | 10,404            | 11,307            | **1.09×**       |
| 4       | 20,794            | 22,779            | **1.10×**       |

(4-core container; ~4 s per run. Both modes scale near-linearly with threads.)

## Verdict

**Mode B is faster, but modestly — ~10%, not a multiple.** The single-thread
1.16× isolates the per-choose overhead Mode B removes (the marshal + dump-read +
`JSON.parse` that Mode A pays every batch); at 2–4 threads the ratio holds near
1.10× because the octogen deliberation dominates the wall clock in both modes.
This matches the design's own prediction (§8b.8: "per-batch postMessage/JSON
overhead … unlikely at ~25 Hz total").

Caveats, stated openly:
- This is Node, not the browser. Node's in-process bridge has **less** overhead
  than the browser's real `postMessage` per batch, so the browser Mode-A cost is
  a bit higher and the real-browser Mode-B win is likely **somewhat larger** than
  10%. Conversely, this Mode-A path memcpys pre-captured marshal bytes instead of
  running the JS `__marshalGame` arithmetic, which slightly *understates* Mode A's
  cost. The two effects roughly cancel; ~10–16% is a fair estimate.
- This minimal Mode B measures choose-throughput; it does **not** yet emit the
  per-candidate scores (the MT4/MT5 accumulator). Making it a drop-in Mode A
  replacement needs that plus the browser COOP/COEP change (§8b.2) and the loader
  mode-selection (§8b.8). The choose compute measured here is representative.

**Recommendation:** given Mode A already converges in well under a second on this
hardware, a ~10% throughput win does not justify Mode B's cost (250–350 lines of
C, the COOP/COEP blast radius on the shared `/[game_id]` route, two artifacts,
and heap-region thread-stack safety) for latency alone — exactly the §8b.8
guidance. The build is preserved here as a proven foundation should the
coordination-in-C maintenance taste, or a future heavier per-decision workload,
change that calculus.
