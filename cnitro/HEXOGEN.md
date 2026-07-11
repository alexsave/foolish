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

## Status: HX_PCT=100 — currently == octogen

The only lever wired so far is the **S4 world-raise** (`HX_PCT`, percent of
octogen's per-decision sampled worlds; the block is skipped at 100). It was
measured and is a documented negative as a *standalone* spend
(`L1_SPEND_PLAN.md` Appendix A):

- @125% worlds: paired vs octogen (vs espresso, same deals) is **flat** —
  pc2 diff +0.010 ± 0.017 (1000 games), pc4 +0.015 ± 0.025 (800 games) — while
  costing **+17% pc2 latency**.
- pc2 is solver-dominated with a saturated MC budget (same wall `og_params`
  documents for octogen's own 2×/3× history), so a modest raise buys no
  strength and the raise that *would* (6× oracle) is nowhere near iso-latency.

So the default is `HX_PCT=100`: hexogen == octogen, with no unfunded,
control-losing spend baked in. The knob stays for research (`HX_PCT=<n>` env)
and for the day a node-saving lever funds a real raise.

## What lands here next

- **S3 LEAFBOOK** (`L1_SPEND_PLAN.md` §4) — a precomputed canonical ≤K-card
  endgame oracle. A book hit terminates a whole subtree with a proven value, so
  it saves *real* nodes — the node savings that can fund an iso-latency raise
  the standalone S4 lacked. Behavior-changing ⇒ hexogen-only. Gated by the
  V-book 100%-agreement safety gate and the elo arbiter.
- **S2 bound side-table** (`L1_SPEND_PLAN.md` §3, `C5_BOUNDS_HANDOFF.md`) —
  blocked until C5-v2 lands green.

## Gates (hexogen has no TS mirror, so `bot_parity` does not cover it)

- Outcome ladder: V4 vs TT22, `CAND=12`, 0 win→loss over 3,000 espresso seeds.
- Strength arbiter: `cnitro_elo --pool=hexogen,octogen,semtex,espresso
  --pcs=2,4 --games=2000` (or the lower-variance paired `--control=octogen`),
  shipping bar = head-to-head ≥ 53% vs octogen with the elo CI clear of zero,
  at ms/decision ≤ octogen's.
- The seven shipped families stay byte-identical throughout (V0 + `bot_parity`
  7/7 on every commit).
