# The Infinite Oracle — replay move-strength analysis, designed

**Status: IMPLEMENTED (Mode A).** Mode A shipped on branch
`claude/replay-ui-new-feature-tektn9`: the C hooks (`-DFOOLISH_ORACLE_BUILD`),
the `wasm-oracle` Makefile target + committed `public/oracle.wasm.gz`, the
`src/oracle/` worker fleet + controller, the `OracleOverlay` UI, en/ru/ko
strings, the `e2e/oracle_replay.test.ts` headless suite, and the acceptance
screenshots in `docs/screenshots/oracle-*.png`. Mode B (§8b) is now BUILT too,
but it is dormant: it needs cross-origin isolation, which this site does not
send. What it measures, what it costs, and where §8b's own audit has since gone
stale are in `docs/INFINITE_ORACLE_MODE_B.md` - read that before acting on §8b.
This document is the original build spec; every factual claim
about existing code carries a `file:line` anchor, verified against commit
`cbb896d` (branch tip `1b27238`). If a line number has drifted, the
surrounding identifiers are stable — search for them.

---

## 1. What we are building

A **replay-screen-only** feature: when the user pauses a replay and presses a
new **Oracle** button, an overlay appears showing the strength of the move
that was just animated **and every alternative move that could have been
played at that same point** — as judged by octogen (the strongest shipped
bot) running an *unbounded* Monte-Carlo deliberation in the user's browser.

The strength values **come into focus live**: batches of sampled worlds keep
running in the background, the per-move estimates stream into the React
overlay, error bars shrink, and the numbers sharpen until a convergence
checkpoint is reached. A **Memory** toggle switches octogen's belief between
the full session history ("what a perfect-memory player would know") and no
history ("what a forgetful human knows") — toggling resets and restarts the
accumulation.

Key inversion versus everything else in this repo: the server modules were
meticulously squeezed (32 KiB solver table, 22 KiB stack, byte-counted
buffers) because the server may handle many games at once. **The client
replay page is the opposite regime**: one user, one position, hundreds of MB
and minutes of attention to spare. So this build spends memory and time
freely — big transposition table, parallel Web Workers, unbounded sampling —
while the shipped `bots.wasm` / `rules.wasm` / `guards.wasm` stay
**byte-for-byte untouched**.

Product context: this is the client-side seed of the monetization plan's
"Cordite Coach" (`docs/MONETIZATION_ROADMAP.md:144-151` — chess.com-style
post-game review, one free review/day, premium unlimited). Note the tension,
stated openly: a purely client-side oracle is not server-meterable — any
paywall on it is UI-enforced only. That is acceptable for this phase (it's a
growth/wow feature on shared replays); a billable server-assisted tier can
come later (§14).

### Non-goals (v1)

- **No live-game integration.** Replay screen only. §14 sketches how live
  would work; do not build it.
- **No determinism / reproducibility.** Unlike og_explain (which reproduces
  the live bot's exact decision from the secret deal seed), the oracle seeds
  every batch randomly. Estimates converge to the same values regardless of
  seed; that convergence *is* the product.
- **No changes to shipped wasm artifacts** or to any server behavior.
- **No bot-strength research.** The engine is done
  (`docs/MONETIZATION_ROADMAP.md:227-229`); we only *read out* its judgment.

---

## 2. How it works — one paragraph

The replay page already decodes the full game client-side (no network, no
auth — `src/app/[game_id]/page.tsx:16-22`, `ReplayScreen.tsx:893-918`). When
the Oracle opens at a paused decision step, the main thread builds an
**analysis job**: the pre-move game state (reconstructed from the replay
step), the session log stream up to that move (for memory-ON belief), the
acting seat, and the recorded move. It spawns N Web Workers, each holding its
**own independent instance** of a new `oracle.wasm` (the bots kernel + the
OG_EXPLAIN deliberation dump + a big solver table). Each worker loops:
marshal state → import logs (or not) → set a fresh random strategy seed →
`wasm_choose_move(octogen, seat)` with a *small world budget* → read the
per-candidate JSON dump → post it to the main thread. The controller merges
batches into running per-candidate means and standard errors, and the overlay
re-renders on a throttle. Endgame positions short-circuit: the exact solver's
proven win/loss verdicts arrive in the first batch and batching stops.

---

## 3. Why this converges: time vs memory (design rationale)

Understand this section before touching knobs; it dictates the build flags.

**Monte-Carlo sampling is fixed-memory, time-only.** Sampled worlds are
written into three static BSS `Game` slots that are reused for every sample
(`c/src/cordite_sim.c:2350-2360`; `cordite_sim.h:209-213`). World #10⁶
costs the same memory as world #1. More time → more samples → the standard
error of each candidate's mean-finish estimate shrinks as 1/√N. That is the
"come into focus" effect, and it needs **zero** extra memory.

**But time only buys precision, not truth.** The MC estimate converges to
"the value of this move under octogen's rollout policy and belief-sampled
worlds" — an excellent proxy with finite bias, not ground truth. Ground truth
comes from the **exact endgame solver**, and there the binding resource is
**memory**: solves are node-budget-limited and the transposition table is
explicitly a strength knob ("the solver is node-budget-limited, so table size
is a bot-STRENGTH knob", `c/Makefile:296-304`; collision knee ~TT10-11,
`docs/WASM_L1_BUDGET.md:177-197`). The server ships TT12+2WAY+PACK8 = 32 KiB
(one L1 page, `c/Makefile:292`); native research default is TT16 = 1 MiB
(`cordite_sim.c:803-806`). The oracle build raises this to **TT20 (+2WAY
+PACK8) = 8 MiB per instance**: module size is unchanged (the table is a
runtime bump allocation via `__builtin_wasm_memory_grow`,
`wasm_bots_api.c:29-54`; `cordite_sim.c:962-968`), only linear memory grows.

**Parallelism buys wall-clock, one memory-copy per thread.** MC batches are
embarrassingly parallel and the C is already proven safe under
independent-deliberation parallelism (the native OMP model: every RNG and
all cordite scratch state are `_Thread_local`, `c/Makefile:15-19`).
One browser fact shapes both parallel designs: **wasm cannot spawn threads
by itself** — every wasm "thread" is a Web Worker created by JS. The choice
is therefore not "workers vs threads" but *where the coordination lives*:

- **Mode A — instance fleet (default, and the mandatory fallback):** one
  single-threaded wasm instance per worker, each with its own linear
  memory; batches merge in a small TS controller. Replicates the proven
  OMP model exactly, zero C risk, works in every browser with no header
  changes. Cost ≈ (module ~2 MiB static incl. dump buffer + TT 8 MiB +
  scratch) ≈ **~12 MB per worker**, ≤ ~100 MB at 8 workers. §8.
- **Mode B — shared-memory threads, coordination in C (optional):** one
  shared `WebAssembly.Memory`, the same module instantiated in N workers,
  real TLS restored, and ALL orchestration — job control, per-thread
  seeding, score accumulation, convergence bookkeeping — done with C
  atomics in shared memory. The workers become ~30-line trampolines and
  the TS merge layer disappears; the main thread just polls one snapshot
  export. Requires cross-origin isolation (COOP/COEP) and a threaded
  toolchain build. §8b specifies it completely; it is **optional** — build
  Mode A first, and keep it as the runtime fallback wherever
  `crossOriginIsolated` is false.

Summary: **time → precision; TT memory → exact truth in endgames; worker
memory → throughput.** All three are spent here because the client can afford
all three.

---

## 4. Facts of the existing code this design is built on

These were verified in a six-agent recon pass; the implementing agent should
trust-but-spot-check the anchors.

### 4.1 Octogen's deliberation and the per-candidate score

- Entry: `octogen_strategy_choose` (`c/src/octogen_strategy.c:1758-1764`),
  dispatched by `wasm_choose_move(strat=20, seat)`
  (`c/wasm/wasm_bots_api.c:224-233`; `STRAT.octogen = 20` in
  `sdk/ts/wasm/bots.ts:55`).
- Per-candidate score = **mean finish position**: `score[i]/nsim[i]`, finish
  1 = escaped first (best) … N = durak (worst); accumulated in
  `double score[26]` / `int nsim[26]` (`octogen_strategy.c:1602-1605,
  1679-1697`). **Lower is better.**
- Worlds are run in a 3-stage schedule W1/W2/W3 with racing culls between
  stages (`:1640-1728`). Defaults per player count: pc2 192/336/336 … pc7-8
  240/480/288 (`:1220-1233`). Env overrides: `OG_W1` (>0), `OG_W2` (>0 only —
  **cannot be 0**), `OG_W3` (≥0) (`:1244-1247`).
- **Racing can be disabled with env alone**: `OG_KEEP1=26 OG_KEEP2=26`
  (26 = `OG_MAX_CANDS`, `:1133`) keeps every candidate alive through all
  stages (`:1704-1710`) — every candidate then sees every world with common
  random numbers, so per-batch `nsim` is uniform across candidates.
- **Candidate pruning that CANNOT be disabled**: candidate selection caps at
  12 attacks + 10 covers + 3 passes + GOOD + PICKUP (max 26 total,
  `:1151-1196`), ranked by a cheap heuristic. Legal moves outside this set
  are never scored and never appear in the dump. §7.4 handles the UX.
- The **trump-keep tax** (`OG_TRUMP_KEEP`, default 40 milli = 0.040 per trump
  card in an attack while the deck is alive) is applied at selection only;
  dumped scores are **untaxed** (`:1607-1625`). The client must re-apply it
  for display ranking (§9.4), exactly as the X-ray pages do
  (`c/tools/og_explain/build_data.py:29,199-207`).
- **Determinism of a call**: octogen derives all world seeds from the
  strategy LCG *without advancing it* (`:1600`; `game.c:171`) and
  saves/restores the game LCG (`:1509,1744`). Two identical calls with the
  same strategy seed are bit-identical *given identical TT state* (on a
  warm instance the persistent leaf-solve TT can shift budget-limited
  resolutions — relevant only to heads-up positions where the leaf gate
  engages, `:1519-1520`). **The oracle must set a fresh random
  seed per batch** via `wasm_set_strategy_seed(u32)`
  (`wasm_bots_api.c:191-193`) — the latency bench already batches this way
  (`e2e/_wasm_latency.test.ts:52-62`). No other RNG needs seeding.
- **No hidden-state reads**: belief is built only from own hand + table +
  flip + hand counts + the session log (`og_build_belief`, `:259-534`);
  sampled worlds overwrite every opponent hand slot and the deck prefix
  (`:544-627`). Marshaling placeholder opponent hands/deck of correct
  *counts* provably yields the identical deliberation.
- **Zero logs (memory OFF) degrades gracefully** — no pins/voids/floors, no
  asserts. Two real consequences to surface in the UI (§9.6): (a) discarded
  cards re-enter the unseen pool (the `Game` struct stores only
  `discard_pile_length`, `game.h:94`; discards are reconstructed only from
  LOG_DISCARD replay, `:390-394,498`), so memory-off worlds can deal
  publicly-dead cards; (b) the exact endgame solver is **disabled** once
  anything has been discarded (gate `unknown == pool size`, `:990-991`) — so
  memory-off endgames show MC bars, not verdicts.
- **Env knobs are latched once per instance**: `og_flags_loaded` is never
  reset; `wasm_reload_bot_flags` reloads cordite only
  (`octogen_strategy.c:77,1463-1507`; `wasm_bots_api.c:198-199`;
  `cordite_strategy.c:78-79`). §6.2 adds a 3-line oracle-only hook.
- **Endgame exact solve** fires when: deck empty, no flipped card
  outstanding, exactly 2 players IN, opponent's hand fully deduced, total
  cards ≤ 28 (`og_try_endgame_solve`, `:976-1129`). When it proves a
  win/loss the decision is deterministic and seed-independent — repeated
  batching is pointless (§8.5 stops it).

### 4.2 The OG_EXPLAIN dump — the score readout surface

Compiled only under `-DOG_EXPLAIN_BUILD` (`octogen_strategy.c:863-877`;
targets `og_explain` and `bots-wasm-explain`, `c/Makefile:146-155,
668-688`). Proven move-choice-neutral (probe saves/restores RNG + resets TT;
0-differ over 123- and 222-decision reconstructions, commit `424d1e8`).

- **Format**: one JSON object per line (JSONL), one record per decision,
  written by `og_ex_emit` (`:1333-1445`) into a **1 MiB static buffer**
  `og_ex_wbuf` (`:940-944`) — a plain static *outside* the CD_WASM_OVERLAY
  aliasing families (`wasm_overlay.h:26-42`), so it is never clobbered by
  the solver. Per-record staging cap 16 KiB (`:1343`). **Careful**: on the
  wasm sink an over-cap record is **truncated and still appended** — the
  freestanding snprintf returns the *clamped* count (`:898-931`), so the
  append guard at `:1434-1439` passes and a malformed JSON line lands in
  the buffer (only 1 MiB-buffer exhaustion drops whole records). The worker
  must treat a JSON.parse failure as a real signal (§8.5 step 9), and the
  oracle build enlarges the staging buffer (§6.3). **Reset the buffer every
  batch** regardless.
- **Exports**: `wasm_og_explain_ptr()` / `wasm_og_explain_len()` /
  `wasm_og_explain_reset()` (`:940-944`; `Makefile:684`). TS reader
  precedent: `__ogExplainDump(reset)` (`bots.ts:84-95`).
- **Per-candidate fields** (`:1396-1426`): `type`, `label` (e.g.
  `cover 9C->10H`), `cards` (tokens like `"10H"`, `"AS*"` trump-starred),
  `target` (covers), `score` (mean finish, `null` if nsim==0), `nsim`,
  `alive` (0 = raced out — irrelevant once racing is disabled), `forced_loss`,
  `verdict` (`none|unknown|illegal|win|loss|draw`) + `verdict_val`, `chosen`.
  Plus per record: `seat`, `deck`, `defender`, `trump`, `belief`
  {pinned/pool/voids/floor}, `hand`, `opp_counts`, `table`, `solver`
  {applied, result} (`:1346-1394`).
- **The verdict probe**: when the endgame gate passes, the explain build
  first runs a full-window solve per root move with budget
  `OG_EXPLAIN_SOLVE_BUDGET` (getenv'd **fresh each decision** — the one
  per-call knob; default 4,000,000 nodes, `:1041-1044`), then resets the TT
  and restores the RNG (`:1046-1047`).
- **Critical build coupling**: the wasm dump sink is `#ifdef
  CD_WASM_OVERLAY`, not a generic wasm macro (`:878-960`). The oracle build
  **must keep `-DCD_WASM_OVERLAY`** or the native `FILE*` path fails to link
  under `-nostdlib`. Keeping it is safe: overlay tenancy only constrains
  in-call scratch, and every oracle read happens between complete calls
  (`wasm_overlay.h:1-22`; `docs/BOTS_WASM_MEMORY_PLAN.md:334-398`).

### 4.3 The TS bridge call order (one shared `g_io` buffer — order is law)

From `wasmChooseMove` (`bots.ts:244-289`) and the C side:

1. marshal state: write bytes at `wasm_io_ptr()` → `wasm_import_state()`
   (`__marshalGame`, `engine.ts:329-385`; layout spec `wasm_api.c:182-196`).
   This **resets `num_logs` to 0** (`view.c:125`) — memory-off is free.
2. `wasm_import_strategy_keys` — AFTER import_state (reads `num_players`,
   `wasm_bots_api.c:161-187`).
3. `wasm_import_logs` — memory ON only. Wire: `u16 LE count`, then per
   record `i8 type, i8 seat, i8 defender_index, u8 n_pairs, n_pairs ×
   (u8 primary, u8 target)` 1-byte wire cards, `0xFE` hidden / `0xFF` none
   (`bots.ts:126-132`; `wasm_bots_api.c:128-159`). Caps: 512 records / 64
   pairs (`game.h:21-22`; `bots.ts:131-132`).
4. env: per pair write `key\0value\0` at io ptr → `wasm_setenv_from_io()`;
   table is 16 slots × 32-byte keys/values (`wasm_bots_api.c:83-126`).
5. seed: `wasm_set_strategy_seed(u32)`.
6. `wasm_choose_move(20, seat)` → chosen index; chosen-move bytes are also
   written into g_io (`wasm_bots_api.c:208-244`).
- `engine.ts` is fully browser-safe and already browser-loaded by replay
  decode (`replay/decode.ts:23-26`); it exports the reusable helpers
  `__marshalGame`, `__mem`, `__wireLogCard`, `__LOG_TYPE_TO_INT`, etc.
  (`engine.ts:1491-1507`). **Do NOT reuse `bots.ts`** in the browser: it is a
  singleton that compiles synchronously and hijacks the engine slot via
  `__adoptEngine` (`bots.ts:105-117`) — that would clobber the replay
  screen's rules instance. The oracle gets its own bridge (§8).
- Linear memory **grows** at runtime (TT bump alloc) — any cached
  `Uint8Array` view must be refreshed after growth (guard precedent:
  `engine.ts:214-218`).

### 4.4 What the replay client has at a paused step

- Route: any path segment > 7 chars renders `<ReplayScreen code=... />`, no
  auth, fully offline (`src/app/[game_id]/page.tsx:16-22`;
  `codec.ts:324-328`). Dev URL: `http://localhost:3000/<CODE>`.
- **Committed test codes**: 3-player `TUTORIAL_MOVES_CODE`
  `ENSCBI2LBAVUBJJ3J7NODALIBDGEQYLLLICQ` (`src/components/tutorialGame.ts:17`);
  two 8-player codes with extras in
  `c/tools/og_explain/samples/octogen-4v4.json` and `octogen-8way.json`
  (`url` fields).
- `steps[i]` is the state AFTER `d.logs[i]`; `steps.length = logs.length+1`
  (`src/replay/view.ts:147-244`); the paused index is `stepIdx` in
  `ReplayStage` (`ReplayScreen.tsx:526`), `-1` = pre-deal.
- **The full session log stream is client-side**: `DecodedReplay.logs` is
  the complete kernel log stream including derived events, DRAW-masked
  exactly like the server belief feed (`core.ts:66-79`; `view.ts:195-208`;
  masking contract `logwire.ts:17-21`). Logs before decision `j` are
  `d.logs.slice(0, j)`.
- Steps are **public/retrodicted views, not kernel Games**: per seat
  `hidden` count + `known` cards + `slots` (FIFO-retrodicted identities —
  exact at the final step, "a consistent guess mid-game", `view.ts:248-253`);
  fool's leftovers are complement-deduced (`view.ts:248-274`). **Deck
  contents/order are never modeled — only `deckCount`** (`view.ts:80`).
  Ground truth would need the server-only deal seed; irrelevant here because
  octogen never reads deck contents or opponent hands (§4.1).
- All hands render face-up behind the eye toggle (`replay_hands`,
  `ReplayScreen.tsx:310-448,760`).
- UI mount points: transport knob row built with the `btn()` helper
  (`ReplayScreen.tsx:715-743, 856-874`); overlay slot via GameBoard `overlay`
  / `chrome` props (`GameBoard.tsx:42-57,107`; RevealedHands precedent).
  Mini-cards: `ScaledCard`/`InlineCard` (`ReplayScreen.tsx:46-97`).
- Timing: no rAF loop; animations advance on `setTimeout(ANIMATION_TIME=500)`
  + 25 ms gaps (`AnimationContext.tsx:1278-1332`); replay autoplay uses a
  250 ms `setInterval` gated on `isAnimating` (`ReplayScreen.tsx:652-686`).
  `reactStrictMode: true` double-fires dev effects (`next.config.mjs:3`) —
  the oracle loop must be cancellation-safe.
- i18n: new strings must be added to the `StringId` union AND all three
  tables en/ru/ko (`src/localization/strings.ts:2-90,93,185,277`).

### 4.5 Build & distribution facts

- The closest template is `bots-wasm-explain` (`Makefile:668-688`): same
  `WASM_BOT_SRC` (`:601-609`), same `WASM_BOT_CFLAGS` (incl.
  `-DCD_WASM_OVERLAY -DCD_LEAFBOOK`, `:630`), same per-file `-Oz`/`-O3`
  split (`:645-653`), same wasm-opt post-pass (`:197,200-205`), plus
  `-DOG_EXPLAIN_BUILD` and the three dump exports.
- **Must keep**: no `-flto` (corrupts the indirect function table,
  `:210-213`), no `-ffast-math` (`:213-215`), `-mbulk-memory`,
  `-D_Thread_local=`, `-nostdlib -ffreestanding`, `--no-entry
  --export-memory --stack-first`. New `wasm_*` exports must be added to the
  target's export list (`:328-329`). Do NOT edit the shared `WASM_FLAGS`
  string — `WASM_RULES_FLAGS` filter-outs match it verbatim (`:417`); add
  overrides in the oracle's own flag group via `filter-out` (precedent:
  rules, `:417-421`).
- bots.wasm has **no memory pin** (only rules/guards pin, `:420-421,516`);
  the oracle needs none either. Stack: omit the bots-only 22528 override to
  inherit `WASM_FLAGS`' 262144 (`:294,644`) — zero-risk headroom.
- `public/` is committed and served with zero config (`next.config.mjs` is
  6 lines; no vercel.json). A committed `public/oracle.wasm.gz` is fetchable
  at `/oracle.wasm.gz`; the vendored sync `gunzip.ts` inflates it
  client-side (`sdk/ts/wasm/gunzip.ts:1-24`). Embeds are
  the wrong tool at this size (`Makefile:707-709` — "a real 47KB binary, not
  a 155KB base64 .ts embed").
- CI never rebuilds wasm (`Makefile:181-183`); `collect_metrics.mjs`
  hardcodes the three shipped modules with try/catch
  (`scripts/collect_metrics.mjs:18,66-83`) — a new committed artifact breaks
  nothing. The service worker is dormant (not registered, not in `public/`);
  if ever revived its cache-first handler would pin the asset → version the
  filename then (`offlinefun/sw.js:107-131`).
- No browser-automation tooling is committed (Playwright appears only as
  Next's optional peer dep); the screenshot acceptance run installs it
  ad-hoc (§12.3).

---

## 5. Analysis semantics — what exactly is being scored

### 5.1 The decision under the cursor

When paused at `stepIdx = i`, the move "just animated" is `d.logs[i]`. Not
every step is a decision (DRAW/DISCARD/DEFENDER_CHANGE/PLAYER_OUT/GAME_START
are derived events). Define:

```
ORACLE_DECISION_TYPES = { ATTACK, COVER, PASS, PICKUP, GOOD }   // seat != null
```

Walk back from `min(i, d.logs.length - 1)` — the clamp matters: the step
list ends with a synthetic `'end'` entry at index `logs.length`
(`view.ts:242-244`), where `d.logs[i]` is undefined; that final step is also
where most end-of-game review happens — to the nearest `j` with
`d.logs[j].log_type ∈ ORACLE_DECISION_TYPES && d.logs[j].seat != null`. The
analysis job is then:

- **pre-move state** = `steps[j-1]` (or the pre-deal state if `j === 0` —
  which cannot happen, since log 0 is GAME_START; if no such `j` exists the
  Oracle button is disabled),
- **actor** = `d.logs[j].seat`,
- **recorded move** = `d.logs[j]` (type + card_pairs),
- **belief logs (memory ON)** = `d.logs.slice(0, j)` — everything strictly
  before the decision,
- **decision id** = `${code}:${j}:${memoryOn ? 1 : 0}` (accumulators are
  keyed by this; changing step or toggling memory starts a fresh
  accumulation, per the product requirement).

The overlay header names the decision: "Seat 3 played *cover 9♣→10♥* — what
octogen thinks of every option:".

### 5.2 The position is the *retrodicted* position — say so honestly

The marshaled pre-move state uses the replay pipeline's retrodicted hand
identities for the acting seat (exact at game end; a consistent-with-the-
record guess mid-game, `view.ts:248-253`). Opponent hands and the deck are
placeholders (counts only) — **provably irrelevant** to octogen (§4.1).
Consequences:

- The recorded move is always available in the marshaled hand (retrodiction
  binds a played card to a slot that exists at play time).
- Mid-game, the acting seat's *unplayed* cards may differ from historical
  truth (draw-order ambiguity). The analysis is then of "a position
  consistent with everything publicly visible in this replay" — which is
  also exactly the information a human reviewer has. Note: for any replay
  that survives `buildReplaySteps`' conservation check (`view.ts:288-303`),
  the complement fill (`view.ts:254-274`) binds *every* slot — a `null`
  acting-hand slot should be unreachable. Keep a belt-and-suspenders
  fallback anyway (fill deterministically from the unseen complement, set
  `approx: true`, show the `oracle_approx` footnote) since the marshal must
  never emit an invalid card.
- This is a *feature-level* honesty requirement: the overlay subtitle for
  mid-game analyses reads "based on the publicly visible record" (i18n
  `oracle_basis`).

### 5.3 Memory ON vs OFF

- **ON**: import `d.logs.slice(0, j)` (converted to the kernel log wire,
  §8.3). Octogen's belief then carries pins (watched pickups), voids, rank
  floors, discard knowledge, and the flipped-trump pin — the full
  perfect-memory deduction (`og_build_belief`, `:259-534`). This matches the
  live bot's belief feed semantics (DRAW-masked stream, `logwire.ts:17-21`).
  Cap: 512 records (keep-first, drop-newest — the same cap and behavior the
  LIVE bot has, `bots.ts:131,147,175`; observed session peak ~1,030 over 7k
  games, `docs/BOTS_WASM_MEMORY_PLAN.md:597-606`). v1 accepts the parity
  with the live bot; raising `MAX_LOGS` is future work (it cascades into
  `WASM_IO_CAP`/overlay-slot sizing — see risk R8).
- **OFF**: skip the import (marshal already zeroed `num_logs`). Belief =
  own hand + table + flip + counts only. Two visible effects the UI must
  anticipate: dead cards resurrect into sampled worlds, and endgame verdicts
  vanish once anything is discarded (§4.1). That asymmetry is the point —
  it approximates a player who forgot the history.

### 5.4 The two result regimes

- **MC regime** (most of the game): per-candidate `score` = mean finish
  position, converging with √N. UI shows relative bars + focus animation.
- **Exact regime** (heads-up, deck empty, deduction complete, ≤28 cards,
  memory ON): `solver.applied = 1` and per-move verdicts `win/loss/draw`
  appear. Values are **proven**; batching stops after the first batch; the
  UI switches to verdict bars with an "exact" badge (§9.5). If the solver
  fired but proved nothing within budget (`solver.applied && no win/loss
  verdicts`), stay in the MC regime and keep batching (the X-ray renders the
  same distinction, `gen_html.py:393-455`) — **but first defuse the probe**:
  in this regime EVERY subsequent choose re-runs the full per-root-move
  verdict probe (up to moves × 2M nodes) plus two full-TT memsets (8 MiB
  each at TT20; `:1015,1046`; `cordite_sim.c:1820`), costs the §8.6 tuner
  cannot touch (they are independent of `OG_W1`). Since the verdicts were
  already proven unknowable at full budget, the worker rewrites the env pair
  `OG_EXPLAIN_SOLVE_BUDGET=0` after that first batch — the knob is getenv'd
  fresh per decision (`:1041-1044`), so no reload call is needed, and a ≤0
  budget records `unknown` instantly. Later batches are then pure-MC-priced.

---

## 6. C changes (minimal, oracle-build-only)

All C changes are guarded by a **new compile flag `-DFOOLISH_ORACLE_BUILD`**,
defined ONLY by the new Makefile target. Shipped builds do not define it, so
`bots.wasm` / `rules.wasm` / `guards.wasm` compile to byte-identical output.
(Do not name it `OG_ORACLE*` — `og_oracle` already means octogen's 6×-worlds
research variant, `octogen_strategy.c:1244,1766-1772`.)

### 6.1 Inventory — three tiny additions, nothing else

| # | File | Change |
|---|------|--------|
| C1 | `c/src/octogen_strategy.c` | `og_reload_flags()` — un-latch the env cache |
| C2 | `c/wasm/wasm_bots_api.c` | `wasm_og_reload_flags()` export wrapper |
| C3 | `c/src/octogen_strategy.c` (`og_ex_emit`) | emit verdict-only entries for solver-classified moves outside the candidate set |

### 6.2 C1+C2 — the env reload hook

Problem: OG_* knobs (`OG_W1`, `OG_KEEP1`, …) are read once per instance
behind `og_flags_loaded` (`:77,1463-1507`) and `wasm_reload_bot_flags`
resets cordite only. The oracle wants to (a) install its env before the
first choose — which works today — and (b) **adapt the batch size between
batches** (§8.6), which needs a reset.

In `octogen_strategy.c`, next to the `og_flags_loaded` definition:

```c
#ifdef FOOLISH_ORACLE_BUILD
// Infinite-oracle hook (client replay analysis, docs/INFINITE_ORACLE_DESIGN.md):
// allow the bridge to re-read the OG_* env between deliberation batches, so
// the per-batch world budget can adapt to the measured device speed. Mirrors
// cordite_reload_flags (cordite_strategy.c). Compiled ONLY into oracle.wasm;
// shipped builds carry no trace of it.
void og_reload_flags(void) { og_flags_loaded = false; }
#endif
```

(Match the actual type/initializer of `og_flags_loaded`; if it is `int`, use
`0`.) Declare it in `octogen_strategy.h` under the same `#ifdef`. In
`wasm_bots_api.c`:

```c
#ifdef FOOLISH_ORACLE_BUILD
void og_reload_flags(void);
void wasm_og_reload_flags(void) { og_reload_flags(); }
#endif
```

Export it only in the oracle target's list (§7). The bridge calls it after
rewriting env pairs (the same moment `bots.ts:234` calls
`wasm_reload_bot_flags` today).

### 6.3 C3 — verdict entries for pruned moves (endgame UX)

Problem: in the exact regime, a move the player actually made may be
excluded from candidacy as a proven loss (`:1119-1127,1563-1568`) — exactly
the move the user most wants judged ("you played into a proven loss").
The probe already computed its verdict into `og_ex_verdict[]` (indexed by
**move index**, `:934-935,1035-1045`); it just isn't emitted.

In `og_ex_emit`, after the candidates array is written, under
`#ifdef FOOLISH_ORACLE_BUILD`: when the solver probe ran
(`solver.applied`), iterate all legal moves; for each move index `mi` not
present in the candidate set but with a computed verdict, append an entry:

```json
{"type":"...","label":"...","cards":[...],"score":null,"nsim":0,
 "alive":0,"pruned":1,"forced_loss":<flag>,"verdict":"loss","verdict_val":-987,
 "chosen":0}
```

Do **not** emit pruned entries in the MC regime (mid-game menus can be
thousands of moves; §9.4 handles the mid-game pruned-move UX bridge-side).

**Record-size requirement (verified, load-bearing):** heads-up ≤28-card
positions can still enumerate ~75-97 root moves (equal-rank subset attacks),
and the base record (belief + big hand + 26 candidates) is already 4-6 KiB —
pruned verdict entries can push a record past the 16 KiB staging cap, where
the wasm snprintf **truncates instead of dropping** (§4.2), producing
malformed JSON. Under `FOOLISH_ORACLE_BUILD`: (a) enlarge the staging buffer
to a file-scope `static char buf[65536]` (it currently lives in a
non-recursive frame at `:1343`); (b) if the formatted length still hits the
cap, drop the record and append a tiny `{"overflow":1}` line instead so the
worker sees an explicit signal rather than a parse crash. Reuse the existing
label/cards formatting helpers (`og_ex_move_label`, `:1306-1328`).

### 6.4 Explicitly rejected C changes (do not do these)

- **No new score-export ABI.** The JSONL dump is proven, tested tooling; a
  binary score table would be a second source of truth.
- **No `OG_W2=0` gate change** (`:1246`). Minimum batch = W1+1 worlds is fine.
- **No TT-reset export.** TT warmth across batches makes batches slightly
  non-i.i.d. (later batches resolve more solver leaves within the 3,000-node
  leaf budget, `cordite_sim.c:2163-2167`) — this only *helps* later batches
  and is harmless to a converging mean. Documented as risk R6.
- **No touching `wasm_choose_move`'s dispatch** (the 6×-worlds
  `octogen_oracle_strategy_choose` stays unreachable; world budget comes
  from `OG_W*`).

---

## 7. The build target and artifact

### 7.1 Makefile target `wasm-oracle`

Add to `c/Makefile`, modeled directly on `bots-wasm-explain`
(`:668-688`). Same sources (`WASM_BOT_SRC`), same per-file opt split
(`WASM_BOT_O3_SRC` at -O3, rest -Oz), same wasm-opt pass:

```make
# ---------------------------------------------------------------------------
# oracle.wasm — the client-side "infinite oracle" replay analyzer
# (docs/INFINITE_ORACLE_DESIGN.md). bots.wasm's brain + the OG_EXPLAIN
# per-candidate dump + oracle-only hooks (-DFOOLISH_ORACLE_BUILD), rebuilt for
# the browser's unconstrained-memory regime: TT20 (+2WAY+PACK8 = 8 MiB runtime
# table — a strength knob, Makefile:296-304) and the default 256 KiB stack
# (no 22528 override). CD_WASM_OVERLAY stays: the explain sink requires it
# (octogen_strategy.c:878-960) and repeated complete calls respect the
# tenancy. NEVER replaces the shipped modules; ships as a committed
# public/oracle.wasm.gz the replay page fetches lazily.
WASM_ORACLE_CFLAGS := $(filter-out -DCD_TT_BITS=12,$(WASM_BOT_CFLAGS)) \
                      -DCD_TT_BITS=20 -DOG_EXPLAIN_BUILD -DFOOLISH_ORACLE_BUILD
WASM_ORACLE_LDFLAGS := $(filter -Wl%,$(WASM_FLAGS))
WASM_ORACLE_EXPORTS := $(WASM_API_EXPORTS) $(WASM_BOTS_API_EXPORTS) \
  -Wl,--export=wasm_og_explain_ptr -Wl,--export=wasm_og_explain_len \
  -Wl,--export=wasm_og_explain_reset -Wl,--export=wasm_og_reload_flags
.PHONY: wasm-oracle
wasm-oracle:
	@mkdir -p build/botoracle
	@for src in $(WASM_BOT_SRC); do \
	  base=$$(basename $$src .c); opt=-Oz; \
	  case " $(WASM_BOT_O3_SRC) " in *" $$src "*) opt=-O3;; esac; \
	  echo "  CC(oracle) $$src [$$opt]"; \
	  $(WASM_CC) $(WASM_ORACLE_CFLAGS) $$opt -c $$src -o build/botoracle/$$base.o || exit 1; \
	done
	$(WASM_CC) --target=wasm32 -nostdlib $(WASM_ORACLE_LDFLAGS) \
	          $(WASM_ORACLE_EXPORTS) build/botoracle/*.o -o build/oracle.wasm
	$(call wasm_postopt,$(WASM_BOTS_POSTOPT),build/oracle.wasm)
	gzip -9 -n -c build/oracle.wasm > ../public/oracle.wasm.gz
```

(`-n` matters: without it gzip embeds the input mtime, so the committed gz
churns on every rebuild of identical bytes — and note the shipped
`wasm-bots` recipe lacks `-n` (`Makefile:710`), so §12.1's "shipped
artifacts untouched" check must compare *inflated* bytes for
`bots.wasm.gz`, not the gz files.)

Notes for the builder:

- `filter-out -DCD_TT_BITS=12` + re-add TT20 follows the rules-target
  precedent (`:417-421`); `-DCD_TT_2WAY -DCD_TT_PACK8` ride through
  unchanged → 2²⁰ × 8 B = **8 MiB** table, allocated lazily at first solve.
- Omitting the `-Wl,-z,stack-size=22528` override means the shared flags'
  262144 wins — recon confirmed bigger TT / more worlds do not raise stack
  high-water (TT is heap; worlds are BSS iteration,
  `c/Makefile:631-644`, `cordite_sim.c:1286-1302`), so this is pure
  belt-and-suspenders headroom.
- Requires `wasm-opt` (binaryen) on PATH like every bots-flavored build —
  build-time dep only; CI never rebuilds wasm.
- Expected size: shipped `bots.wasm.gz` is 53,398 B; the explain machinery
  adds the shim + dump strings — expect **~60-75 KB gz**. Record the actual
  number in the landing commit message (convention: standalone artifact
  commit, `docs/BOTS_WASM_MEMORY_PLAN.md:434-441`, commit `4f02507` style).

### 7.2 Artifact placement & loading

- **Committed** at `public/oracle.wasm.gz` (raw `.wasm` would also work but
  gz keeps the repo and transfer small and reuses the vendored inflater).
- Fetched lazily: nothing on the replay page downloads it until the first
  Oracle button press. Loader: `fetch('/oracle.wasm.gz')` → `arrayBuffer` →
  `gunzip()` (`@shared/wasm/gunzip.ts`) → post the inflated bytes to each
  worker → `await WebAssembly.instantiate(bytes, {})` inside the worker
  (imports object is `{}` — the module is freestanding and brings its own
  bump allocator, `wasm_bots_api.c:24-54`).
- Add a source comment noting the SW risk: if `offlinefun/sw.js` is ever
  registered, this filename must become content-hashed (R9).
- If Mode B (§8b) is built, it is a **second** target (`wasm-oracle-mt`) and
  a second committed artifact `public/oracle-mt.wasm.gz` — a shared-memory
  module cannot instantiate without a shared memory import, so the two
  builds coexist and the loader picks by `crossOriginIsolated` (§8b.2).

---

## 8. Mode A — the TS runtime: worker fleet + controller (default; build first)

### 8.1 File layout

```
src/oracle/
  types.ts            shared types (job, candidate, batch, state)
  logsWire.ts         SeatLog[] -> kernel import-logs wire bytes
  oracleBridge.ts     everything that talks to one wasm instance (worker-side)
  oracleWorker.ts     the Worker entry: protocol + batch loop
  OracleController.ts main-thread: fetch/spawn/merge/converge/publish
  replayOracleInput.ts DecodedReplay + steps + stepIdx -> OracleJob
src/components/OracleOverlay.tsx   the overlay panel
```

Workers are created with the bundler-native pattern
`new Worker(new URL('./oracleWorker.ts', import.meta.url))` (supported by
Next/webpack 5; no config change — verify once in dev, it is the one piece
of this design with no in-repo precedent; fallback if Next fights it: a
single-file worker bundled by placing the entry under `src/oracle/` and
letting webpack chunk it).

Worker count: `clamp(navigator.hardwareConcurrency - 2, 1, 8)`.

### 8.2 The job (structured-clone-safe)

```ts
interface OracleJob {
  decisionId: string;          // `${code}:${j}:${memoryOn?1:0}`
  seat: number;                // acting seat
  memoryOn: boolean;
  gameBlob: OracleGameState;   // plain-JSON Game shape for __marshalGame (§8.4)
  logsWire: Uint8Array;        // pre-encoded kernel log wire (empty if memory off)
  recordedKey: string;         // canonical key of the recorded move (§9.4)
  numPlayers: number;
  deckAlive: boolean;          // step.deckCount > 0 || step.flipped !== null —
                               // octogen's actual tax gate (octogen_strategy.c:1614)
                               // includes the deck-empty-but-flip-outstanding
                               // window; this improves on the X-ray's deck>0
                               // (build_data.py:203), which can't see has_flipped
  approx: boolean;             // §5.2 null-slot fill happened (should be unreachable)
}
// gameBlob.good_players / .elimination_order are player_id STRING arrays
// ('seat-N') — see §8.4; numeric seats fail silently in __marshalGame.
```

### 8.3 `logsWire.ts` — SeatLog[] → import wire

Mirror `importLogs` (`bots.ts:164-203`) but from the decoded `SeatLog[]`
(seat already numeric — simpler): `u16 LE count`, per record `i8 type`
(`SeatLog.log_type` is a runtime STRING of the LogType union; map it via
the exported `__LOG_TYPE_TO_INT` Map, `engine.ts:1502-1504`, with `?? 0`
fallback per `bots.ts:185`), `i8 seat (0xFF if null)`,
`i8 defender_index (0xFF if null)`, `u8 n_pairs (≤64)`, pairs via
`__wireLogCard` (hidden `{-1,-1}` → `0xFE`, absent target → `0xFF`).
Truncate at 512 records keep-first (mirrors the live cap). Encode ONCE on
the main thread per job; ship the bytes to every worker.

### 8.4 `replayOracleInput.ts` — building the marshal-shaped state

`__marshalGame(ex, game)` (`engine.ts:329-385`) wants a Game-shaped object:
status, players[] (status, awaiting_attack, hand: Card[]), power_suit,
first_attacker, defender, discard_pile_length, flipped, good_players,
good_timestamp, deck (array — length must equal the true deck count),
table_battles, elimination_order. Build it from `steps[j-1]` + `d`:

- `status: PLAYING`; `power_suit` / `flipped` / `deckCount` / `discard` /
  `battles` / `firstAttacker` straight from the step.
- `deck`: `deckCount` × placeholder card (`{suit:0,value:5}` — any real
  card; octogen overwrites the deck prefix in every world, and only the
  COUNT is meaningful. Precedent: `clientGuards.ts:77,164`).
- acting seat's `hand`: the step's `known` + retrodicted `slots` for that
  seat (`ReplaySeatView`, `view.ts:22-40`), null-slot fill per §5.2.
- every other seat: `hand_length` placeholders.
- **`defender`, goods, outs — already on the step (verified)**:
  `ReplaySeatView` carries `out` / `isDefender` / `good` per seat
  (`view.ts:37-39`, populated at `:127-134`), and `stepToGame` already
  derives the marshal-ready values: `defender =
  players.findIndex(p => p.isDefender)` (`:355`), `good_players =` the
  `'seat-N'` id list (`:359-361`), statuses IN/OUT (`:348`). **Reuse those —
  do not re-fold.**
- **The ONE genuinely missing field: the ORDERED elimination list.**
  `stepToGame` hardcodes `elimination_order: []` (`view.ts:357`) — display
  code never needed it, but for the oracle it is **load-bearing for every
  score**: `cd_sim_from_game` copies the marshaled elimination order into
  the sim (`cordite_sim.c:90,101`) and every rollout finish position is
  `elim_order index + 1` (`:1968-1970,2017-2019`). Marshaling `[]` after
  the first elimination silently skews every EF. Capture the ordered list
  in the `buildReplaySteps` fold — from PLAYER_OUT logs **plus the silent
  no-log outs**: the kernel's empty-stock refill marks players out with no
  log record (`game.c:440-447`, mirrored client-side at `view.ts:221-225`),
  so fold the `out`-flag *transitions* in kernel seat order, not PLAYER_OUT
  events alone. Add the §12.2 assertion: at any decision after k
  eliminations, every candidate mean must be ≥ k+1.
- **`good_players` / `elimination_order` are arrays of player_id STRINGS.**
  `__marshalGame` converts them to the seat bitmask / seat list via
  `findIndex(p => p.player_id === pid)` (`engine.ts:347-350,371-374`) —
  numeric seats would silently produce an empty good mask and `0xFF`
  elimination entries, and the §12.2-1 legality test would NOT catch it (a
  recorded stream never replays an already-good press). Use the
  `'seat-N'` convention `stepToGame` already uses (`view.ts:347`), and add
  a §12.2 assertion that the good mask is non-empty at a step with goods
  pending.
- `awaiting_attack`: **inert — marshal `false` for every seat.** In the
  kernel it is an attacker-side flag that is write-only for our purposes:
  set/cleared in `game.c` (`:591,707,850`) but read nowhere in `legal.c`,
  `should_bot_act`, or the sim — its only read is view serialization
  (`view.c:47,112`). No derivation is needed and no test can (or needs to)
  validate it.

### 8.5 `oracleWorker.ts` — the batch loop

Protocol: `{t:'init', bytes}` → instantiate + `wasm_init()`;
`{t:'analyze', job, seedSalt}` → loop; `{t:'stop'}` → abandon loop.

Per batch (order per §4.3 — it is load-bearing):

```
1. if env dirty (first batch, or W1 retuned):
     for each pair: write "KEY\0VALUE\0" at wasm_io_ptr() -> wasm_setenv_from_io()
     wasm_og_reload_flags()
2. __marshalGame(ex, job.gameBlob)          // fresh marshal every batch
3. importStrategyKeys: one i8 -1 per seat -> wasm_import_strategy_keys()
   // -1 everywhere: the only strategy that reads strategy_key is
   // espresso_prod (espresso_prod_strategy.c:978), which is NOT linked into
   // the bots/oracle wasm (Makefile:601-609) — keys are inert for the whole
   // module. The call itself is still required only because
   // wasm_import_strategy_keys reads num_players bytes unconditionally
   // (wasm_bots_api.c:182-187).
4. if job.memoryOn && job.logsWire.length: write logsWire -> wasm_import_logs()
5. wasm_set_strategy_seed(nextSeed())       // crypto-seeded xorshift, never 0
6. wasm_og_explain_reset()
7. idx = wasm_choose_move(20, job.seat)     // 20 = STRAT.octogen
8. len = wasm_og_explain_len(); if len==0 -> post {t:'empty'} and stop
   text = decode(memory at wasm_og_explain_ptr(), len)   // refresh the
   Uint8Array view EVERY read — memory.grow invalidates buffers (§4.3)
9. record = JSON.parse(first line)
   // a parse failure or an {"overflow":1} line (§6.3) is a real signal:
   // post {t:'error'} and stop — do not retry-loop a malformed dump
10. postMessage({t:'batch', decisionId, record, batchMs})
11. STOP RULES (all verified against real emit paths):
    a. EXACT: record.solver?.applied and any candidate verdict in {win,loss}
       -> post {t:'exact'}, stop.            // proven; no more sampling
    b. FORCED/SOLVED: every candidate has nsim==0 (and no win/loss verdict)
       -> post {t:'forced'}, stop after this first batch. This covers three
       real paths that would otherwise spin to the hard cap: single legal
       move (:1452-1461, emits C=NULL, no solver flag), solver-win with all
       probe verdicts 'unknown' (:1550-1559 — the narrow-window win-hunt can
       prove a win the full-window probe timed out on), and single surviving
       candidate (:1577-1584). The UI renders the record as-is with the
       oracle_forced_move note.
    c. UNPROVEN-SOLVER DEFUSE: record.solver?.applied but no win/loss
       verdicts -> rewrite env OG_EXPLAIN_SOLVE_BUDGET=0 (per-call getenv,
       no reload needed) and keep batching in MC mode (§5.4).
12. yield (setTimeout 0) and continue unless stopped
```

Env set (6 of 16 slots; all values ≤ 32 bytes):

```
OG_KEEP1=26  OG_KEEP2=26            // racing OFF -> uniform nsim per batch
OG_W1=<adaptive>  OG_W2=1  OG_W3=0  // batch = W1+1 worlds per candidate
OG_EXPLAIN_SOLVE_BUDGET=2000000     // per-move probe budget (per-call getenv)
```

### 8.6 Batch sizing (adaptive)

Target ~40 ms per batch so `stop` is responsive and progress feels alive.
Start `OG_W1=24`. After each batch: if `batchMs < 25` → `W1 = min(W1*2,
192)`; if `batchMs > 80` → `W1 = max(8, W1/2)`; mark env dirty (step 1
re-runs). Known cost anchors: full-budget octogen ≈ 41.5 ms/decision native
pc2 (`docs/BOTS_WASM_MEMORY_PLAN.md:675`), ~56-64 ms wasm e2e p50
(`LEAFBOOK.md:9-11`) at 864 worlds — so ~25-world batches should land well
under 40 ms; **measure, don't trust the estimate** (no small-budget wasm
figure exists in-repo).

### 8.7 `OracleController.ts` — merge & converge

Accumulator per candidate key (§9.4 keys):

```
n      += rec.nsim              // uniform across candidates (racing off)
sum    += rec.score * rec.nsim  // reconstruct the batch sum
batches.push(rec.score)         // per-batch means, for SE
mean    = sum / n
se      = stddev(batchMeans) / sqrt(batches.length)   // Welford
```

Also track `verdict` (first non-none wins; exact regime), `forced_loss`,
`pruned`, `totalWorlds`, `worldsPerSec`.

Candidate identity across batches: **candidate enumeration is a
deterministic function of the marshaled state + belief** (heuristic-ranked
insertion, no RNG — `og_pick_candidates`, `:1151-1196`), so the same
decisionId yields the same candidate set every batch. Key by canonical form
anyway (type + sorted cards + sorted targets) for safety, and tolerate a
batch introducing an unseen key (merge it in).

Convergence checkpoint ("keep running until a pretty big checkpoint"):

```
CONVERGED when  minCandidate(n) >= 65_536  OR  maxCandidate(se) <= 0.005
                (both computed over candidates with n > 0 ONLY — pruned and
                 scoreless rows are excluded or forced steps never converge)
HARD CAP        wall clock 180 s (belt and suspenders)
EXACT           solver verdicts arrived -> stop immediately, badge "exact"
FORCED          first batch reports all-nsim==0 with no verdicts -> stop,
                badge oracle_forced_move (§8.5 step 11b)
```

All four numbers are named constants in `types.ts`; tune after measuring.

Publishing: controller keeps a plain subscriber list (precedent:
`animationFeed`, `src/state/animationFeed.ts:82-103`) and notifies at most
every 200 ms; the overlay subscribes in a `useEffect` and `setState`s the
snapshot. Never publish per-message — 8 workers × 25 Hz would melt React.

Lifecycle: `start(job)` cancels any prior run (post `stop` to workers, bump
a run-generation counter so stale `batch` messages are dropped);
`dispose()` terminates workers (on overlay close keep them warm; terminate
on replay-screen unmount). StrictMode-safe: effects must
`return () => controller.stopCurrent()`.

---

## 8b. OPTIONAL Mode B — shared-memory wasm threads, coordination in C

> **Built. See `docs/INFINITE_ORACLE_MODE_B.md` for what it actually measures**
> (about 10% more worlds per second, and no memory saving), for the determinism
> answer, and for the cross-origin-isolation gate that keeps it dormant on the
> deployed site. Two claims below have gone stale and are corrected there:
> §8b.2's "no CDN fonts" (the app loads Google Fonts now) and §8b.5's MT6
> ("verdicts come from the solver pass octogen already runs" - the full
> per-move probe was hoisted after all).

This section is a complete recipe for moving the parallelism into C proper,
so the TS layer keeps no cross-thread state at all. It is **optional**: Mode
A ships first and remains the runtime fallback (no cross-origin isolation →
no SharedArrayBuffer → Mode B cannot run). Do not start Mode B until Mode A
is merged and measured; the UI, `replayOracleInput`, `logsWire`, and the
overlay are 100% shared between modes — Mode B replaces only the controller
internals and the worker body.

### 8b.1 What actually changes

| Concern | Mode A (fleet) | Mode B (threads) |
|---|---|---|
| Memory | N private linear memories | ONE shared `WebAssembly.Memory({shared:true})` |
| Instances | N independent | same compiled module instantiated N times over the shared memory (per-instance mutable globals = per-thread stack pointer & TLS base) |
| Marshal/logs/env | per worker, per batch | ONCE, by the control instance on the main thread; threads read the shared resident state |
| Seeding | TS sends seed per batch | C: each thread seeds its own `_Thread_local` strategy LCG from `mix(seed_base, tid, batch_no)` |
| Score merge | TS controller sums batch records | C: threads atomically add integral finish-position sums into a shared per-candidate table |
| UI feed | postMessage per batch | main thread polls `wasm_mt_snapshot()` every ~200 ms |
| Worker code | bridge + batch loop | trampoline: instantiate, set stack, init TLS, call `wasm_mt_thread_main(tid)` once — it never returns |
| Explain dump | JSONL per batch | not used — replaced by the C accumulator + one binary candidate-descriptor export |

### 8b.2 Browser prerequisites and deployment

- SharedArrayBuffer requires **cross-origin isolation**: serve the document
  with `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: credentialless` (falls back to
  `require-corp` + CORP audit if a target browser lacks credentialless).
  Next.js: add a `headers()` block to `next.config.mjs`. **Caution**: the
  replay route shares the `/[game_id]` pattern with live games, so these
  headers will apply beyond the replay page; audit every cross-origin
  subresource (this app is unusually clean — procedural assets, no CDN
  fonts; Supabase REST/realtime are CORS fetches/websockets, which COEP
  permits; verify any OAuth popup flow still works under COOP — redirects
  are safe, popups are not). Rollback is trivial: remove the headers and
  the runtime detection (below) silently returns everyone to Mode A.
- Runtime selection in the loader:
  `const mt = typeof SharedArrayBuffer !== 'undefined' && crossOriginIsolated;`
  → fetch `oracle-mt.wasm.gz`, else `oracle.wasm.gz`. **Two artifacts**: a
  shared-memory module declares its memory import shared and cannot
  instantiate without one, so Mode B is a separate build, committed
  alongside the Mode A artifact.

### 8b.3 Toolchain (freestanding threads — no emscripten)

Diff against the Mode A target (§7.1):

```
compile:  + -matomics                    (atomics ops; -mbulk-memory already on)
          - -D_Thread_local=             (restore REAL TLS — the whole point:
                                          solve_ws, cd_tt, RNGs, world slots
                                          become genuinely per-thread again,
                                          exactly the proven native OMP model)
          - -DCD_WASM_OVERLAY            (the overlay aliases fixed static
                                          addresses into solve_ws, which is
                                          only legal when TLS is stripped —
                                          wasm_overlay.h:1-22 says exactly
                                          this; with real TLS it would be the
                                          native data race. Costs +162.5 KiB
                                          statics — irrelevant here)
          - -DOG_EXPLAIN_BUILD           (its wasm sink is #ifdef
                                          CD_WASM_OVERLAY and would fail to
                                          link; Mode B replaces the dump with
                                          the C accumulator, MT4)
          + -DFOOLISH_ORACLE_BUILD -DFOOLISH_ORACLE_MT
link:     + -Wl,--shared-memory -Wl,--import-memory
          + -Wl,--max-memory=134217728   (shared memories MUST declare a max;
                                          128 MiB covers 8 threads × ~9.6 MB
                                          + statics with headroom)
          + -Wl,--export=__stack_pointer -Wl,--export=__wasm_init_tls
          + -Wl,--export=__tls_size -Wl,--export=__tls_align
wasm-opt: + --enable-threads
```

Toolchain facts the builder should know (these are LLVM/wasm-ld behaviors,
not repo facts — verify once with a hello-threads spike before wiring the
full target):

- With `--shared-memory`, wasm-ld turns data segments **passive** and
  synthesizes a start function that runs `memory.init` exactly once behind
  an atomic guard — multi-instantiation over one memory is safe without any
  crt (our `--no-entry` builds never call ctors; plain-C statics need none).
- TLS: LLVM emits `__wasm_init_tls(block_ptr)` plus `__tls_size`/`__tls_align`
  globals when TLS symbols exist under `--shared-memory`. Each
  instantiation starts with the *linked* stack pointer and an uninitialized
  TLS base — the trampoline must fix both **before any other export call**.
- The one-page bump allocator keeps working: `memory.grow` on shared memory
  is atomic. But the bump pointer itself needs a lock (MT1).

### 8b.4 Per-thread bootstrap protocol

Main thread (controller):

```
1. bytes = fetch + gunzip oracle-mt.wasm.gz ; module = await WebAssembly.compile(bytes)
2. memory = new WebAssembly.Memory({ initial: 512, maximum: 2048, shared: true })  // pages
3. control = await WebAssembly.instantiate(module, { env: { memory } })   // main-thread instance
   control.wasm_init()
4. base = control.wasm_mt_reserve(nThreads)   // C bumps its allocator once for
   // nThreads × (STACK 512 KiB + TLS block of __tls_size, aligned) and
   // returns the region base; the control instance computes nothing else.
5. for tid in 0..nThreads-1: spawn worker, postMessage({ module, memory, tid,
     stackTop: base + (tid+1)*STACK, tlsPtr: tlsBase(tid) })   // one message, ever
```

Worker trampoline (~30 lines, the only worker code in Mode B):

```
const inst = await WebAssembly.instantiate(module, { env: { memory } });
inst.exports.__stack_pointer.value = stackTop;   // BEFORE any call
inst.exports.__wasm_init_tls(tlsPtr);
inst.exports.wasm_mt_thread_main(tid);           // never returns
```

The control instance runs on the **main thread** and is the only one that
marshals: per job it does the §8.5 steps 1–4 (env → `wasm_og_reload_flags`
is not needed — see MT7; marshal; strategy keys; logs) against the shared
`g_game`, then `wasm_mt_setup(seat, w1, seedBase)`. Threads never touch
`g_io`. The main thread must never `Atomics.wait` (browsers forbid it);
it only does atomic stores (setup/stop) and non-blocking snapshot reads.

### 8b.5 The C additions (MT1–MT8, all under `FOOLISH_ORACLE_MT`)

This is the honest "large lift" — roughly 250–350 lines of new C, all in
one new file `c/wasm/wasm_oracle_mt.c` plus small `#ifdef` seams:

- **MT1 — atomic bump allocator.** `g_brk` in `wasm_bots_api.c:29-54` is a
  plain static; two threads' first TT `calloc` would race. Wrap alloc in a
  one-byte spinlock (`__atomic_exchange_n(&lock,1,__ATOMIC_ACQUIRE)` loop /
  release store). `memory.grow` itself is thread-safe.
- **MT2 — per-thread move list.** `wasm_choose_move` enumerates into the
  shared `g_moves` (`wasm_api.c:125,146-147`) — a race if threads used it.
  Threads do NOT call `wasm_choose_move`; the MT batch path allocates a
  `_Thread_local LegalMoves` (237 KB/thread in TLS) and calls
  `calculate_legal_moves` + `octogen_strategy_choose` directly.
- **MT3 — restore-TLS audit.** With `-D_Thread_local=` gone, everything the
  native OMP build already keeps per-thread (solve_ws, the cd_tt pointer,
  both LCGs, world/trial/diff slots, `og_flags_loaded`,
  `og_polmap`/`og_bbleaf_on`, `forced_loss_flags`, solver scratch) is
  per-thread again by construction — an adversarial audit confirmed every
  mutable static on the choose path is `_Thread_local` in the native
  sources, **except the four below**, which are the audit's starting list:
  1. **Bitboard mask tables** (`SUIT_MASK`/`VALUE_MASK`/`HIGHER_MASK`,
     guard `g_masks_ready`, `cordite_sim.c:31-53`) are **lazily first-touch
     initialized** from `cd_sim_from_game`, not setup-time. Plain-int guard,
     no acquire/release: a thread could observe the guard before the mask
     words and score worlds on zero masks. Fix (required): `wasm_mt_setup`
     forces the init on the control instance before the first generation
     (call `ensure_masks` via a tiny export, or run one dummy
     `cd_sim_from_game`), so threads only ever read them.
  2. **The snapshot ring**: `wasm_init()` installs `snap_cb` into the
     shared `engine_snap_hook` (`wasm_api.c:165-167`), and SNAP() fires
     inside `handle_*` (`game.c:107,347-826`) — reachable from threads via
     `og_apply` on the STRUCT solver/rollout paths (`octogen_strategy.c:
     173-182,1080,1110,802,1689`) with **no `wasm_*` call involved**, so
     the "threads don't call exports" rule does NOT cover it. Fix
     (required): the MT build must not have the hook installed while
     threads run — `wasm_mt_setup` sets `engine_snap_hook = NULL` (the
     oracle never reads snapshots).
  3. **`engine_last_reject`** (`game.c:105`), rewritten at the top of every
     `handle_*` — benign diagnostic race (only read by
     `wasm_reject_reason`); classify and leave.
  4. **`log_alloc`'s static drop-sink scratch** (`game.c:256`), which
     `solve_clone_prefix` routes solver-child log appends into
     (`cordite_sim.c:2323-2328`) — contents never read, so benign, but a
     formal data race; classify and leave (or make it TLS for tidiness).
  Additionally: the struct-path env knobs (`OG_NO_BBSOLVE`,
  `OG_NO_FASTROLL`, `OG_LEAF`, `OG_DIFFTEST`, `OG_NO_WORLDSIM`) are
  **forbidden in Mode B env sets** — they route threads into `handle_*`
  where items 2–4 live. The default fast bitboard path never enters them.
  Write the full classification into the PR description.
- **MT4 — the shared accumulator.** A static `JobControl`:

  ```c
  typedef struct {
      _Atomic uint32_t generation;    // bumped by setup; threads re-arm on change
      _Atomic uint32_t stop;          // 1 = park
      uint32_t seed_base, w1;         // set by setup before generation bump
      int seat, n_candidates;         // candidate table fixed at setup
      OracleCand cand[26];            // move descriptor: type,n,cards,targets (bytes)
      _Atomic uint64_t sum_fp[26];    // Σ finish positions — INTEGRAL (fp ∈ 1..8),
      _Atomic uint32_t nsim[26];      //   so u64 atomic adds merge exactly
      _Atomic uint32_t total_worlds, batches;
      _Atomic uint32_t solver_state;  // 0 none, 1 applied+verdicts published
      int8_t verdict[26]; int16_t verdict_val[26];
      _Atomic uint32_t stack_canary_trips;
  } JobControl;
  ```

  Threads add per-candidate batch sums with `__atomic_fetch_add`. The
  per-candidate score sums really are integral: octogen accumulates
  finish positions (ints 1..N) into `double score[26]`
  (`octogen_strategy.c:1602-1697`), so `(uint64_t)llround()` of a batch's
  sum is exact.
- **MT5 — the accumulation hook.** In `octogen_choose_impl`, call
  `og_mt_accumulate(&C, score, nsim, forced_loss, verdicts…)` at the
  full-MC emit point (`:1739-1743` — the ONLY site where `C`, `score[]`,
  `nsim[]`, `alive[]`, `forced_loss[]` are all in scope; verified). The
  four degenerate emit sites (single legal move `:1453-1459`, solver-win
  `:1551-1557`, no-candidates `:1570-1574`, single-candidate
  `:1578-1582`) have no score arrays — there is nothing to accumulate;
  they get a small `og_mt_publish_trivial(...)` that fills the candidate
  descriptor table + solver verdict/`chosen` info only (mirroring the
  NULL-argument `og_ex_emit` forms the explain build uses there).
  Candidate order is deterministic for a fixed
  (state, belief, env) — same insertion-ranked enumeration every thread,
  every batch (`og_pick_candidates`, `:1151-1196`, no RNG) — so index `i`
  is a stable key; the first accumulate CAS-publishes the descriptor table,
  later ones assert agreement (mismatch → drop batch, bump an error
  counter; the snapshot surfaces it).
- **MT6 — endgame short-circuit.** A thread whose choose engaged the root
  solver publishes verdicts + `solver_state=1` under the MT1 spinlock;
  `thread_main` checks it each batch and parks. (Verdict probe machinery
  from the explain build is not present; verdicts come from the solver
  pass octogen already runs — win index + forced_loss flags,
  `:1548-1560,1119-1127`. That is slightly coarser than the explain
  probe's full per-move win/draw/loss table: acceptable for Mode B v1, or
  hoist the probe out of `OG_EXPLAIN_BUILD` behind `FOOLISH_ORACLE_MT`
  later.)
- **MT7 — the thread loop.**

  ```c
  void wasm_mt_thread_main(int tid) {
      for (;;) {
          wait_for_generation_change();        // memory.atomic.wait32 — workers may block
          og_reload_flags();                   // re-read OG_* env for this generation (C1)
          uint32_t gen = load(generation);
          for (uint32_t b = 0; !load(stop) && load(generation) == gen; b++) {
              random_strategy_set_seed(mix3(seed_base, tid, b));
              run_one_batch(seat);             // MT2 path; MT5 accumulates
              check_stack_canary(tid);
          }
      }
  }
  ```

  Env race note: the control instance rewrites the (shared) env table only
  while `stop=1` and threads are parked; the generation bump is the
  release fence. `og_flags_loaded` is `_Thread_local` here, so each thread
  re-latches after `og_reload_flags()` — C1 is reused as plain C (its
  export wrapper is Mode A plumbing).
- **MT8 — control exports** (control instance only):
  `wasm_mt_reserve(n)`, `wasm_mt_setup(seat, w1, seed_base)` (forces the
  bitboard-mask init and clears `engine_snap_hook` — MT3 items 1–2 — then
  resets the table, publishes the job, bumps generation,
  `memory.atomic.notify`),
  `wasm_mt_stop()`, `wasm_mt_snapshot()` (relaxed-load copy of JobControl
  into `g_io`; approximate-while-running is fine for UI),
  `wasm_mt_candidates()` (descriptor table into `g_io` — TS renders labels
  and mini-cards from the raw type/cards bytes; no JSON anywhere).

### 8b.6 Per-thread stacks — the one real safety regression

Mode A inherits `--stack-first` (overflow = loud trap). Mode B's threads
run on heap-region stacks where overflow would silently smash adjacent
memory. Mitigations, all required: 512 KiB per thread (≈ 36× the measured
14.3 KiB worst case, ≈ 23× the shipped 22 KiB stack,
`c/Makefile:631-644`); a canary word at each
stack's low end checked after every batch (`stack_canary_trips` in the
snapshot; UI kills the run if it ever ticks); thread stacks placed at the
LOW end of the reserved region so thread k's overflow walks into thread
k's own TLS block, not another thread's stack.

### 8b.7 Mode B testing

- Node supports shared wasm memory + `worker_threads` natively: mirror the
  §12.2 suite with a `worker_threads` trampoline. Key extra assertions:
  (1) `nThreads=1` Mode B and Mode A converge to statistically identical
  means on the same decisions (KS-style tolerance, not exact — different
  seed streams); (2) `nThreads=8` total `nsim` ≈ 8× the 1-thread rate;
  (3) generation churn (setup/stop storms) never wedges a thread;
  (4) canary counter stays 0 across the full fixture sweep.
- Browser: same §12.3 script, plus assert `crossOriginIsolated === true`
  on the page and that Mode B was actually selected (expose
  `window.__oracleMode` in dev builds).

### 8b.8 Decision guidance

Build Mode B only if, after Mode A ships, one of these is true: fleet
memory (N × module) actually matters on target devices; per-batch
postMessage/JSON overhead shows up in profiles (unlikely at ~25 Hz total);
or the team simply wants the coordination surface in C for maintenance
taste — a legitimate reason, stated openly, since the C side is where this
repo's rigor lives. The COOP/COEP change is the only part with blast
radius beyond the oracle; land it as its own commit with its own rollback
note.

---

## 9. The overlay UI

### 9.1 Placement & controls

- **Oracle button**: joins the transport knob row via the existing `btn()`
  helper (`ReplayScreen.tsx:715-743`, rendered near the eye/pen toggles at
  `:856-874`). Icon: a crystal-ball/scales glyph via the inline `Glyph` SVG
  convention (`:453-507`). `data-testid="oracle-btn"`. Active state amber
  like the others. Disabled (dim + title `oracle_no_decision`) when no
  decision step ≤ stepIdx exists.
- **Panel**: mounts through GameBoard's `chrome` slot (like the status bar /
  Telestrator, `ReplayScreen.tsx:761-888`) as a right-anchored panel
  (max-width ~340px, max-height ~70vh, scroll-y), `z-index` between the
  board (60) and transport (1100). `data-testid="oracle-panel"`. On narrow
  screens it becomes a bottom sheet above the transport row.
- **Header**: decision label ("Seat 3 · cover 9♣→10♥"), memory toggle
  (`data-testid="oracle-memory"`), live status line: spinner + "N worlds ·
  W/s" while running; "converged" chip at checkpoint; "exact" chip in the
  solver regime.
- **Memory toggle**: two-state pill `Memory: on / off` (i18n
  `oracle_memory_on/off`). Toggling changes `decisionId` → full reset +
  restart (per the product decision that resets are fine).

### 9.2 Candidate rows

One row per candidate, sorted by **adjusted score** ascending (best first):

```
[mini-cards]  label      [========== bar ==========]  EF 2.31 ±0.04   (chips)
```

- Mini-cards via `ScaledCard`/`InlineCard` (`ReplayScreen.tsx:46-97`) —
  they must render inside the replay provider tree, which the chrome slot
  satisfies.
- **Bar width** replicates the X-ray formula
  (`gen_html.py:398-434` / `multi_render.py:470-525`): `eff = score +
  trumpTax`; `best = min(eff)`; `span = max(worst-best, 0.4)`;
  `barW% = clamp(12, 100, 96 - ((eff-best)/span)*84)`. Best row gets the
  green accent; the **recorded move** row gets an outline + `played` chip.
- **Score text**: expected finish `EF m.mm ± se` (lower = better; tooltip
  explains "average finishing place over N sampled worlds"). Plus a
  chess.com-style classification chip relative to best, computed on
  adjusted scores: Δ=0 `best`; Δ<0.05 `excellent`; Δ<0.15 `good`; Δ<0.35
  `inaccuracy`; Δ<0.7 `mistake`; Δ≥0.7 `blunder` (constants in one place;
  tune after dogfooding).
- **Trump tax**: `trumpTax = 0.040 × (#trump cards)` for `type==='attack'`
  while `job.deckAlive` — one exported constant `ORACLE_TRUMP_KEEP = 0.040`
  with a comment tying it to `OG_TRUMP_KEEP` default 40
  (`octogen_strategy.c:1480`) and the deployed env
  (`bot_strategy.ts:96`). Taxed rows show `raw +0.08` in the tooltip, as
  the X-ray does.

### 9.3 The "come into focus" animation

Focus factor per candidate: `f = clamp01(1 - se/SE0)` with `SE0 = 0.25`
(one named constant). Map `f` to: value `filter: blur((1-f)*2.5px)` and
`opacity: 0.45 + 0.55*f`, transitioned with CSS (200 ms). At `n=0` the row
shows a shimmering placeholder bar. The intended feel: rows materialize out
of fog over the first seconds and lock crisp as the SE crosses SE0→0.
Numbers gain a decimal as they sharpen (0 decimals while `se>0.15`, 2 when
below). When `converged`, drop the transition and render fully crisp.

### 9.4 Recorded-move matching & pruned moves

Canonical key: `type | sortedCardTokens | sortedTargetTokens` (tokens from
the dump's own card grammar; order-insensitive per the X-ray `normLabel`
precedent, `gen_html.py:579-582`). Build `recordedKey` from the SeatLog the
same way (map card pairs → tokens; trump star derived from `power_suit`).

If the recorded move is **not** among candidates (possible: candidate caps
12/10/3 — §4.1):

- MC regime: append a scoreless row for it with chip `pruned` and tooltip
  `oracle_pruned_tip` ("ranked below octogen's consideration set — it never
  sampled this move"). Honest and informative; no C support needed.
- Exact regime: C3 (§6.3) emits its verdict entry, so it renders as a
  normal verdict row (typically `loss` — the most valuable feedback in the
  product).

### 9.5 Verdict (exact) mode

When `solver.applied` and any candidate verdict ∈ {win, loss}: switch the
panel to verdict mode (mirror `gen_html.py:393-434`): sort by verdict rank
{win, draw, unknown, none, loss, illegal}; fixed bar widths
`win 100% / draw 55% / unknown 45% / loss 16%` with distinct colors; chips
`WIN in 3` using `verdict_val` (encoded ±(1000−depth) — depth =
1000−|verdict_val|); suppress MC decorations; badge `exact` in the header;
stop the fleet. Memory-off caveat: verdicts vanish (solver gate closed,
§5.3) — the panel stays in MC mode; add an info footnote
`oracle_memory_off_endgame` when deck==0 and memoryOn==false.

### 9.6 i18n & theming

Add to the `StringId` union and ALL THREE tables
(`src/localization/strings.ts` — the union is typed; missing a language
fails the build). Keys and English copy (ru/ko drafts follow the file's
existing tone; have them reviewed like the rest of the table):

```
oracle_button_title   "Oracle: move strength"
oracle_no_decision    "No move to analyze yet"
oracle_analyzing      "Analyzing… {n} worlds · {rate}/s"
oracle_converged      "Converged"
oracle_exact          "Exact (solved)"
oracle_memory         "Memory"
oracle_memory_on      "On — octogen remembers the whole game"
oracle_memory_off     "Off — octogen forgets the history (human-like)"
oracle_played         "played"
oracle_best           "best"
oracle_pruned         "not considered"
oracle_pruned_tip     "Octogen never sampled this move — it ranked below its consideration set."
oracle_approx         "Approximate position (some hidden cards inferred)."
oracle_basis          "Based on the publicly visible record."
oracle_memory_off_endgame "Exact endgame proofs need Memory on."
oracle_forced_loss    "proven loss"
oracle_forced_move    "Forced — no alternatives to compare"
oracle_class_best/excellent/good/inaccuracy/mistake/blunder
oracle_unavailable    "Oracle failed to load"
```

**Failure path** (gives `oracle_unavailable` a behavior): on any
fetch/gunzip/instantiate/Worker-construction failure, the panel renders
`oracle_unavailable` with a retry affordance (re-running the §7.2 loader
from scratch), the button stays enabled, the error is `console.error`'d,
and no half-initialized fleet survives (dispose whatever partially
started — StrictMode-safe).

Theming: use `var(--color-text-primary)` / `--color-text-muted`, the
`text-shadow` class, and `useStyles()` for the Soviet branch — copy the
transport row's conventions (`ReplayScreen.tsx:715-743,786-830`). Verdict
colors: reuse the semantic green/red/amber the X-ray pages use, expressed
through CSS vars so both themes read.

### 9.7 Interaction with playback

- Opening the panel does not pause playback by itself, but analysis only
  arms when `!playing && !isAnimating` (from `useAnimation()`); pressing
  play or stepping cancels the current run (`controller.stopCurrent()`),
  and — if the panel is open — a new job re-arms automatically once
  animation settles on the new step (subscribe to `stepIdx` + `isAnimating`
  in an effect).
- Scrub/`jumpTo` (`ReplayScreen.tsx:588-598`) likewise cancels; no oracle
  work runs while animating (workers are idle, so the 500 ms animation
  cadence is untouched).

---

## 10. What runs where (recap table)

| Piece | Thread | Why |
|---|---|---|
| fetch + gunzip oracle.wasm.gz | main, once, lazy | tiny; result posted to workers |
| wasm instances (N) | one per worker | independent-deliberation parallelism, zero shared state |
| marshal/logs/env/choose/dump-read | worker | keeps main thread jank-free; each read between complete calls |
| JSONL parse | worker | ship parsed objects, not text |
| merge, SE, convergence | main (controller) | trivial arithmetic |
| React updates | main, ≤5 Hz | throttled snapshot publish |

---

## 11. Performance & memory budget

| Item | Expected | Knob |
|---|---|---|
| Module size (gz) | ~60-75 KB | measure at build, record in commit |
| Worker count | `min(max(cores-2,1),8)` | `ORACLE_MAX_WORKERS` |
| Linear memory / worker | ~2 MiB static (incl. 1 MiB dump buffer) + 8 MiB TT + growth slack ≈ 12 MB | `CD_TT_BITS` |
| Fleet memory | ≤ ~100 MB at 8 workers | worker count |
| Batch wall time | target 40 ms | `OG_W1` adaptive (§8.6) |
| Worlds/sec (fleet) | measure; naive estimate O(10³-10⁴)/s mid-game | — |
| Checkpoint | 65,536 worlds/candidate or SE ≤ 0.005 or 180 s | constants |
| Endgame probe | ≤ moves × 2M nodes, **per batch while the solver gate passes** — proven → exact stop after batch 1; unproven → defused to budget 0 after batch 1 (§5.4, §8.5-11c) | `OG_EXPLAIN_SOLVE_BUDGET` |
| TT reset memset | 8 MiB per root-solve engage (2× with the probe) — bounded by the same batch-1 stop/defuse rules | — |

---

## 12. Testing & acceptance

### 12.1 Build gates

- `make wasm-oracle` succeeds; `git status` shows ONLY
  `public/oracle.wasm.gz` new — the three shipped artifacts and their
  embeds are untouched.
- Sanity: `make wasm && make wasm-bots && make wasm-guards`, then verify the
  shipped modules are byte-identical — for `bots.wasm.gz` compare the
  **inflated** bytes (its recipe embeds an mtime in the gz header,
  `Makefile:710`), for the embeds a plain `git diff` suffices. This proves
  the `FOOLISH_ORACLE_BUILD` guards leak nothing into shipped builds. Then
  `git checkout` any mtime-only gz churn rather than committing it.
- Existing suites stay green: `npm run test:e2e` (at minimum the
  no-Postgres suites: `replay_codec`, `wasm_engine`, `bot_parity`).

### 12.2 New headless suite — `e2e/oracle_replay.test.ts` (no Postgres)

Load `public/oracle.wasm.gz` via `node:fs` + the vendored gunzip (test-only
loader; the browser bridge is exercised separately). Drive the REAL
`src/oracle/` modules (`logsWire`, `replayOracleInput`, merge logic) — no
second copies. Fixtures: the tutorial 3p code + the two 8p sample codes
(§4.4).

1. **Legality invariant** (the load-bearing one): for EVERY decision step of
   all three replays, marshal the constructed pre-move state and assert the
   recorded move appears in the legal-move set — this validates
   defender/goods derivation (§8.4; `awaiting_attack` is inert and cannot be
   validated this way). Enumerate via the **oracle instance's own**
   `wasm_legal_moves` (4096 cap), NOT `kernelLegalMoves` on the engine slot
   (rules.wasm caps at 1024, `Makefile:417-418`); for 8-player COVER steps
   whose menus can saturate even 4096 (`Makefile:222-233`), fall back to
   direct validation of the single recorded move (apply it via the kernel
   and assert no reject) instead of menu membership. Report the count.
   Additional assertions from §8.4: the good mask is non-empty at a step
   with goods pending, and at any decision after k eliminations every
   candidate mean is ≥ k+1 (catches a silently-empty `elimination_order`).
2. **Batching**: at three mid-game decisions, run 5 batches with distinct
   seeds; assert candidate keys are stable across batches, `nsim` uniform
   per batch, cumulative `n` strictly increasing, and at least two batch
   means differ (fresh seeds actually vary worlds).
3. **Determinism per seed**: same seed twice → identical dump text. Valid
   only while TT state is identical (§4.1): run it on a fresh instance or
   before any heads-up leaf-solving decision — the 3p/8p fixtures dodge the
   pc2 leaf gate (`:1519-1520`), which checks total seats; note why in the
   test.
4. **Memory toggle**: at a decision after the first discard, ON vs OFF
   dumps differ in `belief.pool` size (discards resurrect), and at a
   deck-empty heads-up decision ON yields verdicts while OFF does not.
5. **Exact regime**: at the last decisions of the tutorial game (durak
   always ends heads-up), assert `solver.applied`, win/loss verdicts
   present, and the §6.3 pruned-verdict entries appear when the recorded
   move is outside the candidate set (construct or find such a step; if
   none exists in fixtures, unit-test C3 by asserting entries exist for all
   legal moves at some solved position).
6. **Env reload**: set `OG_W1=8`, batch, read `nsim`; set `OG_W1=16`,
   `wasm_og_reload_flags()`, batch, assert `nsim` grew accordingly.

### 12.3 Browser acceptance (the user-requested proof)

No committed browser harness exists; do this ad-hoc with Playwright.
(Environment note, not a repo fact: if the dev container pre-provisions
browsers — check `echo $PLAYWRIGHT_BROWSERS_PATH` / `ls /opt/pw-browsers` —
install a playwright version whose bundled Chromium revision matches the
provisioned directory and skip `playwright install`; otherwise
`npx playwright install chromium`.)

```bash
npm run dev &                       # port 3000; replay needs no Supabase
# script: scripts/oracle_shots.mjs (playwright, ad-hoc; keep out of deps or
# add as devDependency — builder's call, ROADMAP screenshots were ad-hoc too)
```

Script steps (use the `data-testid` hooks from §9.1):

1. Open `http://localhost:3000/ENSCBI2LBAVUBJJ3J7NODALIBDGEQYLLLICQ`.
2. Step forward (`ArrowRight`) to a mid-game ATTACK/COVER decision.
3. Click `oracle-btn`. Screenshot `oracle-1-open.png` immediately.
4. Wait 2 s → `oracle-2-focusing.png`; wait 4 more → `oracle-3-sharp.png`.
   **Acceptance: the EF numbers/bars visibly differ across the three shots
   and error bars shrink** (values change live).
5. Scrub near the end of the game; wait for a heads-up deck-empty decision;
   screenshot `oracle-4-exact.png` (verdict bars + exact badge).
6. Click `oracle-memory` (off). Screenshot `oracle-5-memory-off.png`.
   **Acceptance: values reset and re-accumulate; verdict bars replaced by
   MC bars + the memory-off endgame footnote.**
7. Repeat 1-4 once on the 8-player 4v4 sample code (big-menu smoke test).
8. Drop the shots in `docs/screenshots/` (`NN-description` convention) and
   reference them from the PR/commit body.

Also verify by hand in dev: no `oracle.wasm.gz` network request before the
first button press; stepping while running cancels cleanly; StrictMode
double-mount doesn't double the fleet (watch worker count in devtools).

### 12.4 Perf measurement to record in the landing commit

Batch ms at OG_W1∈{8,24,96} on the dev machine, fleet worlds/sec mid-game
(3p and 8p), time-to-checkpoint, and endgame first-batch latency at
`OG_EXPLAIN_SOLVE_BUDGET=2000000`. Tune §8.6/§8.7 constants from these.

---

## 13. Build order for the implementing agent

1. **C changes** (§6): C1+C2+C3 behind `FOOLISH_ORACLE_BUILD`. Native
   `make tests` and `make difftests` stay green (they don't define the
   flag; this is a compile-out check).
2. **Makefile target** (§7.1); build; confirm size; confirm shipped
   artifacts untouched (§12.1). Commit artifact separately per convention.
3. **`logsWire.ts` + `replayOracleInput.ts`** incl. the `buildReplaySteps`
   extension; write test §12.2-1 immediately — it validates the hardest
   reconstruction before any UI exists.
4. **`oracleBridge.ts` + `oracleWorker.ts` + `OracleController.ts`**; tests
   §12.2-2..6 (drive the bridge in-process against the wasm; the Worker
   wrapper is thin enough to leave to browser testing).
5. **UI** (§9): button, panel, i18n (all three languages), theming.
6. **Browser acceptance** (§12.3) + perf numbers (§12.4) + constant tuning.
7. Docs touch-up: add one line to `README.md`'s replay section and a
   pointer in `docs/MONETIZATION_ROADMAP.md`'s Cordite Coach item to this
   doc.
8. *(Optional, separate effort — only per §8b.8 guidance)* Mode B: toolchain
   spike (hello-threads: shared memory, TLS init, two workers incrementing
   an atomic), then MT1–MT8, the `wasm-oracle-mt` target, the COOP/COEP
   commit, mode detection in the loader, and the §8b.7 suite. Mode A stays
   the fallback path forever; nothing from steps 1–7 is thrown away.

---

## 14. Later: bringing this to live games (do NOT build now)

The oracle is deliberately replay-page-pure. To attach it to live games
later: (a) post-game, the win screen already has the full session on the
client — the same controller can run on the finished game's decoded
snapshot (the replay pipeline is the cleaner entry: navigate to the
generated replay code); (b) mid-game analysis for spectators/players is a
fairness question (it's real-time engine assistance) — if ever done, it
must be spectator-only and delayed; (c) metering for "Cordite Coach"
monetization needs either a server-issued analysis token gating the UI
(soft) or a server-side oracle endpoint (hard metering, real COGS — the
existing `bots.wasm` infrastructure could serve it at small budgets). None
of this changes the C or the build target — the client fleet is reusable
as-is.

---

## 15. Risks & mitigations (numbered; referenced above)

| # | Risk | Mitigation |
|---|------|-----------|
| R1 | OG_* env latch: knobs frozen after first choose | C1/C2 reload hook; env installed before first choose regardless (§8.5 step 1 runs first) |
| R2 | Identical batches if seeding is forgotten (strategy LCG is read, not advanced) | §8.5 step 5 is mandatory; test §12.2-2/3 catches it |
| R3 | Dump buffer overflow silently drops records | `wasm_og_explain_reset()` every batch (§8.5 step 6) |
| R4 | Endgame probe cost per batch (≤ moves × budget nodes, re-paid every batch while the gate passes) | proven → stop after batch 1; unproven → worker rewrites `OG_EXPLAIN_SOLVE_BUDGET=0` after batch 1 (per-call getenv; §8.5-11c) |
| R5 | Played move pruned from candidates (caps 12/10/3) | §9.4 scoreless `pruned` row; C3 covers the endgame case with real verdicts |
| R5b | Forced/solved decisions (1 legal move, 1 surviving candidate, solver-win with unknowable probe) never accumulate nsim → naive loop spins to the hard cap | FORCED stop rule §8.5-11b + `oracle_forced_move`; convergence computed over n>0 candidates only (§8.7) |
| R6 | TT warmth makes batches non-i.i.d. | harmless to a converging mean; SE from batch means absorbs it; documented, no reset export |
| R7 | Retrodicted mid-game hands ≠ historical truth | §5.2 honesty footnotes (`oracle_basis`, `oracle_approx`); exact at game end |
| R8 | 512-log truncation in ultra-long games | matches the LIVE bot's own cap (parity); raising MAX_LOGS cascades into IO_CAP/overlay sizing — future work, do not do casually |
| R9 | Service worker (if ever revived) pins the asset | filename versioning noted at the loader; SW is dormant today |
| R10 | Worker bundling in Next has no in-repo precedent | verify `new Worker(new URL(...))` first thing in step 4; fallback noted §8.1 |
| R11 | StrictMode double-fires the fleet | run-generation counter + effect cleanup (§8.7) |
| R12 | Main-thread jank | all wasm in workers; publishes throttled to ≤5 Hz; nothing runs while animating |
| R13 | `memory.grow` invalidates cached views | refresh `Uint8Array` on every read (§8.5 step 8) |
| R14 | Client-only oracle is unmeterable vs the monetization plan | stated divergence (§1, §14); acceptable for this phase |
| R15 | 16-slot / 32-byte env table | oracle uses 6 short pairs; adding knobs must respect the caps (`wasm_bots_api.c:83-88`) |
| R16 | defender/goods/elimination reconstruction wrong (esp. the silently-empty `elimination_order` skewing every score, and player_id-string vs numeric-seat mismatches failing silently) | §8.4 reuses the step's existing per-seat flags; §12.2-1 legality + good-mask + post-elimination-EF assertions gate it; `awaiting_attack` is inert (marshal false) |
| R17 | (Mode B) COOP/COEP headers hit the shared `/[game_id]` route — auth popups / cross-origin subresources | credentialless COEP; audit per §8b.2; own commit + rollback note; runtime fallback to Mode A is automatic |
| R18 | (Mode B) freestanding-threads toolchain unknowns (TLS init, passive-segment init, stack-pointer export) | mandatory hello-threads spike before MT1 (§13 step 8); two-artifact strategy isolates it from Mode A |
| R19 | (Mode B) heap-region thread stacks lose `--stack-first` trap-on-overflow | 512 KiB stacks + per-batch canary check + layout ordering (§8b.6) |
| R20 | (Mode B) a missed shared static races under real TLS | MT3 audit is a deliverable, classified in the PR; MT2 keeps threads off the bridge statics entirely |

---

## 16. Open questions intentionally left to the builder

- Exact visual treatment of the focus animation (blur vs opacity vs decimal
  gating — §9.3 gives the mechanism; taste is yours; keep it subtle).
- Whether the panel lists GOOD/PICKUP as rows with mini-card-less labels
  (they have no cards) — yes, just text labels.
- Whether to keep workers warm across steps (recommended: yes, kill on
  unmount) and whether worker 0 should get a bigger TT (heterogeneous
  fleet) — v1 ships homogeneous; leave a TODO.
- ru/ko copy review by a native speaker before merge.
