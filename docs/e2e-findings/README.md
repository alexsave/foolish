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
