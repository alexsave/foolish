# Hexogen — octogen's reserved successor scaffold

Hexogen (RDX, octogen/HMX's literal chemical sibling) is registered as
`hexogen`/`hx` (C-only, native eval + elo; no wasm/TS mirror yet). It is a
**thin wrapper over octogen's brain** (`src/hexogen_strategy.c` →
`octogen_set_hexogen` → `octogen_strategy_choose`), so every octogen fix flows
into it for free and octogen itself stays byte-identical (iron rule R1,
`docs/L1_SPEND_PLAN.md` §0).

It exists to be the landing site for the behavior-changing "spend the free
32 KiB" candidates in `docs/L1_SPEND_PLAN.md`: anything that changes how the bot
plays must be a NEW id, never an edit to octogen (which is pinned to its
TS-oracle mirror by `bot_parity`). Hexogen is that id.

## What makes hexogen: octogen + LEAFBOOK

hexogen = octogen's brain + the **S3 LEAFBOOK** (`LEAFBOOK.md`): a precomputed,
exhaustively-solved canonical endgame oracle probed at round-boundary ≤K-card
nodes before the TT. A book hit terminates the whole subtree with a PROVEN value
— no search, no TT traffic, no budget. It is proven-exact (V-book 10⁶ +
in-engine probe 2×10⁶, 0 mismatches), so it changes play only by resolving lines
the budget-limited search would have aborted.

Measured (`L1_SPEND_PLAN.md` Appendix A):
- pc2 latency: **26.72 ms/dec vs octogen's 30.08 — 11% faster.**
- pc2 strength (1000 paired vs espresso): diff **−0.010 ± 0.010**, win **81.0%
  vs 80.0%**, better/worse/eq **58/48/894** — neutral-to-slightly-better.
- pc4: byte-identical to octogen (the book resolves the same values octogen
  already reached within budget there).

This is the first L1 spend to clear R2 (a win over the control): faster, at ≥
strength. It works because it is a *different* lever from more search worlds.

## The world-raise (`HX_PCT`) — off by default, measured flat

hexogen also carries the S4 world-budget knob (`HX_PCT`, percent of octogen's
per-decision worlds; skipped at 100). As a standalone spend it is a documented
negative: @125% it is flat vs octogen (pc2 +0.010 ± 0.017, pc4 +0.015 ± 0.025)
at +17% latency, because the MC world budget is saturated — the same wall the
6× `octogen_oracle` hits (Ordnance Chart: +1.6 pp for 6× compute, research-only).
Default `HX_PCT=100`: no unfunded, control-losing spend baked in. The knob stays
for research; note LEAFBOOK's 11% headroom could fund a raise, but the axis is
saturated so it buys little.

## What lands here next

- **LEAFBOOK K=5** (`LEAFBOOK.md`) — 19,715 entries fit L1 via a 1 B/entry
  minimal-perfect-hash; reaches bigger, budget-binding subtrees (resolution
  gain, not just speed). Pipeline + safety gates already in place.
- **S2 bound side-table** — blocked; C5-v2 was characterized as a negative.

## Gates (hexogen has no TS mirror, so `bot_parity` does not cover it)

- Outcome ladder: V4 vs TT22, `CAND=12`, 0 win→loss over 3,000 espresso seeds.
- Strength arbiter: `cnitro_elo --pool=hexogen,octogen,semtex,espresso
  --pcs=2,4 --games=2000` (or the lower-variance paired `--control=octogen`),
  shipping bar = head-to-head ≥ 53% vs octogen with the elo CI clear of zero,
  at ms/decision ≤ octogen's.
- The seven shipped families stay byte-identical throughout (V0 + `bot_parity`
  7/7 on every commit).
