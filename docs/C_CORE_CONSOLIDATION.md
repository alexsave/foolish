# What else belongs in C — report & action plan

*Investigation, July 2026. Question asked: with an offline iOS MVP coming,
the server's bot loop suddenly needs a client twin — and "do not repeat
yourself" is a founding principle here. What ELSE should sink into the C
core so it is written once and reused by the server, the iOS app, and the
watch/telegram/steam clients later? And which client-side functionality
(animation? guards-style logic?) should follow guards.wasm into shared
C+WASM?*

*This is a report + action plan only — no code was changed. Every claim was
verified against the tree at `ffcd39b` (two independent read-only sweeps:
server loop anatomy; client logic inventory). Companion docs:
`ARCHITECTURE_AS_A_PATTERN.md` (the doctrine), `RULES_DUPLICATION_FINDINGS.md`
(the rules sweep this extends), `PACKED_WIRE_CUTOVER.md` /
`STATE_BLOB_CUTOVER.md` (the wire story).*

---

## 0. Executive summary

The scary version of the question — "we need a whole client-side bot loop" —
is already false: **the kernel already picks and applies bot moves natively**
(`fio_actor_mask` + `fio_bot_step_json`, driven by
`ios/FoolishKit/Engine/LocalGame.swift`), because rules, legality, bot
brains, deal, refill, masking, and the replay codec are all C already. What
remains duplicated is thinner but real, and it clusters into **four
findings**, ranked by leverage:

| # | Finding | Duplicated today in | Verdict |
|---|---|---|---|
| F1 | **The animation plan** — the "which card flies where, in what order" sequence | C (`evwire.c`, server-emitted) · TS mirror (`evwire.ts`+`view.ts` decode) · **planned Swift re-derivation (`BoardDiff.swift`, unwritten)** · TS replay twin (`src/replay/view.ts`+`animate.ts`, ~800 lines) | **Consolidate now — before BoardDiff is born.** Kernel emits the plan for every consumer. |
| F2 | **The bot conductor** — eligibility scan → fair pick → choose → apply → pacing | TS (`bot_actions.ts` cycle, 3000/300 ms policy) · Swift (`LocalGame.runBots`, 600–1200 ms policy) · every future client | **Consolidate the cycle + pacing policy into one C step call.** Transport/lease/CAS stay TS. |
| F3 | **Wire decode on the web** — packed view/evwire/awire → JS objects | C (`view.c` etc.) · ~960 lines of parity-policed TS mirrors (`@shared/wire/*`) · iOS does it in C already | Medium priority: mirrors are tested and cheap, but every wire change pays twice. Fold into the client wasm when next touching the wire. |
| F4 | **The iMessage envelope + concurrency rules** (`msg_wire.c`, Rule P, rebase) | Nowhere yet — greenfield | Already decided (IMESSAGE_GAME_DESIGN §20/H0): **born in C**, never in TS/Swift. Listed here so the pattern is explicit. |

Everything else surveyed is either correctly TS/Swift (transport, storage,
lease/CAS, React/SwiftUI rendering), already sanctioned thin projections
(parity-policed), deliberately-advisory heuristics, or too small to matter
(§4). The action plan is §5.

---

## 1. Ground truth: what is already single-sourced in C

Verified inventory (the baseline that makes the rest tractable):

- **Rules, end to end**: deal/shuffle (`game.c start_game` + `deal_rng.c`
  ChaCha20), validation + application (`handle_attack/cover/pass/pickup/good`),
  refill, round transition, elimination, win detection (`wasm_finalize_win`),
  whose-turn (`should_bot_act`). TS `actions/*.ts` are marshaling shims.
- **Bot brains**: every production strategy in `*_strategy.c`, dispatched by
  `wasm_choose_move` (`wasm_bots_api.c:216-252`); only the chosen move's
  bytes cross the boundary.
- **Per-viewer masking**: `view.c` (server) — "you only see your own hand"
  is computed in C, not TS.
- **All four wire formats**: awire (actions), view (masked state), evwire
  (animation events), state blob (durable) — C structs/codecs; TS has
  read/write mirrors (F3).
- **Replay codecs v2–v5 + v6** (`replay.c`), including the new
  hidden-state-lossless v6 with legal mid-game cut.
- **Client legality**: guards.wasm (validate-only kernel, 1 wasm page) — the
  precedent this report generalizes: the client asks the kernel, never
  re-implements.
- **Native iOS bridge**: `cnitro/ios/ios_api.c` (`fio_*`, JSON out) — proof
  the core reuses cleanly outside wasm, per `ARCHITECTURE_AS_A_PATTERN.md`'s
  "offline is a consequence, not new work."

Four sanctioned TS projections remain by design (`canCover`, `game_done`,
`get_next_player_index`, `shouldBotActCore`) — parity-policed by
`e2e/wasm_engine.test.ts`. Not findings; working as intended.

## 2. F1 — The animation plan (highest leverage; act before Milestone B)

**Facts.**
- On the server path, the animation plan already comes from C:
  `wasm_events_serialize(viewer, actor, ended)` → the evwire stream, one
  masked snapshot per step (`cnitro/src/evwire.c`; TS twin `buildEvents` in
  `_shared/wasm/engine.ts:991` for JS-object paths, byte-parity e2e'd).
- The web client therefore never derives animations: it decodes evwire
  (`@shared/wire/evwire.ts`) and plays it back
  (`AnimationContext.processAnimationQueue`).
- The iOS design, however, planned a **from-scratch Swift diff engine** —
  `BoardDiff.swift`, "given (old GameView, new GameView) produce moves"
  (`IOS_APP_DESIGN.md` §16.B4) — because offline there is no server to send
  evwire. **It is not yet written** (`ios/README.md` "the board springs on
  state change"). The watch, telegram, and steam clients would each face the
  same fork: consume events or re-derive diffs.
- Separately, replay playback re-derives per-step board states in pure
  client-only TS: `src/replay/view.ts` (461 lines: `buildReplaySteps`, v5
  retrodiction, v6 exact hands, complement deduction) + `src/replay/animate.ts`
  (333 lines: synthesizes forward+reverse animation sequences shaped like the
  live broadcast). iOS has begun its own twin (`DecodedReplay.swift`, board
  projection listed as a known gap).

**Finding.** The plan-computation logic exists ONCE in C (`evwire.c` fires at
the same hook points the old TS handlers did) — but only the *online server
path* uses it. Offline iOS was about to re-implement it in Swift, and replay
playback already re-implements a cousin of it in TS. That is the exact
pattern guards.wasm was built to kill.

**Consolidation.**
1. **`fio_apply_json_with_events` / `fio_bot_step_json` gain an events
   output** (kernel already records snapshots per action; `ios_api.c` adds a
   JSON emitter over the same evwire data — precedent: every other `fio_*`
   JSON accessor). `LocalGame` then feeds the SAME event stream the web
   plays, and `BoardDiff.swift` is never written. SwiftUI renders events with
   `matchedGeometryEffect` exactly as §16.B4 planned — only the *source* of
   moves changes from "Swift diff" to "kernel events."
2. **Replay steps in C**: `replay.c` already replays the game to decode it;
   add a step-emitting decode (`fio_replay_steps_json` / a
   `wasm_replay_steps` export) that returns per-event board snapshots via the
   same snapshot hooks. `src/replay/view.ts` shrinks to a consumer;
   `animate.ts`'s sequence synthesis becomes unnecessary on any client that
   plays kernel events; iOS replay board projection (a listed §17.7 gap)
   comes free.
3. What stays per-platform, correctly: rendering (React DOM measurement in
   `AnimationOverlay.tsx`, SwiftUI springs), queue/timing UX, and the web's
   optimistic-conflict layer (§4.3).

**Cost/risk.** Small C surface (emitters over existing data), the parity
pattern already exists (evwire e2e). Timing matters: this should land
*before* iOS Milestone-B animation work, or the Swift diff engine gets
written and becomes legacy on day one.

## 3. F2 — The bot conductor (the question that started this)

**Facts.** The server loop (`_shared/bot_actions.ts`) is ~470 lines of TS.
Dissected, it is two very different things interleaved:

| Concern | Lines/behavior | Nature |
|---|---|---|
| Lease + heartbeat (`try_acquire_bot_lease`, renew, `bot-heartbeat` cron scan) | infra | **Server-only forever** (multi-writer world). |
| CAS commit, broadcast, belief-log persistence | infra | Server-only forever. |
| CPU-budget predictor vs Supabase's ~2s cap | infra | Server-only forever. |
| **Eligibility scan** (`shouldBotActCore` + legal-move check per AI seat) | game | Kernel-answerable today (`should_bot_act`, `fio_actor_mask`). |
| **Fair pick** (Fisher-Yates among eligible bots) | game | Trivial but *behavioral* — affects which bot races first. |
| **Choose + apply** (`wasm_choose_move` → `runPackedGameAction`) | game | Already C. |
| **Passive bundling** (zero-event good/wait folds into next cycle) | game | TS-only rule, affects perceived pacing. |
| **Pacing policy** (3000 ms with humans / 300 ms bots-only) | product | TS constants. iOS re-invented its own (600–1200 ms random + thermal fallback in `LocalGame.swift:116-179`). |

**Finding.** The genuinely shared part — *"given a game, which bot acts next,
what does it play, and how long should it pretend to think"* — is small but
it is **product behavior**, and it now exists twice with different
personalities (server bots deliberate 3000 ms; iOS bots 600–1200 ms). Watch,
iMessage-adjacent hotseat, telegram, and steam would each roll a third and
fourth opinion. Drift here is user-visible ("the app's bots feel different
from the site's").

**Consolidation.** One C entry point, exposed to both worlds:

```c
// engine_bot_cycle: scan eligible AI seats (ai_mask), seeded-fair-pick one,
// choose its move, apply it, and report what happened + a pacing hint.
// Returns: acted seat (or -1), events (evwire/JSON per F1), and
// pace_class ∈ {INSTANT (passive/bundled), BOT_ONLY, HUMANLIKE} — the
// canonical delay policy table lives beside it in C.
int engine_bot_cycle(uint32_t ai_mask, uint32_t human_mask, BotCycleOut *out);
```

- The **server** keeps `lockedBotLoop`'s shell (lease, CPU budget, CAS,
  broadcast, `setTimeout(pace_ms)`) and deletes its scan/pick/bundle logic —
  it calls the cycle through bots.wasm and sleeps the hint. Behavior is
  pinned by the existing determinism/e2e suites (the Fisher-Yates must move
  into C with a seeded RNG so replays of bot games stay reproducible —
  the kernel already has the deterministic RNG plumbing from
  `GAME_DETERMINISM_FIX.md`).
- **iOS `LocalGame.runBots`** shrinks to: call cycle → animate events (F1) →
  `Task.sleep(paceHint)` → repeat; keeps only the thermal downgrade (a
  platform concern, correctly Swift).
- Delay *values* become one C table (`pace_class → ms range`), so tuning bot
  "personality" tunes every surface at once.

**Cost/risk.** Modest; mostly moving decisions, not inventing them. The
subtle bits to preserve byte-for-byte: passive bundling, the
belief-log-hydration trigger (`strategyUsesLogs` → the kernel already owns
belief import via `wasm_import_logs`), and cached-move replay after CAS
conflict (server-only, stays in the shell).

## 4. F3 and the rest of the client inventory

### 4.1 F3 — web wire decode (TS mirrors of C codecs)

`@shared/wire/view.ts` (358) + `evwire.ts` (253) + `awire.ts` (170) +
`logwire.ts` (181) ≈ **960 lines of pure TS that shadow C structs
byte-for-byte**, kept honest by e2e parity. iOS already went the other way
(decode in C, `fio_view_from_packed_json`). Two defensible positions:

- *Keep the mirrors*: they are small, tested, and the only remaining
  consumers are the web client + edge functions (telegram would reuse the web
  stack; steam/native reuses C directly). Cost is per-wire-change, bounded.
- *Fold decode into the client wasm*: guards.wasm already links `view.c` and
  imports view blobs; adding evwire decode + a JSON/typed-array emitter kills
  the two biggest mirrors at the render boundary.

**Recommendation:** do it opportunistically — the next time a wire format
changes (variants v6 header work is a natural trigger), move that format's
client decode into wasm instead of updating the mirror. ⚠ Budget note: the
guards module is deliberately pinned at ONE wasm page
(`RULES_GUARDS_WASM_MEMORY_PLAN.md`); event decode + JSON emit will not fit —
this lands either as a second tiny module or as a deliberate, documented
guards budget bump. Decide with the memory-plan discipline, not by accident.

### 4.2 Small shared-candidate accessors (cheap wins, batch them)

- `fio_unambiguous_cover` / `wasm_unambiguous_cover`: the "one-tap cover"
  affordance (`coverCombinations.ts` `findUnambiguousCover`, 95 lines) is
  needed by web drag, phone tap-commit, watch chooser (`WATCHOS_SPEC.md` §5),
  and iMessage. Pure set logic over legality — belongs beside `legal.c`.
- Card sort/display order helpers if the phone ships auto-sort (the web's
  `reorderHand`/`isHandPermutation` stay client-side; the kernel already owns
  rearrange legality via `wasm_rearrange_hand`).
- The FMSG **Rule P comparator and rebase** (F4): implement in `msg_wire.c`
  from day one; e2e drives it through wasm, XCTest through libfoolish
  (already specified in `IMESSAGE_GAME_DESIGN.md` §20/H0 — reiterated here
  because it is this report's pattern applied to new code: *concurrency
  rules are game rules; born in C*).

### 4.3 Surveyed and deliberately NOT moved

| Candidate | Verdict |
|---|---|
| Optimistic overlay/conflicts (`optimisticConflicts.ts`, ~400 entangled lines in `AnimationContext`) | Web-specific UX today; iOS Stage C1 ships without optimism, C2 plans a fixture-driven port. Revisit only if C2 actually happens — and then consider the event-sourced Core from `ARCHITECTURE_AS_A_PATTERN.md` rather than transliterating. |
| ELO / rankings (`utils.ts:1026-1169`) | Server-only concern; offline games are unrated. Port nothing. |
| Lease/CAS/heartbeat, broadcast, persistence | Multi-writer server infra; no client analog (offline is single-writer synchronous). |
| GPT strategy adapter | Non-algorithmic, network-bound, TS by nature. |
| Oracle input prep (`src/oracle/`, ~1000 lines) | Pure-ish but web-monetization-only today; the native Oracle plan already reuses the C oracle build when it ships. |
| Rendering everywhere (`AnimationOverlay.tsx`, SwiftUI boards) | Platform code, correctly per-platform. |
| Belief heuristics (`move_stats.ts`, `pass_prob.ts`) | Advisory-only by policy (documented in `RULES_DUPLICATION_FINDINGS.md`); never gate a move. |
| Timers | None exist in the engine (no round timer) — nothing to consolidate. |

### 4.4 Bugs/inconsistencies noticed in passing (not consolidation, still real)

- **iOS renders raw `%`-prefixed bot names online** (`PackedGame.swift`
  roster merge → `FSeatBadge`) — no `botDisplayName` port exists. Fixed as a
  side effect of the naming work: `docs/IOS_BOT_NAMING.md` §5 specifies
  `BotName.swift` at that exact choke point.
- **The web live board shows raw `%` too** (`PlayerRing.tsx:184` uses the
  unstripped name; only Leaderboard/MatchHistory/Lobby-add-button strip it).
  Cheap web tidy whenever the board is next touched.

## 5. Action plan

Ordered so nothing blocks the current push (offline iOS MVP → iMessage), and
so no consolidation happens *after* a duplicate has been freshly written.

| # | Action | When | Size | Acceptance |
|---|---|---|---|---|
| **A1** | **Kernel event emitter for local play** (F1.1): `fio_*` events output wired into `LocalGame`; delete `BoardDiff.swift` from the iOS plan (amend `IOS_APP_DESIGN.md` §16.B4 to "consume kernel events") | Before iOS Milestone-B animation work — effectively next | S–M (C emitter over existing snapshots + Swift consumption) | Offline move animates from kernel events on simulator; e2e asserts native events ≡ server evwire for a seeded game |
| **A2** | **`engine_bot_cycle` in C** (F2): scan+pick+bundle+pace-hint; server `bot_actions.ts` thins to shell; `LocalGame.runBots` thins to sleep-and-render | With A1 (same milestone; they share the events plumbing) | M | Arena fingerprint + `bot_parity` + determinism suites unchanged; server cycle behavior byte-identical under e2e; iOS and server share the pacing table |
| **A3** | **`msg_wire.c` with Rule P + rebase in C** (F4) | H0 of the iMessage plan (already scheduled) | M | `e2e/msg_wire` + `e2e/msg_concurrency` green through wasm; XCTest parity through libfoolish |
| **A4** | **Replay steps from the kernel** (F1.2): `wasm_replay_steps`/`fio_replay_steps_json`; `src/replay/view.ts` becomes a consumer; iOS replay board projection closes its §17.7 gap | After A1 (reuses emitter), before native replay polish | M | Web replay renders identically (snapshot tests); iOS plays a web-generated code step-for-step |
| **A5** | **Client wire decode into wasm** (F3), format-by-format, starting with whichever wire changes next; new module or documented guards budget bump | Opportunistic | M | Mirror file deleted per format; parity test flips from TS-vs-TS to wasm-vs-fixture |
| **A6** | **Small accessors batch** (§4.2): `fio/wasm_unambiguous_cover` first | With the first surface that needs it (phone tap-commit or watch chooser) | S | Web `coverCombinations.ts` delegates; watch/phone use it natively |
| **A7** | Name-display tidies (§4.4) — iOS `BotName.swift` (already specced), web `PlayerRing` strip | With the naming work | XS | No `%` visible on any surface |

**Global disciplines** (unchanged from the repo's standing pattern, restated
because every action above must follow them): every move of logic into C
ships with a differential test against the retiring implementation (frozen as
oracle, not deleted, until parity runs green at fuzz scale); wasm memory
budgets are explicit line items (`WASM_L1_BUDGET.md`); the three sacred
invariants from `CONSOLIDATION_PLAN.md` (arena fingerprint, parity/fuzz/
adversarial suites, frozen oracles byte-identical) gate every step.

**What this buys, concretely:** the offline iOS app animates and paces bots
with zero new game logic in Swift; the watch app inherits the same for free
(it was already snapshot-driven by design); the iMessage extension's hardest
logic (concurrency) is testable in CI without a Mac; the web sheds up to
~1,800 lines of parity-maintained TS over time (960 wire + ~800 replay); and
"the bots feel the same everywhere" becomes a compile-time property instead
of a hope.
