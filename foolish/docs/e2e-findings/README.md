# findings — investigation narratives

> Preserved from the `claude/supabase-local-stress-test-e6snsa` branch (June 2026)
> before it was deleted. The code and tests it produced live in main; these are
> the investigation notes that would otherwise have been lost with the branch.

These are the write-ups behind the e2e suite: how each bug was found, reproduced,
and fixed (broadcast reordering, client cross-bout reconciliation, the cover
validate/execute mismatch, hand-order swaps, optimistic/revert, latency
thresholds, the subsystem probes).

They're historical narrative. The **codified, runnable** version of every
assertion here is `e2e/*.test.ts` (`npm run test:e2e`). Repro commands in these
docs that reference `tests/stress/...` point at the original exploratory harness,
which was folded into the real-code e2e suite and removed — the equivalents now
run against the actual deployed code. See `git log` for that history.

**Stale paths, once, for all eight (2026-09-18).**
These predate the A10 server split and the C game shape migration, so the code
anchors below have moved even where the mechanisms have not:

- Client merge and reconcile logic (`mergeTableBattles`, `mergeHandOrder`,
  `reconcileHandMemory`, `displayedHand`) now lives in
  `src/state/clientReconcile.ts`, not inline in `ServerContext.tsx`. Every
  `ServerContext.tsx:NNN` / `AnimationContext.tsx:NNN` line cite is stale.
- `server/impls/supabase/functions/_shared/utils.ts`,
  `functions/rearrange-hand/index.ts`, `src/replay/animate.ts` and
  `src/replay/decode.ts` no longer exist.
- The TS action handlers (`actions/cover.ts` and friends) are gone entirely -
  the rules live in the C kernel. The bug classes and the regression tests are
  still relevant; the file names are not.

The shipped mechanisms these findings produced are all still live:
`lastAppliedVersionRef`, `applyOptimisticOverlay`, `mergeTableBattles`,
`e2e/fuzz.test.ts`, `e2e/race_conditions.test.ts`.

One item is still open: `FINDINGS_PROBES2.md` Q8 - reconnect resync has no
debounce, so a flapping connection fires one REST refetch per resubscribe.
