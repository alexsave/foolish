# What else belongs in C — kernel reuse audit & action plan

*Investigation, July 2026 — merged from two independent audits of the same
question (one traced the server/iOS game loop, the other the client-side
logic surface); their findings are unioned here and the originals retired.
Trigger: the offline iOS MVP needs a client-side "bot loop" — a thing that
has only ever existed server-side — and the project's standing rule is "do
not repeat yourself: rules live once, in C." This doc inventories everything
that sits BETWEEN the C kernel and each platform, finds what is duplicated
(including two live behavior divergences), and proposes the C additions that
make the server, the iOS offline client, and every future client (watchOS,
iMessage, Telegram, Steam, `/m/` web-play) thin shells around the same
machine code. (Scope trim, owner decision 2026-07-15: the two I/O-adapter
bot strategies — **`console`** (stdin) and **`gpt`** (LLM) — are DROPPED;
neither is worth dragging through every plan. Deletion rides A7 in §6.)
No code has been changed; this is the report + work order.*

*Companions: `ARCHITECTURE_AS_A_PATTERN.md` (the doctrine),
`RULES_DUPLICATION_FINDINGS.md` (the earlier rules sweep this extends),
`PACKED_WIRE_CUTOVER.md` / `STATE_BLOB_CUTOVER.md` (the wire story).*

---

## 0. Executive summary

The scary version of the question — "we must build a whole client-side bot
loop" — is already false: **the kernel already picks and applies bot moves
natively** (`fio_actor_mask` + `fio_bot_step_json`, driven by
`LocalGame.swift`), because rules, legality, bot brains, deal, refill,
masking, and the replay codec are all C already (§1). What remains
duplicated is thinner but real — and one piece is **already wrong in the
field** (§3). Findings, ranked by leverage:

| # | Finding | Duplicated today in | Verdict |
|---|---|---|---|
| F1 | **Bot roster: key → brain + tuning knobs + logs flag** | TS registry (`bot_strategy.ts:63-99`) · C table in `ios_api.c:37-48` **without knobs** → live strength/latency divergence (§3) | **DONE** (§4.1): `cnitro/src/bot_roster.c` is the one table; the phone's knob + arena/prod drift is fixed. |
| F2 | **The bot drive cycle** (eligibility → fair pick → apply → passive bundling → stop conditions) | TS (`bot_actions.ts:262-411`) · Swift+C first-eligible walk (`LocalGame.runBots`) — with a second live divergence: iOS picks first-eligible, not shuffled | **`bot_drive` (§4.2): kernel + iOS DONE**; the server port remains. |
| F3 | **Pacing policy** (what a move is worth pausing for) | TS constants 3000/300 ms + silent-skip · Swift 600–1200 ms jitter, no bundling | **DONE (§4.3)**: `bot_pacing_ms` is the one class→ms table; the server's values won. |
| F4 | **The animation plan** (which card flies where, in what order) | C (`evwire.c`, server-only today) · TS decode mirrors · **planned Swift re-derivation `BoardDiff.swift` (unwritten)** · TS replay twin (`src/replay/view.ts`+`animate.ts`, ~800 lines) | **F4.1 DONE (§4.4)**: `evwire_walk` + sinks; local play consumes kernel events and BoardDiff was never born. F4.2 (replay steps) = A5. |
| F5 | **v6 replay production** (reveal-stream assembly at game end) | TS choreography (`finalizeEndedGame` + `reconstructSeededDeal` + `replay/encode.ts`) · absent on iOS (offline shares are v5-only) | `replay_encode_v6_from_game` in C; server finalize becomes call-verify-store; offline shares gain exact hands. |
| F6 | **Rematch / reset-to-lobby transform** | TS 10-field mutation (`meta_actions.ts:188`) · client mirror (`clientReconcile.ts:10-40`, "must match byte-for-byte") · specced for a third port in iOS M-D5 | `wasm_reset_to_lobby` / `fio_reset_to_lobby`; all mirrors become decode-and-render. |
| F7 | **Wire decode on the web** (packed view/evwire/awire → JS) | C codecs · ~960 lines of parity-policed TS mirrors (`@shared/wire/*`); iOS already decodes in C | Fold into client wasm opportunistically, format-by-format (⚠ guards.wasm memory budget, §4.7). |
| F8 | **TS rules projections** (`shouldBotActCore`, `canCover`, `game_done`, `get_next_player_index`) | sanctioned, parity-policed mirrors in `common_utils.ts` | After F2 removes their last real consumer: migrate callers to kernel calls and delete (or demote to documented render-hints). |
| F9 | **Small shared invariants & accessors** | scattered | One header for `FOOLISH_SEED_LEN 32` (+ encode/decode rejects); `fio/wasm_unambiguous_cover` for the one-tap-cover affordance (web drag, phone tap-commit, watch chooser, iMessage); FMSG `msg_wire.c` with Rule P/rebase **born in C** (already the iMessage plan). |

Everything else surveyed is correctly per-platform (§5). Action plan: §6.

## 1. Ground truth: what is already single-sourced in C

- **Rules end to end**: deal/shuffle (`game.c start_game` + `deal_rng.c`
  ChaCha20), validation + application (`handle_*`), refill, round
  transition, elimination, win detection (`wasm_finalize_win`), whose-turn
  (`should_bot_act`). TS `actions/*.ts` are marshaling shims.
- **Bot brains**: every production strategy in `*_strategy.c`
  (`wasm_choose_move`, `wasm_bots_api.c:216-252`); only the chosen move's
  bytes cross the boundary.
- **Per-viewer masking**: `view.c` — "you only see your own hand" is
  computed in C.
- **All four wire formats**: awire, view, evwire, durable state blob.
- **Replay codecs v2–v5 + v6** (`replay.c`), incl. v6's legal mid-game cut.
- **Client legality**: guards.wasm (validate-only kernel, 1 pinned wasm
  page) — the precedent this whole doc generalizes.
- **Native iOS bridge**: `cnitro/ios/ios_api.c` (`fio_*`, JSON out) — the
  proof the core reuses outside wasm; `ARCHITECTURE_AS_A_PATTERN.md`'s
  "offline is a consequence, not new work," realized.

## 2. Method

Two production hosts run games end-to-end today — the server
(`bot_actions.ts` leased loop → `pure_bot_actions.ts` → `wasm/bots.ts` →
kernel; CAS commit; packed broadcast) and iOS offline (`LocalGame.swift` →
`EngineC.swift` → `ios_api.c` → the same kernel, statically linked). For
every responsibility in the game path, classify: **[K]** already kernel,
**[D]** duplicated per-host logic that belongs in C, **[P]**
platform-inherent. The [D]s are §0's findings. The client sweep additionally
inventoried every pure-logic module a THIRD client (watch/telegram/steam)
would need, which is where F4/F7/F9 come from.

## 3. The motivating bugs (found by this audit — live divergences)

*Bug 1 is FIXED by A1 (§4.1), along with a third divergence this audit missed:
`ios_api.c` pointed the `handwritten`/`espresso` rungs at the arena variants
rather than the production mirrors, so offline Handwritten was a different bot
from the site's regardless of knobs. Bug 2 is FIXED on the phone by A2 (§4.2):
`bot_drive` is the one cycle, so the seat-order advantage and the per-passive
padding are gone. The server still runs its own loop until the A2 port, but the
two now agree by construction rather than by hope.*

1. **Offline cordite is not the website's cordite.** The C strategies read
   tuning through env: `CD_BUDGET` defaults to **0 = arena mode**
   (`cordite_strategy.c:90`), and the server sets `CD_BUDGET=prod|max`,
   `CD_RACE=1`, `CD_RACE_C=75` per roster entry via the wasm env table
   (`bot_strategy.ts:84-98`). The iOS dispatcher (`ios_api.c
   dispatch_choose`) sets **no env at all** — on the phone cordite runs at
   the arena budget with early-race off (different strength, different
   latency), and `octogen` matches the site only because `OG_TRUMP_KEEP`'s C
   default happens to equal today's server override; the next server-side
   knob change silently forks the phone. This is exactly the drift class the
   one-kernel rule exists to prevent — and why the roster must be C data,
   not three tables (F1).
2. **Offline bot fairness differs from the site.** The server picks among
   simultaneously-eligible bots with a Fisher-Yates shuffle
   (`bot_actions.ts:314-318`); `fio_bot_step_json` walks seats
   first-eligible. In 8-player bot-heavy games the phone's seat order gets a
   systematic tempo advantage the site doesn't have. Also: the server
   bundles zero-event passives (silent "good"s coalesce), iOS pays the full
   UX delay per silent action — an 8-player round with five passives feels
   padded on the phone (F2/F3).

## 4. Findings in detail

### 4.1 F1 — One canonical bot roster table, in C

**STATUS: LANDED (A1, July 2026)** — `cnitro/src/bot_roster.{h,c}` +
`cnitro/src/bot_knobs.{h,c}` exist; `ios_api.c` consumes them (§3.1 and the
arena/prod mix-up below are fixed); the `_max` tiers are gone; parity is pinned
by `e2e/bot_roster_parity.test.ts` + the roster tests in `cnitro/tests/tests.c`.
Four notes on how it landed versus how it was specced here:

1. **Two flags, not one `shipped`.** The seeded site ladder and the offline
   picker are different sets in *both* directions — `espresso`/`robusta`/
   `gunpowder` are offline-only rungs, and the `_max` tiers were seeded-only —
   so one boolean could not express membership. The entry carries `seeded` and
   `offline`; `fio_strategy_*` is the `offline` projection in tier order.
2. **The `_max` tiers were deleted, not modelled** (owner decision). They were
   never distinct bots: `octogen_max` registered the *same* brain with the
   *same* knobs as `octogen` (an admitted alias), and `cordite_max` was
   `CD_BUDGET=max` — a FLAT 120/240/168 world budget, versus `prod`'s
   player-count-aware schedule (240/480/336 at 6p). So "Max" only out-sampled
   plain Cordite at 2-4 players and ran at ~HALF its budget at 6-8: the tier
   advertised as stronger was the weaker bot in the bigger games. One
   `cordite`, on `prod`. Migration `20260715120000_drop_max_bot_tiers`.
3. **A third live divergence, found while doing this.** `ios_api.c` mapped
   `handwritten`→`STRAT_HANDWRITTEN` and `espresso`→`STRAT_ESPRESSO` — the
   *arena/rollout* variants, which drifted from the production bots and stay
   frozen because cordite's rollout policy is tuned against them. The site maps
   `handwritten`→`STRAT_HANDWRITTEN_PROD`. Offline "Handwritten" was therefore
   not the site's Handwritten at all, independent of any knob. The roster points
   both rungs at the `_PROD` mirrors.
4. **Knob precedence is env > roster > C default** ("env vars kept as research
   overrides", below). This is what let A1 land as a **no-op on the server**:
   the TS registry still writes an identical env table, so bots.wasm is
   unchanged and the parity suites cannot move, while the phone — which set no
   env at all — gets the right knobs immediately. Deleting the TS `env` blocks
   is the cutover, and it rides with folding `wasm_choose_move`'s switch onto
   the roster (that pairing is deliberate: linking `bot_roster.c` into bots.wasm
   drags in `espresso_prod`/`gunpowder` for two rungs the server never seeds, so
   it is the same change that rewrites `bots.wasm.gz` and wants the CI
   toolchain). `bot_knobs.c` alone is already in the wasm module and is inert
   there — with no roster spec installed, `bot_knob()` *is* `getenv()`.

The specced shape, for reference:

```c
typedef struct {
    const char *key;        // "octogen", "cordite_max" — the DB strategy_key
    int         strat;      // STRAT_* brain id
    const char *knobs;      // "CD_BUDGET=prod,CD_RACE=1,CD_RACE_C=75" or ""
    uint8_t     uses_logs;  // belief bot: hydrate session log before choosing
    uint8_t     shipped;    // part of the public ladder (seeded / offline roster)
    uint8_t     tier;       // ladder order for pickers/display
} BotRosterEntry;
const BotRosterEntry *bot_roster(int *count);
int  bot_roster_find(const char *key);              // -1 if unknown
int  bot_roster_choose(int idx, const Game *g, int seat,
                       const LegalMoves *m);        // applies knobs, dispatches
```

Consumers: `wasm_bots_api.c` (replaces its dispatch switch; the TS registry
shrinks to a thin `key → wasm call` shim with NO exceptions — the two
I/O-adapter strategies, `console` (stdin; only ever the `CONSOLE` key in
`types.ts:254` plus the "manually include" comment) and `gpt` (LLM; the
lazy-load path in `getBotStrategy` + `strategies/gpt_strategy.ts`), are
dropped rather than carried; with `gpt` go its advisory helpers
`strategies/move_stats.ts` / `pass_prob.ts` once unreferenced, retiring the
deliberate heuristic-layer duplication documented in
`RULES_DUPLICATION_FINDINGS.md`), `ios_api.c`
(replaces `ROSTER[]` + `dispatch_choose`, fixing §3.1), and a tiny wasm
export so e2e can assert `seed.sql` seeds exactly the `shipped` set (today
that invariant lives in a SQL comment). `uses_logs` rides along so every
host hydrates belief bots identically. Knob application becomes
`bot_roster_choose` setting values directly (struct-driven, not getenv),
with env vars kept as research overrides. `tier` + `key` is also the join
column for the localized display names (`docs/IOS_BOT_NAMING.md`).

### 4.2 F2 — The bot drive cycle becomes a kernel entry point

**STATUS: kernel + iOS LANDED (A2, July 2026); the server port is the next
step.** `cnitro/src/bot_drive.{h,c}` is the cycle; `fio_bot_drive_json` and
`wasm_bot_drive` expose it; `LocalGame.runBots` is now one call per cycle, so
the phone's fairness and bundling divergences (§3.2) are gone. `bot_actions.ts`
still runs its own loop — porting it onto `wasm_bot_drive` behind the
differential harness is what remains. Decisions taken while building it:

1. **A human being able to act is NOT a stop condition** (owner decision). The
   spec above reads "stop: human seat eligible", which is the *phone's* rule;
   the site has always driven bots regardless. They are not interchangeable:
   `should_bot_act` makes the defender and every not-yet-good attacker eligible
   AT ONCE, so yielding to any eligible human stops bots throwing in while a
   human deliberates — a large online gameplay change that can also stall a
   bout on an idle player. `human_mask` therefore means "seats the kernel must
   not drive", full stop, and the phone adopts the site's rule.
2. **The shuffle is seeded from PUBLIC state, not the game RNG.** The spec says
   "seeded shuffle (game RNG)", but drawing from that stream would shift every
   subsequent refill and break "reproducible from the deal seed". A hash of the
   public board varies per action, is identical on every host, and replays
   exactly — with none of that cost.
3. **The offline-only rungs are not linked into bots.wasm.** The roster's
   dispatch would otherwise drag `espresso_prod` + `gunpowder` in for two rungs
   the server can never seed: measured at **+5.1 KB gzip** (54.1 → 59.2 KB) on
   every edge cold start. `-DFOOLISH_SEEDED_BOTS_ONLY` keeps the shipped module
   to the seeded ladder (net cost of the whole A1+A2 kernel: +0.3 KB gzip over
   the pre-A1 artifact). The roster TABLE is unchanged everywhere; only which
   brains a build can RUN differs, which was already true of CORE_SRC vs
   WASM_BOT_SRC. This is §4.7's "deliberate, documented budget decision".

The server loop's INNER CYCLE (`bot_actions.ts:262-411`) is game logic in a
TS coat: find eligible bot seats, pick one **shuffled** (fairness), choose
via roster, apply, **bundle zero-event passives**, stop when a human becomes
eligible / game ends / an event-bearing move lands. Proposal:

```c
// Drive bot seats until a stop condition. Applies 0..n actions.
// stop: human seat eligible | game over | event-bearing action applied
//       | max_actions reached. Passive zero-event actions bundle.
// Selection among simultaneously-eligible bots: seeded shuffle (game RNG) —
// identical fairness on every host, reproducible in replays/tests.
int bot_drive(Game *g, uint32_t human_mask, int max_actions, BotDriveOut *out);
// out: n applied (seat+awire each), events (see F4), ended, paused_reason,
//      pacing_class per action (see F3)
```

Exported as wasm + `fio_bot_drive_json`. The server keeps: lease, CAS commit
of returned products, broadcast, CPU budget (`max_actions`/repeat-call maps
onto its measured-cost bailout), cached-move replay after CAS conflict. iOS
keeps: timers, thermal downgrade (a roster-tier override between calls),
rendering. Telegram/Steam later: the same three calls (`new_game`, `apply`,
`bot_drive`). Verification is the standing playbook: seeded games through
the old TS cycle vs `bot_drive`, byte-compare committed products.

### 4.3 F3 — Pacing policy as shared data, not per-host constants

**STATUS: LANDED (A2).** `bot_pacing_ms(class, humans_present)` in
`bot_drive.c` is the one table, reaching Swift as `BotDrive.delayMs` and TS as
`wasm_bot_pacing_ms`. **The server's values won** (owner decision): 3000ms with
a human watching, 300ms bots-only. The phone had been pacing at 600–1200ms
while its own comment claimed to "mirror the server" — it never did — so
offline bot moves are now ~3.3x slower, partly offset by bundling (a
passive-heavy 8-player round was five separate ~900ms waits and is now one
cycle). One deliberate change to the *server's* behavior rides along, landing
with the port: `BUNDLED_PASSIVE` is 0ms, where `bot_actions.ts` today skips its
delay only in bots-only games and otherwise pauses the full 3000ms for a cycle
that changed nothing on screen. Bundling exists so those cost nothing.

Three "feels" exist today (server 3000 ms with humans / 300 ms bots-only /
skip-when-silent; iOS 600–1200 ms jitter always; docs claiming they match).
`bot_drive` returns a `pacing_class` per applied action
(`NONE | BUNDLED_PASSIVE | MOVE | ROUND_TRANSITION`); the class→milliseconds
table lives in ONE place (tiny C fn or shared constants header consumed by
TS and Swift). Sleeping remains the host's job; *how long a move deserves*
becomes product policy with one home — bot "personality" tunes every
surface at once.

### 4.4 F4 — The animation plan: one emitter for online, offline, and replay

**Facts.** On the server path the animation plan already comes from C:
`wasm_events_serialize` → the evwire stream, one masked snapshot per step
(`evwire.c`; TS twin `buildEvents` for JS-object paths, byte-parity e2e'd).
The web client never derives animations — it decodes and plays. But:

- The iOS design planned a from-scratch Swift diff engine —
  `BoardDiff.swift`, "given (old GameView, new GameView) produce moves"
  (`IOS_APP_DESIGN.md` §16.B4) — because offline there is no server to send
  evwire. **It is not yet written** ("the board springs on state change").
- Replay playback re-derives per-step board states in pure client-only TS:
  `src/replay/view.ts` (461 lines — v5 retrodiction, v6 exact hands,
  complement deduction) + `animate.ts` (333 lines — synthesizes
  forward+reverse sequences shaped like the live broadcast). iOS has begun
  its own twin (`DecodedReplay.swift`; board projection is a listed §17.7
  gap).

**STATUS: F4.1 LANDED (A3, July 2026) — `BoardDiff.swift` is cancelled and was
never written.** The derivation now has exactly one home: `evwire_walk`
(`evwire.c`) turns hook snapshots + the action's logs into the event sequence
and hands each event to a **sink**. The packed evwire writer is one sink (the
web path, byte-identical — `e2e/packed_wire_parity.test.ts` still green); the
iOS bridge's JSON emitter is the other. So offline play animates from the SAME
events the website plays, and no client works out which card flew where.

- `fio_bot_drive_json` carries its cycle's `events` inline; `fio_last_events_json`
  is the apply-path companion, so a human's card flies by the same plan a bot's
  does. Swift decodes them as `GameEvent` (`Models.swift`) and `LocalGame`
  publishes `lastEvents`.
- The kernel's redaction rides along: a card dealt/drawn into someone else's
  hand arrives as `null`, so the app cannot leak an identity it never got.
- One iOS-specific care point: this file keeps ONE resident `Game` whose log is
  the whole history (the replay encoder and belief bots read it), where the wasm
  bridge clears its log per call. The emitter therefore SLICES this action's
  logs (`log_start`) instead of clearing — clearing would silently break offline
  replay codes.
- Remaining in F4: **A5**, replay steps from the kernel (`src/replay/view.ts` +
  `animate.ts`), which now has a sink to reuse.

**Consolidation (as originally specced).**

1. `fio_apply_json` / `bot_drive` gain an **events output** (a JSON emitter
   over the same evwire data the kernel already records). `LocalGame` then
   feeds the SAME event stream the web plays, and `BoardDiff.swift` is never
   written. SwiftUI renders events with `matchedGeometryEffect` exactly as
   §16.B4 planned — only the source of moves changes.
2. **Replay steps in C**: `replay.c` already replays the game to decode it;
   add a step-emitting decode (`wasm_replay_steps` / `fio_replay_steps_json`)
   returning per-event snapshots via the same hooks. `src/replay/view.ts`
   shrinks to a consumer; `animate.ts`'s synthesis becomes unnecessary on
   any client that plays kernel events; the iOS replay projection gap closes
   free.
3. Stays per-platform, correctly: rendering (React DOM measurement,
   SwiftUI springs), queue/timing UX, the web's optimistic-conflict layer.

**Timing matters:** this lands *before* iOS Milestone-B animation work, or
the Swift diff engine gets written and becomes legacy on day one.

### 4.5 F5 — v6 replay production from the kernel game

Game end on the server is TS choreography (`finalizeEndedGame`): re-deal
from the stored seed (`reconstructSeededDeal`), assemble the v6 reveal
stream (initial hands + stock in draw order, `replay/encode.ts`), verify
round-trip, fall back to v5. The phone can't reuse any of it —
`fio_replay_encode_b32` emits v5, so offline replays lose exact hidden-hand
fidelity (worse Oracle input). Proposal:
`replay_encode_v6_from_game(const Game *g, ...)` in `replay.c` — a seeded,
kernel-resident game already knows the true deck and draw order
(`REPLAY_FORMAT6_HIDDEN_STATE.md` producer note), so reveal assembly is a C
loop. Export via wasm (server finalize becomes call-verify-store) and
`fio_` (offline and iMessage FINISHED shares become v6). The extras blob
(names + timing) needs per-move timestamps only the server records —
follows later; flag, don't block.

### 4.6 F6 — Rematch/reset-to-lobby as a kernel transform

`handleContinue` (`meta_actions.ts:188-232`) hand-zeroes ten `Game` fields;
`clientReconcile.ts:10-40` mirrors the list ("must match byte-for-byte or
the UI snaps"); iOS online rematch (M-D5) is specced to port it a third
time. That is a state transition — kernel property. Add
`wasm_reset_to_lobby` / `fio_reset_to_lobby` producing the post-reset blob;
all three mirrors become decode-and-render.

### 4.7 F7 — Web wire decode (TS mirrors of C codecs)

`view.ts` (358) + `evwire.ts` (253) + `awire.ts` (170) + `logwire.ts` (181)
≈ 960 lines of pure TS shadowing C structs byte-for-byte, kept honest by
parity e2e. iOS already decodes in C. Fold into the client wasm
**opportunistically** — the next time a wire format changes, move that
format's client decode into wasm instead of updating the mirror (F4.2's
replay-steps work is a natural first domino). ⚠ Budget: guards.wasm is
deliberately pinned at ONE wasm page (`RULES_GUARDS_WASM_MEMORY_PLAN.md`);
event decode + JSON emit will not fit — a second tiny module or a
deliberate, documented budget bump, decided with the memory-plan
discipline, not by accident.

### 4.8 F8 — Retire the TS rules projections (cleanup, after F2)

`common_utils.ts` keeps four kernel-mirrored projections for synchronous
client use — parity-tested but still a second implementation of rules. The
browser ships guards.wasm anyway, iOS never had them, and once F2 moves the
bot loop (their last real consumer) into C, migrate callers to kernel calls
and delete — or, where a synchronous JS answer is genuinely needed before
wasm warms, keep but demote to documented render-only hints. Zero new C;
deletion work.

### 4.9 F9 — Small shared invariants & accessors (batch)

- `#define FOOLISH_SEED_LEN 32` beside `game_set_deal_seed_bytes` +
  encode/decode-side rejects (a short seed silently degrades to the legacy
  32-bit LCG — catastrophic for iMessage where both devices must reproduce
  the wide deal; the handoff already requires the reject).
- `fio/wasm_unambiguous_cover`: the one-tap-cover affordance
  (`coverCombinations.ts findUnambiguousCover`, 95 lines) is needed by web
  drag, phone tap-commit, watch chooser, iMessage. Pure set logic over
  legality — belongs beside `legal.c`.
- **FMSG `msg_wire.c` with Rule P + rebase in C from day one**
  (`IMESSAGE_IMPLEMENTATION_HANDOFF.md` M0): concurrency rules are game
  rules; e2e drives them through wasm, XCTest through libfoolish — never a
  TS/Swift reimplementation.
- Display-name tidies found in passing: iOS renders raw `%` nicknames
  online (fixed by `BotNames.swift`, see the naming doc); the web live
  board does too (`PlayerRing.tsx:184`) — cheap web tidy.

## 5. Explicit non-goals (stays per-platform, on purpose)

Lease/CAS/version fence and broadcast fan-out (Postgres semantics); Supabase
CPU-budget prediction and isolate wall clock; iOS thermal downgrade and
haptics; timers/sleeps (I/O); Elo and leaderboards (DB); auth; the web's
optimistic overlay (web UX today; if
iOS Stage C2 ever ports it, revisit via the event-sourced Core idea in
`ARCHITECTURE_AS_A_PATTERN.md` rather than transliterating); all rendering.
**The kernel decides what happens and what it's worth pausing for; hosts
decide how to wait, persist, and draw.**

## 6. Action plan (ordered; each step uses the standing migration playbook —
mirror → parity harness → cutover → freeze the old path as test oracle)

| # | Action | When | Verification |
|---|---|---|---|
| A1 | **F1 roster table** — smallest, fixes §3.1 immediately, F2 depends on it — **DONE** (§4.1; server-side cutover = deleting the TS `env` blocks, rides with the bots.wasm rebuild) | now | **done**: `e2e/bot_roster_parity.test.ts` (roster ≡ registry knob-for-knob; seed.sql ≡ the `seeded` set; `_max` stays dead) + roster/knob tests in `cnitro/tests/tests.c`; ios-smoke + difftests green |
| A2 | **F2 `bot_drive` + F3 pacing classes** — **kernel + iOS DONE** (§4.2/§4.3): the cycle, the pacing table, `fio_bot_drive_json`, `wasm_bot_drive`, and `LocalGame.runBots` collapsed onto one call. **REMAINS:** port `bot_actions.ts` onto `wasm_bot_drive` behind the differential harness (the lease/CAS/broadcast/CPU budget stay TS-side; the cached-move replay after a CAS conflict stays too) | with A1 | **done**: fairness/bundling/determinism + pacing tests in `cnitro/tests/tests.c` (the fairness one fails if the shuffle is removed), ios-smoke, Swift `EngineGoldenTests`, bot-parity + determinism e2e unchanged against the rebuilt bots.wasm. **Remains**: seeded games, TS cycle vs kernel cycle, byte-compare committed products |
| A3 | **F4.1 kernel events for local play** — **DONE** (§4.4): `evwire_walk` + sinks; `fio_bot_drive_json` events inline + `fio_last_events_json`; `GameEvent`/`lastEvents` in Swift; `IOS_APP_DESIGN.md` §16.B4 amended to "consume kernel events" and `BoardDiff.swift` cancelled | before iOS Milestone-B animation work | **done**: kernel events verified for bot cycles AND human moves through the real bridge; evwire byte-parity with the TS twin unchanged (`packed_wire_parity`); Swift decodes them; C suites + `ios-smoke` + Swift tests green. **Remains**: rendering them (Milestone B) |
| A4 | **F5 v6-from-game** | after A1–A3 | extend `replay_v6_test.c`; server finalize diff-tested against the TS assembly on real finished games |
| A5 | **F4.2 replay steps from the kernel** | after A3 (reuses emitter), before native replay polish | web replay renders identically (snapshot tests); iOS plays a web-generated code step-for-step |
| A6 | **F6 reset transform**, then **F8 projection deletions** | cleanup wave | `resetToLobby` mirrors deleted; rematch e2e green on web + iOS |
| A7 | **F9 batch**: seed-len header + rejects (rides the iMessage M0), `unambiguous_cover` (with the first surface that needs it), `%`-name tidies (with the naming work), and the **`console`+`gpt` drop** — delete the `CONSOLE` key (`types.ts:254`), the gpt lazy-load path + `strategies/gpt_strategy.ts`, and, once unreferenced, `move_stats.ts` + `pass_prob.ts` (neither strategy was ever seeded in production; `registerBotStrategy` itself STAYS — the offlinefun research harnesses use it) | opportunistic | per-item; grep proves no `console`/`gpt` strategy reference survives |
| A8 | **F7 wire-decode moves**, format-by-format on next wire change; module/budget decision documented | opportunistic | mirror file deleted per format; parity test flips to wasm-vs-fixture |

**What this buys, concretely:** the offline app gets site-identical bots
(strength AND feel) with zero new game logic in Swift; the fairness and
pacing divergences die; offline/iMessage replays become Oracle-exact v6;
the watch inherits everything (it was already snapshot-driven by design);
the iMessage extension's hardest logic is CI-testable without a Mac;
Telegram/Steam become "the same three calls"; and the web sheds up to
~1,800 lines of parity-maintained TS over time (960 wire + ~800 replay).
"The bots feel the same everywhere" becomes a compile-time property instead
of a hope.
