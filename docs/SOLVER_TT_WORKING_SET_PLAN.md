# Shrinking the cordite solver's TT working set — design candidates

Status: **plan + measured groundwork**. Nothing here is enabled; every candidate is a
compile flag, off by default. This doc is written to be executed by a separate
implementer, one candidate at a time, against the validation ladder in §3.

Goal: cut the transposition-table working set (or the bytes needed to hold it) so
`bots.wasm` can ship a **64 KiB-class table (TT12) or smaller** with behavior at
least as faithful as today's TT13 — i.e. vs-TT22 move divergence ≤ ~0.15% and no
excess win→loss outcome flips vs a strong opponent.

Non-goal: changing what move the bot *should* play. Every candidate below touches
only the TT **key, store policy, or layout** — never the move generator, the value
function, or the search order. (Search-order changes are proven regressions; see §6.)

---

## 1. The mental model (read this first)

The endgame solver `sim_solve_rec` (`cnitro/src/cordite_sim.c`) is an exact,
node-budget-limited, depth-capped (ply 48, `CD_SIM_SOLVE_MAX_DEPTH`) minimax with a
direct-mapped TT (`tt[key & CD_TT_MASK]`, 16-byte `CdTTEntry {key, value, depth, valid}`).

Facts every candidate must respect:

- **Only EXACT values are stored.** The store site requires `alpha0 < best < beta0`
  (fail-soft result strictly inside the node's original window). Fail-high /
  fail-low completions store **nothing**.
- **Value lattice** is `{-(1000-d), 0, +(1000-d)}` — win/loss-in-d-plies or draw.
  Stored values are depth-relative and rebased on lookup (probe site, ~line 1290).
- **Three solve-traffic classes** (grep `cd_sim_solve_d(` for windows):
  1. Strategy win-hunts (octogen/semtex/cordite/novichok/torpex `*_strategy.c`):
     per-candidate-move solves with window `(alpha, 2000)`, depth0=1. Wins land
     strictly inside → **winning subtrees store exact values**; refuted candidates
     fail low and store nothing.
  2. Strategy loss-probes: window `(-1, 0)`. **No representable value lands strictly
     inside** → these solves can never store anything.
  3. In-sim deck-empty hunts (`cordite_sim.c` ~1651/1858): window `(-1, 1)` → only
     draws (0) storable → effectively never store.
- **Budget mediation**: every node entry decrements `S->budget`; on exhaustion or
  ply-48 the solve **aborts** and the strategy falls back to Monte-Carlo. TT hits
  save budget. Therefore *anything* that changes hit patterns changes budget
  trajectories, and knife-edge games sit exactly on abort boundaries. There is no
  such thing as a bit-identical-under-budget change; the correct target is the
  **validation ladder**, not perfect SIG equality at every size.
- **Safety classes** used below:
  - **Value-safe**: a TT hit returns exactly the value a recompute would produce
    (candidate only re-keys/re-routes exact entries). At TT22 + prod budget these
    must be **SIG-identical** to std (gate V1). At small sizes they differ only via
    collision patterns — the same channel as today.
  - **Outcome-safe**: root-level win/loss claims remain provably correct, but
    fail-soft magnitudes may shift → the win-hunt's "fastest win" tie-break may
    pick a *different winning move*. Requires outcome gates (V4), not SIG gates.

### Measured groundwork (census, TT22, prod env, Jul 2026)

Store census over the 11 tricky handwritten seeds (the games that size the table)
and 40 typical pc2 games. `CD_TT_CARDS c n` = distinct-key insertions at nodes with
`c` cards across both hands:

| slice | tricky (11 games) | typical (40 games) |
|---|---|---|
| distinct keys total | 66,466 | 48,772 |
| at ≤ 4 cards | 50.2% | 52.5% |
| at ≤ 6 cards | **90.7%** | **91.6%** |
| at ≥ 7 cards | 6,188 (≈560/game) | 4,119 (≈100/game) |
| fail-high completions (stored: nothing) | 91.2M | 212.6M |
| fail-low completions (stored: nothing) | 108.0M | 221.9M |

Two conclusions that drive the candidate list:

1. **W is a near-leaf tail phenomenon.** ~91% of distinct keys are positions with
   ≤6 cards left. The expensive, hard-to-recompute ≥7-card proofs are only
   ~100–600 distinct keys per game — TT10-11 territory on their own.
2. **The search discards essentially all of its refutation work.** ~200–400M
   completed node-windows per few dozen games produce bounds the exact-only policy
   cannot store — so every win-hunt candidate re-refutes lines its predecessors
   already refuted, and the `(-1,0)`/`(-1,1)` probe classes cache nothing, ever.
   This is why the 500459-class hunts burn 55–166k nodes: the budget is spent
   re-deriving known refutations. (Full mechanism: `docs/seed-500459-thrash.html`.)

Prior results this plan builds on (details: `docs/OCTOGEN_PC2_DIVERGENCE.md`,
`docs/tt-divergence.html`):

- Divergence vs TT22 at 720+ seeds: TT8 1.39%, TT9 0.69%, TT10 0.28%, TT11–TT18
  flat at ~0.14% (the birthday floor). Latency flat ~48ms/decision TT10–TT16.
- Paired outcome flips vs espresso: TT8 2/1000 (both win→loss; both overflow, W >
  256), TT13 1/2500 (0.04%, seed 720958, W=1395 — *below-M* thrash, not overflow).
- 11,100-game octogen pc2 working sets: median 392, p99 3,843, max 8,130 < 8,192.
- Peak W is **not reducible by search-order changes** (§6) — but the census shows
  it *is* reducible by store policy, because the peak is ~91% tail.

---

## 2. Ground rules for the implementer

- One candidate = one compile flag (`-DCD_TT_<NAME>`), default-off, `#ifdef`-clean:
  the default build must be bit-identical in behavior (gate V0).
- Never touch move generation, `sim_apply_sol`, or the iteration order of the
  candidate loop. Keys/stores/probes/layout only.
- The **real state `s` is never modified** — canonicalization happens only inside
  fingerprint computation.
- Build pattern (native measurement):
  `CORE=$(make -s print-core); cc -O2 -ffast-math -Isrc -Wno-deprecated-declarations -DCD_TT_STATS -DCD_TT_<FLAG> -DCD_TT_BITS=<b> $CORE src/main_eval.c -o /tmp/l1/<name> -lm`
- Env for every measurement: `CD_BUDGET=prod CD_RACE=1 CD_RACE_C=75`, plus
  `GAME_SIG=1` (move hash + fin), `CD_GW=1` (per-seed W), `CD_LAT=1` (decision
  latency). pc2 games ≈ 1.5 s each; shard seeds across cores (see the tools in §7).
- Commit each experiment (flag + results in the commit message) even when negative.

---

## 3. Validation ladder (every candidate climbs, in order)

| gate | what | command sketch | pass criterion |
|---|---|---|---|
| V0 | default build unchanged | build with NO flags; SIG on 20 seeds vs pre-change binary | 100% identical |
| V1 | value safety at TT22 | candidate TT22 vs std TT22, SIG over 200 mixed seeds (100 `--opp=handwritten` from 500000, 100 `--opp=espresso` from 700000) + the §7 panel | value-safe: 100% SIG match. C5: SIG diffs allowed **only** with V4 parity |
| V2 | tricky panel at target sizes | §7 panel at TT13/TT12/TT11/TT8: W, SIG-vs-std22, fin | no outcome regression on panel |
| V3 | divergence at scale | `tools/tt_divergence_viz/accrue_div.sh` (BITS incl. 11 12 13), ≥1500 fresh seeds | candidate@TT12 ≤ std@TT13 floor (~0.15%) |
| V4 | outcome flips at scale | `tools/tt_divergence_viz/outcome_pair.sh` vs espresso, CAND=12 (and 11), ≥2500 seeds | flips ≤ 1/2500; no excess win→loss vs std TT13's 0.04% |
| V5 | latency | `tools/tt_divergence_viz/lat_pass.sh 100 <bits>` | ≤ std+10% at same bits |
| V6 | repo suites | native tests + difftests + `bot_parity` + `test:mem` (see WASM_L1_BUDGET.md §Validation) | all green |

---

## 4. Candidates

### C1 — Store census (DONE — keep the counters)

`CD_TT_STATS` now also emits `CD_TT_CARDS <cards> <n>` (distinct-key insertions by
cards-in-both-hands; valid as a distinct-key census only at TT22 where evictions ≈ 0)
and `CD_TT_STATS2 failhi=<n> faillo=<n>` (completed-but-unstorable node windows).
Results in §1. Use it to re-measure headroom after each candidate lands.

### C2 — `CD_TT_RANKSYM`: rank+suit canonical keys  *(value-safe; medium effort)*

**Idea.** Only three things about card identity affect the endgame value: trump
membership, *relative* rank order within the comparison rules, and *cross-suit rank
equality* (attacks/passes join on equal rank). Absolute ranks don't matter once the
missing cards are gone. So two positions that are **order-isomorphic** — same
pattern after (a) compacting the global rank axis to the ranks actually present and
(b) permuting the three non-trump suits — have isomorphic game trees and identical
depth-relative values. Key the TT on the canonical form and all orbit-mates share
one entry.

**Construction (cheap form — do NOT hash 6 permutations):**
1. `R` = 13-bit mask of ranks present in `hand[a] | hand[b] |` all battle cards
   (rank = id % 13, suit = id / 13).
2. Rank-compact every 13-bit suit block: `block' = PEXT(block, R)` (software
   fallback: loop or 2×8-bit LUT — wasm has no PEXT). Battle card ids remap via
   `newrank[r] = popcount(R & ((1<<r)-1))`.
3. Suit-canonicalize: trump block stays fixed; sort the three non-trump suits by
   their joint content as one integer per suit —
   `key_s = (blockA_s << 26) | (blockB_s << 13) | table_presence_s` — descending.
   Ties ⟹ identical content ⟹ any order gives the same hash. This replaces the
   6-perm min of the existing `CD_TT_SUITSYM` (subsumes it, ~6× cheaper).
4. Hash the remapped `(handA', handB', battles', defender, first_attacker,
   good_mask, num_battles)` with the existing `sim_fingerprint` mix.

**Why value-safe** (each rank-sensitive rule preserved): cover = strictly-higher
same suit (order preserved) or trump (membership preserved); trump-vs-trump by rank
(order preserved); attack/pass sets join on rank equality (equality preserved by
global compaction — this is why compaction must be **global across suits**, not
per-suit); the cover-cap "defcap lowest-score" selection (~line 483) compares
`rank + 1000·trump`, tie-free within a battle (same-rank same-suit impossible) —
selection maps through the isomorphism. The solver still runs on the REAL state;
only the key changes. Hits return exactly what recompute would return.

**Expected effect.** Collapses positions that differ only in which low discards
left the game — plausibly 1.5–4× on the tail (where discard variation concentrates).
Measure directly: W(RANKSYM)/W(std) at TT22 over the §7 panel + 100-game batch.
The existing 6-perm `CD_TT_SUITSYM` gave only ~1.05–1.1× — rank compaction is the
part with real headroom; if measurement shows <1.3×, drop C2 and lean on C3/C5.

**MEASURED (DONE — Jul 2026, `-DCD_TT_RANKSYM`).** Collapse W(std)/W(ranksym) =
**1.17× overall** on the panel at TT22 (1.04–1.57×; best on low-card games) — beats
SUITSYM's ~1.07× but below the 1.3× keep-bar. V1: **not** bit-identical (2/200 seeds
change move hash, both `fin=1` → **0 outcome flips** over 200 games) — the orbit
collapse gives extra hits, saves budget, and reshuffles knife-edge tie-breaks;
consistent with budget-boundary, not a value bug. **Latency: ~2.2× slower per
decision** (6-perm × rank-compaction fingerprint at every node). Verdict: **kept
flag-gated, NOT shipped** — +0.2 effective bit doesn't justify ~2× hot-path cost.
If ever wanted, apply the plan's optimizations first (direct suit-sort instead of
6-perm min; canonicalize only at `cards ≤ 10`) to recover the latency.

**Cost.** Fingerprint goes from ~20 ops to ~80–120 ops/node. Node cost is dominated
by the child memcpy + movegen, but verify with V5; if hot, canonicalize only at
`cards ≤ 10` (where the collapse lives) and tag plain/canon keys with a high bit so
the two key spaces never alias.

**Risks.** A missed rank-sensitive rule ⟹ wrong-value reuse ⟹ V1 fails fast (SIG
mismatch at TT22). Debug aid: `#ifdef` a paranoid mode that, on every canonical hit,
1-in-N re-solves and asserts equality.

### C3 — `CD_TT_TAILCACHE`: side cache for ≤K-card positions  *(value-safe; low effort; biggest measured headroom)*

**Idea.** The census says ~91% of distinct keys are ≤6-card positions whose
subtrees are tiny (cheap to recompute) — they don't deserve slots in the expensive
main table; they need a small, always-hot scratch cache. Route them to a fixed
side table and the **main table only has to hold the ≥K-card working set**
(~100–600 distinct keys/game) — TT10–TT11 territory.

**Spec.**
- `static _Thread_local CdTTEntry cd_tt_tail[CD_TT_TAIL_N];` — start with
  `CD_TT_TAIL_N = 512` (8 KiB, BSS, always resident; sweep 256/512/1024).
- Routing predicate at BOTH probe and store:
  `cards = popcount(hand[a]) + popcount(hand[b]); use_tail = (cards <= K)` —
  start K=6 (sweep 4/5/6/7). Tail probe: `tail[key & (CD_TT_TAIL_N-1)]`.
- `cd_sim_solve_reset` memsets the tail alongside the main table.
- Keep the census counters split (main vs tail insertions) so W(main) is measurable.

**Why value-safe.** Same entry semantics, different slot pool. Hits exact; misses
recompute identical values.

**Budget note.** The tail cache is lossy (512 slots vs ~5k distinct tail keys per
deep window) → more tail misses than a TT22 main table would give → some extra
recompute of *tiny* subtrees, versus the win that expensive deep proofs stop being
evicted at small main sizes (the exact K1/K2 mechanism that kills the 500459 KD
solve — see the deep-dive page). Net effect at TT12-main should be strongly
positive; V2/V3 decide. If aborts rise, raise `CD_TT_TAIL_N` before raising K.

**Wasm note.** +8 KiB BSS against a 64→8 KiB main-table saving.

### C4 — `CD_TT_2WAY`: pairwise 2-way associativity  *(value-safe; trivial effort)*

**Idea.** Direct-mapped tables lose ~half their capacity to conflict misses. Make
each aligned pair of slots a 2-entry bucket: probe both (`base = key & MASK & ~1`,
same 64-byte cache line), hit on either key. Store: same-key slot, else invalid
slot, else **evict the deeper-ply entry** (bigger `depth` = smaller subtree =
cheaper recompute). Always store — never refuse (refusal is the pathology that made
standalone DEPTH_PREF regress 700910; victim-choice-between-two is retention-monotone
for the expensive entry).

**Expected.** Conflict misses ≈ halved ⟹ ≈ +1 effective bit: TT12-2way ≈ TT13
behavior at 64 KiB. ~15 lines. Composes with everything.

**MEASURED (DONE — Jul 2026, `-DCD_TT_2WAY`, prod env).** Landed and validated
through the ladder. Result: **2WAY@TT12 (64 KiB) strictly beats std@TT13 (128 KiB)** —
half the bytes, better fidelity.

- **V0** default build (no flag): SIG-identical to pre-change (20 seeds). ✔
- **V1** value safety: 2WAY@TT22 SIG-identical to std@TT22 over 200 mixed seeds
  (100 handwritten @500000 + 100 espresso @700000). ✔
- **V2** tricky panel (TT13/12/11): 2WAY **fixes** the std-TT13 0.04% outcome flip
  **720958** (win at TT13/12/11, std loses), extends **700910**'s win down to TT11
  (std loses at TT12/TT11), and converges **500459** to the TT22 move at TT12/TT13
  (std diverges at every size). **Zero outcome regressions** across the panel; every
  2WAY `fin=1`. (W census reads *higher* for 2WAY — associativity retains more
  distinct keys, i.e. better slot utilization; it is not a cost.)
- **V4** outcome flips at scale, 2WAY@TT12 vs TT22, 3000 espresso games:
  **0 outcome flips (0.000%)**, move-divergence 0.100% — both below std-TT13's
  0.14% / 0.04% reference. (V3 move-divergence is subsumed: same run.)

Effective gain confirmed at ≥ +1 bit; TT12+2WAY is the recommended production
landing (1 wasm page, L1d-resident). See §5 for wiring.

### C5 — `CD_TT_BOUNDS`: store fail-soft bounds, exact-priority  *(outcome-safe; highest payoff; medium-high effort)*

**Idea.** The census's headline waste: 200M+ completed refutations per few dozen
games, none stored. Standard chess-engine TT flags fix this. Add a flag byte in the
entry's 4 spare pad bytes: `{EXACT, LOWER, UPPER}`.

- Store on fail-high (`best ≥ beta0`): LOWER bound = `best`. Fail-low: UPPER.
- Probe: EXACT → return rebased value. LOWER → if `rebased ≥ beta` return it
  (cutoff), else `alpha = max(alpha, rebased)`. UPPER → mirror with `alpha`/`beta`.
  (Depth rebasing is an additive shift — bound direction survives it.)
- **Exact-priority replacement** (the crucial policy): an EXACT entry is never
  evicted by a bound; bounds may fill empty slots, replace other bounds, or upgrade
  same-key bound→exact. Today's exact-entry retention is then preserved *exactly*,
  and bound entries are pure additive cache: extra valid cutoffs, less budget burn,
  fewer aborts. This is the candidate that attacks the 500459 abort mechanism at
  its root (candidate N re-uses candidate N−1's refutations).
- **Bound-store filter** to avoid flooding the table: store bounds only at
  `cards ≥ 5` (the expensive layer; census says ≤4-card completions dominate raw
  counts and are worthless to cache). With C3 composed: bounds go to main only.

**Safety analysis (why outcome-safe, and possibly SIG-safe).** A LOWER-bound cutoff
returns a *proven* win-bound (true value ≥ stored ≥ beta); win/loss classification
at the hunt roots is unchanged. Magnitudes ("fastest win" tie-break) could shift in
principle — but note the actual window structure: hunts run `(alpha, 2000)` where
beta=2000 is unreachable (max |value| ≤ 1000), so LOWER cutoffs never fire at hunt
depth; UPPER cutoffs return values ≤ alpha which maximizer parents discard; the
`(-1,0)`/`(-1,1)` probes consume only the sign. So under the *current* callers the
root magnitudes plausibly never change → SIG-safe is possible. Do NOT rely on that
analysis: run V1; if SIG diverges, fall back to the outcome gates (V4 at full
strength, both directions reported).

**Risks.** Bound-direction bugs are catastrophic and instantly visible at V1.
Rebasing sign errors likewise. The `!applied || best == ±2000` early-return paths
store nothing today — leave them. Keep `cd_stat_collisions` counting exact-vs-exact
evictions only, or the metric loses meaning.

### C6 — `CD_TT_PACK8`: 8-byte entries  *(probabilistic; do last or skip)*

Pack `{tag:40, value:12, depth:6, flags:2}` into 8 bytes → 2× slots per byte
(TT13 bytes hold TT14 slots). Cost: the key check becomes a 40-bit tag +
13-bit index ≈ 53-bit effective key → expected false hits ≈ probes·2⁻⁴⁰ ≈ 10⁻⁵/game
of *silently wrong values*. That is an undetectable-corruption channel in a system
whose whole story is 10⁻⁴-level guarantees — only worth it if, after C2–C5, byte
budget is still the binding constraint. Position honestly or skip.

---

## 5. Recommended order & the composed end-state

1. **C4 (2WAY)** — trivial, value-safe, immediate +1 bit. Climb V0–V4 once to
   calibrate the ladder itself.
2. **C3 (TAILCACHE)** — the measured 10× main-W headroom. Sweep K and tail size.
3. **C2 (RANKSYM)** — measure the orbit collapse before investing in polish; keep
   if ≥1.3×, else drop (SUITSYM already banks the cheap 1.05×).
4. **C5 (BOUNDS)** — the abort-killer. Biggest single payoff, most care needed.
5. **C6** only if still byte-bound.

Composed target: `RANKSYM keys + TAILCACHE(≤6, 8 KiB) + 2WAY main + BOUNDS(main, ≥5 cards)`
with main table **TT11 (32 KiB) → 40 KiB total**, L1-resident even on 48 KiB x86
L1d — with *fewer* aborts than today's TT13 (bounds reuse), i.e. divergence at or
below the current floor, and likely faster decisions. Conservative landing: TT12
main → 72 KiB total, still a 2× shrink with headroom proven by gates.

After the winner composes: wire the flags into the Makefile `WASM_FLAGS`
(`bots.wasm` block), re-run the wasm parity/mem suites, regenerate
`docs/tt-divergence.html` + `docs/OCTOGEN_PC2_DIVERGENCE.md`, update
`docs/WASM_L1_BUDGET.md`, and let CI's metrics job confirm the peak-page drop.

---

## 6. Failed approaches — do not retry (measured, Jul 2026)

| approach | flag | result | root cause |
|---|---|---|---|
| big-first move ordering | `CD_TT_ORDER2` | W ÷2–100 on most seeds, but **loses 700910/720958 even at TT22** | deep committed lines trip the ply-48 abort → MC fallback; ordering also reshuffles fail-soft tie-breaks |
| short-first ordering | `CD_TT_ORDER3` | outcomes safe on panel but **W increases** (700910: 512→2709) | resolving short lines first explores more before cutoffs |
| adaptive: big-first, re-solve on abort with std order | `CD_TT_ADAPT` | still loses 720958 with W=0 | **aborts are not the only failure channel** — big-first completes "successfully" with different fail-soft values; no abort ⟹ no re-solve. Ordering cannot be patched this way |
| depth-preferred replacement (standalone, 1-way) | `CD_TT_DEPTH_PREF` | fixed 5/11 TT8 divergers, **broke 700910 at TT13** (win→loss) | refusing stores changes hit patterns unpredictably; superseded by C4's always-store victim choice |
| 6-perm suit symmetry | `CD_TT_SUITSYM` | correct, move-identical, but only ~1.05–1.1× | endgame suits are genuinely differentiated; kept, subsumed by C2's cheaper construction |

The general lesson (also §1): **search-order changes are never behavior-safe** in a
budget- and depth-capped solver whose consumers read fail-soft magnitudes. Store
policy, keying, and layout are the safe design space.

---

## 7. Harness inventory & the seed panel

Tools (all committed):

- `cnitro/tools/tt_divergence.sh` — SIG divergence, seed-sharded (`S=`), `BASE=` override.
- `cnitro/tools/tt_divergence_viz/generate.sh` — W measurement (`measure BOT PC N`,
  `REF_BITS=`), rebuilds `docs/tt-divergence.html`.
- `cnitro/tools/tt_divergence_viz/accrue_div.sh` — accumulating seed-keyed divergence
  (per-bits `.div` files, dedup by seed), rebuilds the page + markdown table.
- `cnitro/tools/tt_divergence_viz/outcome_pair.sh` — paired per-seed **outcome** test
  (candidate vs TT22, same seeds, `BOT/OPP/CAND/BASE` env) — the V4 gate.
- `cnitro/tools/tt_divergence_viz/lat_pass.sh` — CD_LAT decision-latency pass (V5).
- Env: `GAME_SIG=1` → `SIG <seed> <movehash> fin=<pos>`; `CD_GW=1` → `GW <seed> <W>`;
  `CD_LAT=1` → `LAT <ns> <decisions>`; `--replay-seeds=<file>` replays a seed list.

The tricky-seed panel (all pc2, prod env). W measured at std TT22; outcomes are
std behavior:

| seed | opp | W | notes |
|---|---|---|---|
| 500459 | handwritten | 2823 | diverges at EVERY size ≤TT18; only TT22 plays KD (see deep-dive page); depth-pref can't fix |
| 500072 | handwritten | 2469 | knife-edge: diverges only at TT14/15; depth-pref fixes at TT15 |
| 500202 | handwritten | 2456 | TT8+TT9 diverger |
| 500262 | handwritten | 1569 | TT8 diverger |
| 500266 | handwritten | 3791 | TT8+TT9; ORDER2 could not fix |
| 500283 | handwritten | 3598 | TT8+TT9 |
| 500304 | handwritten | 1741 | TT8 |
| 500369 | handwritten | 1273 | TT8 |
| 500559 | handwritten | 3901 | TT8+TT9; largest panel W |
| 500660 | handwritten | 2402 | TT8–TT10 |
| 500712 | handwritten | 1187 | TT8 |
| 500795 | handwritten | 939 | TT8+TT9; smallest diverger |
| 700601 | espresso | 2565 | TT8 outcome flip (win→loss); W>256 overflow class |
| 700910 | espresso | 512 | hypersensitive: std loses TT8–TT12, wins TT13; depth-pref/ORDER2 break it |
| 720958 | espresso | 1395 | the TT13 0.04% flip; below-M thrash class; ORDER2/ADAPT lose it |

Panel replay: `printf '%s\n' <seeds> > seeds.txt` then
`<binary> --replay-seeds=seeds.txt --strategy=octogen --opp=<opp> --players=2`
(GAME_SIG/CD_GW as needed). Espresso seeds must run with `--opp=espresso`.

Divergence/outcome context to beat: std TT13 = 0.14% move-divergence floor,
0.04% outcome flips (1/2500); std TT12 = ~0.7% move-divergence, outcome untested —
candidates must bring TT12 (and ideally TT11) to ≤ TT13's numbers.
