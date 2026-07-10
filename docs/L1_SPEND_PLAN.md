# Spending the full 64 KiB — faster bot, or a smarter one

**Context.** The TT shrink round left the solver's table at **32 KiB**
(`TT12 + 2WAY + PACK8`, shipped) with strength ≥ the old 128 KiB table and
~6% faster decisions. The smallest L1d on target hardware is **64 KiB**
(`docs/WASM_L1_BUDGET.md` table). There is no prize for using less than the
cache line budget we already fit — so this plan spends the free **32 KiB** on
either **(A) speed** or **(B) intelligence**. The candidates compose; §6 gives
an exact 64 KiB allocation.

**Read `docs/C5_BOUNDS_HANDOFF.md` first** — one candidate here (S2) depends
on it, and its §3 explains the solver invariant that constrains everything.

---

## 0. Two iron rules (violating either wastes the whole effort)

**R1 — Never change how a shipped bot plays.** `bot_parity` pins the seven
shipped families (octogen, semtex, cordite, novichok, torpex, champion,
hacker) to exact-move TS-oracle mirrors. Anything that changes move choice —
bigger budgets, new evaluation, book probes that alter which lines resolve —
**must land as a NEW strategy id**, never as an edit to octogen. The registry
precedent is `STRAT_OCTOGEN_ORACLE=21` (`src/strategy.h:30-45` — a research
variant wrapping octogen with multiplied budgets). §7 names the new bot
**hexogen** (`STRAT_HEXOGEN 24`) — hexogen/RDX is octogen/HMX's literal
chemical sibling, which is exactly the relationship the bots will have.
Pure-speed changes (same moves, less time) may ship inside octogen only if
they are SIG-identical at the shipped config (the 2WAY/PACK8 standard).

**R2 — Every spend must beat the control.** The dumbest use of the 32 KiB is
doubling the TT (S0). It is nearly free to test and almost certainly worth
~nothing — the measured divergence curve is FLAT from TT11 through TT19 at
octogen's inherent ~3–6/10,000 floor (`docs/WASM_L1_BUDGET.md`), and the
shipped table already scored 0 outcome flips in 3,000 espresso games. Run S0
first anyway; it is the bar every clever candidate must clear, and the
fallback if they all fail.

### The measurement arbiter (use these, nothing else)

```sh
cd cnitro && CORE=$(make -s print-core)
# latency (CPU-time, contention-robust): ms/decision over 40 games
CD_LAT=1 CD_BUDGET=prod CD_RACE=1 CD_RACE_C=75 ./eval_X --strategy=octogen \
  --opp=espresso --players=2 --games=40 --seed-start=810000 2>&1 >/dev/null \
  | awk '/^LAT/{ns+=$2;d+=$3} END{printf "%.2f ms/dec\n", ns/d/1e6}'
# behavior: outcome flips vs TT22 truth (0 w->l over 3,000 = the shipped bar)
EXTRA='-D<flags>' CAND=12 BASE=22 OPP=espresso \
  bash tools/tt_divergence_viz/outcome_pair.sh 3000 700000
# strength: head-to-head elo (the final arbiter for hexogen)
build/cnitro_elo pool=hexogen,octogen,semtex,espresso pcs=2,4 games=2000
# solver internals: -DCD_TT_STATS prints insert/collision/abort census on exit
```

Wins are claimed only from: latency ↓ at identical SIG, or elo/win-rate ↑ at
V4-clean outcomes. Node-count and abort-rate drops are *mechanisms*, not wins.

---

## 1. S0 — control: TT13+2WAY+PACK8 = 64 KiB *(speed: ~0, strength: ~0 — measure to confirm)*

One flag: `-DCD_TT_BITS=13` with the shipped flags. 8,192 slots × 8 B.
Run V4 (3,000 seeds) + latency. Expected: statistically indistinguishable from
TT12 (the curve is flat above the ~TT10-11 knee; TT12 is already at 0/3,000).
Record the numbers; this is the bar. ~1 hour of compute, zero risk.

## 2. S1 — max-tail (C3 at full size) *(speed via fewer leaf recomputes; low risk)*

`CD_TT_TAILCACHE` (committed, validated) routes ≤6-card positions to a small
always-resident side cache; measured: the 512-entry tail absorbed **78% of all
insertions**, and main-table behavior went flat down to TT9. The scale gate at
a *tiny* 16 KiB total (tail512 + main TT10) still scored 1 outcome flip in
3,000 — i.e. the mechanism works even starved. Spend: keep the shipped 32 KiB
main table and grow the tail. `PACK8` already shrinks the tail automatically
(it shares `CdTTEntry`), so:

| `CD_TT_TAIL_N` | tail bytes | total |
|---|---|---|
| 1024 | 8 KiB | 40 KiB |
| 2048 | 16 KiB | 48 KiB |
| 4096 | 32 KiB | 64 KiB |

Sweep all three: build `-DCD_TT_2WAY -DCD_TT_PACK8 -DCD_TT_TAILCACHE
-DCD_TT_TAIL_N=<n> -DCD_TT_BITS=12`, measure latency + `CD_TT_STATS`
tail-insert/collision counts + V4. The win channel is **budget**: every tail
hit skips recomputing a leaf subtree, so decisions finish the same search in
fewer nodes → the latency probe should show it directly. Decision rule: take
the smallest tail within noise of the largest's latency; require V4 = 0 w→l.
If latency doesn't move at any size, the leaf recomputes were cheaper than
the probe overhead — document and drop (that is a real possibility: leaf
subtrees are tiny by definition).

## 3. S2 — bound side-table *(strength via fewer aborts; depends on C5-v2)*

When (and only when) `C5_BOUNDS_HANDOFF.md` §4 lands green: give bounds their
own packed table instead of competing for exact-table slots — 1,024–4,096
entries × 8 B (8–32 KiB), direct-mapped is fine (bounds are cheap to lose).
This attacks the **abort mechanism** — the root cause of every hard divergence
this session chased — with zero interference with exact retention. Gates are
C5-v2's §5 ladder; the strength claim must come from the elo arbiter. Note
per R1: if v2's root-re-solve changes any octogen move, the composed config
ships in hexogen, not octogen.

## 4. S3 — LEAFBOOK: a precomputed canonical endgame oracle *(intelligence; the star candidate)*

**Idea.** The census says **~91% of the solver's distinct positions are ≤6-card
endgames**. Their canonical space — after the rank-compaction + suit-symmetry
reduction already implemented for `CD_TT_RANKSYM` — is small enough to solve
EXHAUSTIVELY offline and bake into the wasm data segment as a read-only,
L1-resident **book**. A book hit terminates the whole subtree with a proven
value: no search, no TT traffic, no budget. This is a genuinely new capability
(offline knowledge), not a bigger cache — the "birth a new strategy" material.

**Key spec.** Probe only at *round-boundary* nodes: `num_battles == 0` (empty
table, `good_mask` clear) with `popcount(hand_a) + popcount(hand_b) <= K`.
Canonical form: attacker's hand + defender's hand as 52-bit masks →
rank-compact (one global monotone rank bijection over the ranks present —
reuse the `nr[]` construction from `sim_fingerprint_ranksym`,
`cordite_sim.c`) → canonicalize the 3 non-trump suits (trump/power suit stays
fixed) by sorted order rather than 6-perm-min (cheaper, same orbit). Store the
value **from the attacker-to-move's perspective**: 2 bits outcome
{win, loss, draw} + 4 bits distance (≤ 15 plies covers K ≤ 6) = 1 byte. Probe
returns `±(1000−(depth+dist))` rebased exactly like a TT EXACT hit; flip sign
if `S->me` is the defender.

**Why the RANKSYM latency objection doesn't apply here:** the ~2.2× fingerprint
cost that killed C2 as a *TT keying* scheme was paid at EVERY node. The book
canonicalization runs only at round-boundary ≤K-card nodes — a tiny fraction
of nodes, each of which *saves an entire subtree* when it hits.

**Execution order (feasibility gates first — do not skip):**

1. **Enumerate the space** (native tool, `tools/leafbook/enumerate.c`):
   generate canonical forms abstractly — choose `d ≤ K` distinct compacted
   ranks, assign each card a (suit-class, rank, owner∈{attacker,defender})
   with suit-classes canonicalized (trump distinguished, non-trump classes in
   sorted order) — and count distinct forms for K = 4, 5, 6. Expect order
   10³–10⁵; the decision table:
   | count(K) | book bytes @1 B/entry | verdict |
   |---|---|---|
   | ≤ 16,384 | ≤ 16 KiB | ship K, sorted-array + binary search |
   | ≤ 32,768 | ≤ 32 KiB | ship K, consider open-addressed hash w/ 16-bit tags |
   | more | — | drop to K−1 |
2. **Solve every form** with the existing solver at a huge budget
   (`cd_sim_solve`, budget 10⁸, assert no aborts) — the book is only as good
   as these values, so log any position that fails to resolve (should be
   none at K ≤ 6).
3. **Value-safety gate (V-book), non-negotiable:** sample ≥ 100,000 random
   *concrete* ≤K positions (random hands/trump/roles), solve each directly at
   big budget, compare to the book value through the canonicalization.
   **Require 100.000% agreement.** This empirically proves the orbit-invariance
   claim (rank-order isomorphism preserves game value) that the book rests on
   — the same claim RANKSYM used, but here it is load-bearing with no search
   to hide an error. One mismatch = stop, fix the canonicalization, restart.
4. **Wire the probe** into `sim_solve_rec` behind `-DCD_LEAFBOOK`, placed
   BEFORE the TT probe (a book hit is strictly better). Emit
   `CD_LEAFBOOK_STATS` hit counters.
5. **Gates:** probes change which lines resolve within budget ⇒ this is a
   behavior change ⇒ **hexogen-only** (R1). Ladder: V4 vs TT22 with the book
   ON at CAND=12 (0 w→l over 3,000), then the elo arbiter. Mechanism check:
   `CD_TT_STATS` should show the ≤6-card insert mass collapsing (it was 91%)
   and the abort rate dropping.

**Expected effect,** honestly: every solve that currently grinds through the
near-leaf layer gets its bottom K plies for free; budget saved compounds into
deeper win-hunts exactly where the 500459-class failures lived. Magnitude is
unknown until measured — that's what the elo gate is for.

## 5. S4 — iso-latency budget raise *(guaranteed strength; trivial mechanism)*

The shrink round made decisions ~6% faster (36.05 → 33.81 ms/dec), and S1/S3
save nodes on top. Convert the surplus into search: raise octogen's bitboard
solver budgets (`og_bb_win_budget = 20000`, `og_bbleaf_budget = 3000`,
`src/octogen_strategy.c:133,154`) and/or the world count by 1.15–1.4×,
calibrated so hexogen's measured ms/decision ≤ octogen's TODAY. More budget =
fewer aborts = strictly better resolution; the oracle variants (6× worlds)
already prove the mechanism's direction. This is the highest-certainty
strength spend and costs ~zero code — but it is a behavior change, so it IS
hexogen (R1). Calibrate with the latency probe, validate with the elo arbiter.

## 6. Composition — the recommended 64 KiB

```
32 KiB  exact TT      TT12 + 2WAY + PACK8      (shipped, unchanged)
16 KiB  LEAFBOOK      K per enumeration        (S3)
 8 KiB  tail cache    CD_TT_TAIL_N=1024        (S1)
 8 KiB  bound table   1,024 packed entries     (S2, after C5-v2)
------
64 KiB  solver-owned L1 working set
```

Honesty clause: L1 is shared with the active stack frames, `SimState`s, and
hot code — "64 KiB" is a working-set discipline, not hardware pinning. The
claim to defend is *small hot working set*, measured by the latency probe,
not by cache-counter theater.

## 7. Birthing hexogen — the checklist

1. `src/strategy.h`: `#define STRAT_HEXOGEN 24` + name map entry
   (`strategy.h:102` style: `"hexogen" || "hx"`), comment stating composition.
2. `src/hexogen_strategy.c` — wrap octogen the way `octogen_oracle` does
   (multiplied budgets + the `-DCD_LEAFBOOK`/side-table flags), or a config
   struct passed via ctx; keep it a thin wrapper so octogen fixes flow in.
3. `main_eval.c` dispatch case + `Makefile` CORE list.
4. TS: add `hexogen: 24` to the `STRAT` map (`supabase/functions/_shared/wasm/bots.ts:~48`)
   when (and only when) it ships to production selection.
5. Validation: hexogen has NO TS mirror, so `bot_parity` does not cover it —
   its gates are the outcome ladder (V4 vs TT22: 0 w→l/3,000) and the elo
   arbiter: `cnitro_elo pool=hexogen,octogen,semtex,espresso pcs=2,4
   games=2000`, shipping bar = head-to-head ≥ 53% vs octogen with the elo CI
   clear of zero, at ms/decision ≤ octogen's.
6. The seven shipped families stay byte-identical throughout (verify: V0 +
   `bot_parity` 7/7 on every commit).

## 8. Sequencing, effort, expected value

| step | effort | risk | expected win | do it when |
|---|---|---|---|---|
| S0 control | hours | none | ~0 (the bar) | first |
| S4 budget raise → hexogen skeleton | 0.5 day | low | high-certainty strength | second — it also creates the hexogen scaffold the others land in |
| S1 tail sweep | 0.5 day | low | some speed; maybe none | third |
| S3 LEAFBOOK | 2–4 days | medium (gated hard) | the ceiling; unknown until elo | fourth — feasibility gate is cheap, do that early |
| S2 bound table | after C5-v2 | medium | abort-rate → strength | last |

Stop rules: any V4 w→l flip = stop that candidate and bisect. Any candidate
that can't beat S0's numbers gets documented (measured numbers in this file,
appendix-style) and dropped. If everything fails, ship S0's TT13 — a free
half-bit of collision margin — and close the file with the numbers. A
documented negative is a valid deliverable; this session produced three
(ORDER2/3, ADAPT, RANKSYM-as-keying) and each one narrowed the next search.
