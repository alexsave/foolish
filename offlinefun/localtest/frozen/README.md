# Frozen TypeScript bots

Retired reference implementations. Production bot play lives in the C kernel
(`cnitro/src/*_strategy.c`, compiled into `bots.wasm` and dispatched through
`supabase/functions/_shared/wasm/bots.ts`) — these TS sources are kept
verbatim as:

- the **oracles** for `e2e/bot_parity.test.ts`, which proves the kernel picks
  the exact same move as the TS original on every decision of thousands of
  seeded games (random, espresso, handwritten, simple_heuristic, champion,
  ultimate_champion, hacker);
- the engines for the offline research harnesses in `offlinefun/localtest/`
  (arenas, nitro training, profiling), which predate the kernel bridge;
- `cordite_core_old.ts` / `cordite_old_strategy.ts`: the pre-v2.3 cordite,
  kept for head-to-head strength baselines.

cordite/fulminate here are the TS ports of the C originals (SimGame engine);
the kernel runs the C bitboard implementation directly, so for those two the
parity contract is design-level (same belief, same budgets via `CD_BUDGET`,
same seat-profiling math), not move-for-move.

Do not edit these files — a drifted oracle makes the parity test meaningless.
The still-live TS strategies (`gpt`, `nitro`, console) remain in
`supabase/functions/_shared/strategies/`.
