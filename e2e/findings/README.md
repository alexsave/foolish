# findings — investigation narratives

These are the write-ups behind the e2e suite: how each bug was found, reproduced,
and fixed (broadcast reordering, client cross-bout reconciliation, the cover
validate/execute mismatch, hand-order swaps, optimistic/revert, latency
thresholds, the subsystem probes).

They're historical narrative. The **codified, runnable** version of every
assertion here is `e2e/*.test.ts` (`npm run test:e2e`). Repro commands in these
docs that reference `tests/stress/...` point at the original exploratory harness,
which was folded into the real-code e2e suite and removed — the equivalents now
run against the actual deployed code. See `git log` for that history.
