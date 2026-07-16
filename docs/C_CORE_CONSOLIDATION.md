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
| F2 | **The bot drive cycle** (eligibility → fair pick → apply → passive bundling → stop conditions) | TS (`bot_actions.ts:262-411`) · Swift+C first-eligible walk (`LocalGame.runBots`) — with a second live divergence: iOS picks first-eligible, not shuffled | **DONE (§4.2)**: `bot_drive` is the one cycle on the kernel, iOS AND the server; both hosts are one call. The differential harness found three kernel bugs on the way — one of them live in the iOS path. |
| F3 | **Pacing policy** (what a move is worth pausing for) | TS constants 3000/300 ms + silent-skip · Swift 600–1200 ms jitter, no bundling | **DONE (§4.3)**: `bot_pacing_ms` is the one class→ms table; the server's values won. |
| F4 | **The animation plan** (which card flies where, in what order) | C (`evwire.c`, server-only today) · TS decode mirrors · **planned Swift re-derivation `BoardDiff.swift` (unwritten)** · TS replay twin (`src/replay/view.ts`+`animate.ts`, ~800 lines) | **F4.1 DONE (§4.4)**: `evwire_walk` + sinks; local play consumes kernel events and BoardDiff was never born. **F4.2 / A5: kernel half DONE (§4.6)** — a v6 replay is the game REBUILT (`start_game_with_deck`) and replayed through the engine, so its events come from the same `evwire_walk`; the specced "step-emitting decode via the same hooks" was impossible (`replay.c` replays an `RModel`, not a `Game` — no hooks fire). v5 refused: it hides the deal. Remaining: the web consumer. |
| F5 | **v6 replay production** (reveal-stream assembly at game end) | TS choreography (`finalizeEndedGame` + `reconstructSeededDeal` + `replay/encode.ts`) · absent on iOS (offline shares are v5-only) | **DONE (§4.5)**: `replay_encode_v6_from_game` is the one producer on both hosts; finalize is call-verify-store and offline shares carry exact hands. The specced `from_game` signature could not work as written (a finished game does not know its own deal), so the seed is a parameter and the kernel re-deals. |
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

**STATUS: DONE (A2, July 2026) — kernel, iOS AND the server.**
`cnitro/src/bot_drive.{h,c}` is the cycle; `fio_bot_drive_json` and
`wasm_bot_drive` expose it; `LocalGame.runBots` and `bot_actions.ts` are each
ONE call per cycle. The fairness and bundling divergences (§3.2) are gone on
both hosts, and `bot_actions.ts:262-411` — eligibility, the shuffle, the roster
dispatch, the apply, the bundling, the stop conditions, the pacing and the
legality re-check on the CAS-retry path — is deleted, not mirrored. The server
keeps exactly what §5 says it should: lease, CAS, broadcast, CPU budget, and
the two I/O reads (the session log, the deal seed).

**What the differential harness found** (`e2e/bot_drive_parity.test.ts`, the
step this port was gated behind — seeded games, TS cycle vs kernel cycle,
byte-comparing committed products). It paid for itself three times:

1. **A strategy's search was leaking into the animation plan.** The
   Monte-Carlo bots deliberate by running real `handle_*` calls over scratch
   games, and `engine_snap_hook` is global — so a cycle that left it installed
   across the choose built the host's event stream out of a rollout's IMAGINARY
   cards, and saturated `MAX_SNAPS` before the real move landed (a 1-action
   cycle reported 12 events). One-move-per-call hosts never saw it: they reset
   the snapshot buffer when they open the apply, which is AFTER the choose. A
   cycle does both in one call, so `bot_drive` now brackets the choose
   explicitly. **This was live in `fio_bot_drive_json` too** — A3's iOS events
   were being built from blackpowder's imagination — so the fix is in
   `bot_drive.c`, where both hosts get it. Guarded by
   `test_bot_drive_choose_emits_no_snapshots` (negative-tested).
2. **Seeding is per DECISION, and has two phases.** The RNG is re-seeded from
   `state_fnv` before every choose (strategy LCG) and every apply (draw LCG);
   a cycle drives several seats per call, so `bot_drive_pre_action_hook` fires
   at both phases per seat. Seeding once per cycle shifts the stream for every
   seat after a stream-CONSUMING bot (`random`, `handwritten_prod`), and
   seeding the draw LCG before the choose feeds the search a value the
   single-move path never gave it — the Monte-Carlo bots SAMPLE from whatever
   the last apply left (`robusta_strategy.c` calls `game_random`;
   blackpowder/cordite/octogen save and restore it). Both changed a real bot
   move in the harness. Guarded by `test_bot_drive_pre_action_hook`
   (negative-tested).
3. **The belief log must be sliced, not exported from zero.** A cycle whose
   bots read the session log has that whole log resident BENEATH the records it
   writes, so `wasm_export_logs_masked_from` / `wasm_events_serialize_from` take
   the offset `wasm_bot_drive_log_start` reports. Exporting from zero would have
   appended the entire session to `logs_packed` a second time, every cycle.

**The one behavioral change in the choose path** (deliberate, and the only one
the harness could not make disappear): inside a bundle, a belief bot now sees
the records of the bundle's earlier actions. The server hydrates once per cycle,
so a bot acting second could not see the `good` the first bot just said; the
kernel keeps the log resident, so it can. That is strictly more information, and
only a public fact the bot would have seen one cycle later anyway. With that
held equal the products match to the byte — which is the proof that it is the
ONLY difference. (A blackpowder seat covered where the TS cycle had it pick up.)

Decisions taken while building it:

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

4. **Knobs come from the roster on the drive path, not from the TS env table.**
   `wasmBotDrive` clears the kernel env before driving, so `bot_roster_choose`'s
   per-seat knob spec is authoritative — env beats roster by design
   (`bot_knobs.h`), which is right for a research override and wrong for a table
   a previous decision happened to leave installed. The values are identical
   either way (`e2e/bot_roster_parity.test.ts` pins them knob-for-knob), so this
   is the A1 server-side cutover for this path, proven by the byte-compare.
5. **The CPU predictor's unit is now the CYCLE**, which is also the unit the
   loop bails in. One drive can deliberate for several seats, so predicting the
   next iteration from per-move costs would under-estimate it and risk the ~2s
   cap.

The server loop's INNER CYCLE (`bot_actions.ts:262-411`) was game logic in a
TS coat: find eligible bot seats, pick one **shuffled** (fairness), choose
via roster, apply, **bundle zero-event passives**, stop when a human becomes
eligible / game ends / an event-bearing move lands. Proposal (as specced, now
built):

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

**STATUS: LANDED (A2), both hosts.** `bot_pacing_ms(class, humans_present)` in
`bot_drive.c` is the one table, reaching Swift as `BotDrive.delayMs` and TS as
`wasm_bot_pacing_ms` — `bot_actions.ts` holds no timing constants at all now.
**The server's values won** (owner decision): 3000ms with a human watching,
300ms bots-only. The phone had been pacing at 600–1200ms while its own comment
claimed to "mirror the server" — it never did — so offline bot moves are now
~3.3x slower, partly offset by bundling (a passive-heavy 8-player round was five
separate ~900ms waits and is now one cycle). One deliberate change to the
*server's* behavior landed with the port: `BUNDLED_PASSIVE` is 0ms, where
`bot_actions.ts` used to skip its delay only in bots-only games and otherwise
pause the full 3000ms for a cycle that changed nothing on screen. Bundling
exists so those cost nothing.

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
- **F4.2 / A5 — kernel half DONE (§4.6).** Remaining: the web consumer
  (`src/replay/view.ts` + `animate.ts`).

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

**STATUS: DONE (A4, July 2026) — kernel, the server AND iOS.**
`replay_encode_v6_from_game` (`replay.c`) is the one v6 producer;
`wasm_replay_encode_v6_from_game` and `fio_replay_encode_v6_b32` expose it.
`finalizeEndedGame` is call-verify-store, offline shares carry exact hands, and
`reconstructSeededDeal` + `encodeReplayV6`'s reveal assembly + `marshalInputV6`
+ `deriveTrump` have **no production caller left** — they are frozen as the
differential oracle (`e2e/replay_v6_parity.test.ts`), per the playbook, not
deleted.

**The specced signature could not work as written.** `replay_encode_v6_from_game(
const Game *g, ...)` rests on the producer note's "a seeded, kernel-resident
game already knows the true deck and draw order". Half of that is false: a
FINISHED game knows neither. `deal_initial` emits no per-seat `LOG_DRAW` (only a
hook snapshot), and `draw_card` splices each drawn card out of `deck[]`, so at
game end `deck_count` is 0 and the deal is simply gone from the struct. The
alternative — capturing the 52-card deal permutation in the `Game` and carrying
it in the durable blob — was rejected (owner decision): it costs ~52 B on every
state write, for drift-immunity that buys nothing when finalize runs seconds
after the deal, on the binary that dealt it. So the **seed is a parameter**, and
the kernel re-deals a scratch game internally. From `g` come only the actions
(its logs, read directly) and the player count.

Decisions and traps, in the order they bit:

1. **A re-deal is a deal: it fires the snapshot hook and moves the deal RNG.**
   `start_game` fires `ENGINE_HOOK_DEAL` per seat and `engine_snap_hook` is
   global — so a re-deal on a host building an animation plan would splice a
   whole IMAGINARY deal into it. This is A2's bug #1 in a new place, and it is
   bracketed the same way (`bot_drive.c choose_move`). It also consumes the
   ChaCha stream and leaves wide mode SET, which `draw_index` reads for EVERY
   game in the thread — so a warm isolate's next game would pop a pre-shuffled
   deck instead of drawing at random. Both globals are saved and restored
   (`game_deal_rng_get/set`), both negative-tested.
2. **The trump is a witness, but only while `has_flipped`.** A wrong seed is
   otherwise silent corruption — the reveals would describe a different deal and
   the encode could still succeed, storing a replay of a game nobody played. The
   game's own `flipped` is the cheap witness, and the producer note's "snapshot
   the trump at deal time" is right for the SERVER but not for the kernel: this
   kernel keeps the card and only clears the flag, while a game round-tripped
   through the state blob comes back with `flipped` = **wire card 0** (`engine.ts
   marshalGame` sends `game.flipped ? wireStateCard(...) : 0`), which reads as a
   real 6, not a sentinel. Indistinguishable here — so the check trusts the flag,
   not the card, and a dry-stock game is caught downstream instead
   (`REPLAY_ENOTINMENU`: the logged opening attack is not in the wrong deal's
   menu). **The harness found this**: every server-path encode failed
   `REPLAY_EHEADER` on the first run.
3. **No action is marshalled any more.** `Src` (the encoder's action reader)
   gained a LOGS mode beside its BYTES mode, so the coder, the model and
   `run_replay_v6` never learn where an action came from, and no multi-KB input
   buffer exists on this path at all. The `GOOD`+`DISCARD` → `round_end`
   synthesis rule now has a kernel copy (`log_atom_kind`) which is the one BOTH
   hosts' v6 runs through — but honestly: it did not remove the others. The v5
   producers still carry theirs (`ios_api.c build_encode_input`, `encode.ts
   collectActions`), as do the test oracles. Those collapse when v5 production
   does, which is not A4.
4. **The scratch deal is a short-log slot, not a `Game`.** A `Game` is ~130 KB
   in the production build (`MAX_LOGS` x `MAX_LOG_PAIRS`); the re-deal needs its
   hands, deck and flip and never its logs. Same trick `cordite_sim.c` plays for
   sampled worlds (`WORLD_SLOT_BYTES`): the slot stops just past the start of
   `logs`, and `log_cap = 1` routes every non-DISCARD append to `log_alloc`'s own
   sink. ~2 KB instead of ~130 KB.
5. **It is a bots.wasm export** (owner decision: "use the big wasm everywhere;
   split back later"). This needs a whole session log resident and rules.wasm is
   built at `MAX_LOGS=128` with no log import — not a knob, since the `Game`
   struct alone would go 33 KB → 133 KB and blow that module's pinned 3-page
   memory. bots.wasm is a superset and adopts the engine slot, so a bot game pays
   nothing; a human-only game's finalize instantiates it. Cost: **+711 B gzip**
   (55,728 → 56,439). The follow-up — retiring rules.wasm so there is ONE module
   — is unblocked (`bots()` is already synchronous and already adopts) but is its
   own change, not A4's.
6. **`FOOLISH_SEED_LEN` landed here** (A7's first item, opportunistically): this
   work needed the constant, and a short seed silently degrading a deal to the
   legacy LCG is exactly the trap A7 names.
7. **Format choice is in C, not in a client.** `fio_replay_share_code_b32`
   returns the best code a game can produce (v6 when its deal is re-derivable,
   else v5) — the same fallback the server makes. Swift asks for "a shareable
   code" and never learns v6 exists, so the watch and the iMessage extension
   inherit it instead of reimplementing it.

The extras blob (names + timing) still needs per-move timestamps only the server
records, and stays TS-side — as flagged, not blocking. The v5 fallback stays for
legacy/seedless games.

**One accepted regression, measured.** The kernel path holds the session log in
`Game.logs[MAX_LOGS]`, so a game with **more than 512 log records cannot be v6**
and falls back to v5 (logged, never silent). The old TS path had no such limit —
it marshalled actions into a byte buffer and never stored a log. Truncating is
not an option instead: a short log is a short ACTION stream, and v6 would happily
encode it as a legal mid-game cut — a silently half-recorded game, which is worse
than v5. Measured over 420 seeded games per config (2..8 players):

| config | over the cap | longest log |
|---|---|---|
| all handwritten | 0% | 369 |
| one `random` seat, rest handwritten | 0% | 413 |
| all `random` (bot-only arena) | 29% | 512 (hit) |

So no human game reaches it — but headroom is only ~20%, and bot-only arena games
on the thrashiest rung (`random` IS a seeded rung) do lose exact hands. Two ways
out when it matters, neither taken here: raise bots.wasm's `MAX_LOGS` (~+200 KB
of linear memory: `g_game` plus the log-export buffer), or give `Src` a third
mode that STREAMS the action stream from the caller's log-wire bytes with no
storage at all — which removes the ceiling entirely and is the same shape as the
LOGS mode. The second is the better answer if this ever bites.

### 4.6 F4.2 / A5 — replay steps: the game rebuilt, not steps re-derived

**The spec's premise was wrong, and the correction is the design.** This file
said (§4.4 Consolidation 2) *"`replay.c` already replays the game to decode it;
add a step-emitting decode returning per-event snapshots via the same hooks."*
It does not replay a game. It replays an **`RModel`** — a belief/bitmask model
(`unseen`, `known[]`, `unknown[]`, `deck_count`) with its own `apply_*` mirrors
of the server's `execute*`. `engine_snap_hook` never fires there, and
`evwire_walk` needs real `Game` snapshots. There were no hooks to reuse.

So A5 does not emit steps beside the decoder. It **rebuilds the game and plays
it**: v6 is hidden-state-lossless, and its exact opening hands plus its stock
draws in pop order ARE a deck. `start_game_with_deck` (`game.c`) feeds that deck
through the identical path `start_game` runs — same deal, same flip, same seats,
same hooks — so `replay_steps_v6` (`replay_steps.c`) gets its events from
`evwire_walk` over real engine hooks. The one derivation, unchanged. A replay is
not a second kind of game, so there is no replay-side projection left to drift.

Two things the decoder had to start saying out loud:

- **The atoms** (`replay_decode_atoms_v6`). `replay_decode`'s output is a *log*
  stream, and a round end is genuinely not recoverable from it: `LOG_DISCARD`
  does not mean round-end (a clean-sweep cover discards too — `apply_cover`),
  and neither does `LOG_GOOD` (a round whose attackers are all out logs none).
  The decoder knows each atom's kind for certain, so it reports it.
- **The header as fields** (`ReplayHeader`), so the atom path needs no log
  buffer. Decode's callers really pass 2 MB; that is not something to hand a
  phone or spend a wasm page budget on.

**v5 is refused, not supported** (`REPLAY_EVERSION`). v5 hides the deal, so its
hands are *retrodiction* — a known / unknown-slot / never-surfaces tri-state
that a `Game`'s concrete `Card hand[]` cannot hold. Dead format (owner call,
July 2026); nothing produces v5 codes any more.

Verified as invariants in C (`cnitro/tests/tests.c`), both mutation-checked:

- the rebuilt game's final board is **byte-identical** (unmasked, so hands are
  compared) to the board the engine actually finished on, np=2..6, and it finds
  the same fool the code claims. Deleting the trump-skip in the deck rebuild
  fails it.
- a **mid-game cut conserves the deck**. This one earns its keep: a finished
  game has drained the stock, so a deck missing its never-drawn tail still ends
  at `deck_count` 0 and looks right — the first version of this test passed with
  that bug in place. Cut the stream early and the stock is still on the table,
  where a missing tail is simply a wrong number on screen.

`ios-smoke` drives `fio_replay_events_json` over the whole 48-game v6 sweep.

**Remaining (the web consumer).** `src/replay/view.ts` (461 lines) +
`animate.ts` (333) still build steps in TS. Note for whoever takes it: a replay
canNOT be one packed evwire frame — `evwire_serialize` backpatches `n_events` as
a **u8** (255 max) and a whole game exceeds it. Either frame per action, or take
JSON as iOS does. Also note `view.ts`'s `slots` retrodiction has a second
consumer, `src/oracle/replayOracleInput.ts:72` (`nullSlots`), which is v5-shaped
and should die with v5 rather than be ported.

### 4.7 F6 — Rematch/reset-to-lobby as a kernel transform

`handleContinue` (`meta_actions.ts:188-232`) hand-zeroes ten `Game` fields;
`clientReconcile.ts:10-40` mirrors the list ("must match byte-for-byte or
the UI snaps"); iOS online rematch (M-D5) is specced to port it a third
time. That is a state transition — kernel property. Add
`wasm_reset_to_lobby` / `fio_reset_to_lobby` producing the post-reset blob;
all three mirrors become decode-and-render.

### 4.8 F7 — Web wire decode (TS mirrors of C codecs)

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

### 4.9 F8 — Retire the TS rules projections (cleanup, after F2)

`common_utils.ts` keeps four kernel-mirrored projections for synchronous
client use — parity-tested but still a second implementation of rules. The
browser ships guards.wasm anyway, iOS never had them, and once F2 moves the
bot loop (their last real consumer) into C, migrate callers to kernel calls
and delete — or, where a synchronous JS answer is genuinely needed before
wasm warms, keep but demote to documented render-only hints. Zero new C;
deletion work.

### 4.10 F9 — Small shared invariants & accessors (batch)

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
- **The `console` + `gpt` drop — DONE (A7).** `STRATEGY_KEY.CONSOLE`/`.GPT`,
  `strategies/gpt_strategy.ts`, and the lazy-load path in `bot_strategy.ts` are
  gone. Two live consumers went with them, both of which existed only to keep
  gpt out of everyone's way: the one-user gate in `handleAddBot`
  (`GPT_ALLOWED_USER_ID`, "to control API costs") and `Lobby.tsx`'s filter that
  hid gpt bots from the picker. Safe because gpt was never seeded — no `gpt` row
  in `seed.sql` or any migration — so no roster row is now exposed by dropping
  the filter.
  **`registerBotStrategy` STAYS** (offlinefun harnesses), and so do
  `move_stats.ts` + `pass_prob.ts`: F9 gates their deletion on being
  unreferenced and they are NOT —
  `offlinefun/localtest/console_strategy.ts` still imports `move_stats`, which
  imports `pass_prob`. That file is a research harness, imported by nothing and
  not using `STRATEGY_KEY`, so the production drop does not touch it. Delete
  those two if and when that harness goes.
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
| A1 | **F1 roster table** — smallest, fixes §3.1 immediately, F2 depends on it — **DONE** (§4.1; server-side cutover = deleting the TS `env` blocks, rides with the bots.wasm rebuild). **Cutover status (July 2026):** the LINK half is already done — `bot_roster.c` has been in `WASM_BOT_SRC` since A2 (bot_drive dispatches through it; the module does not link without it), and it did NOT drag the two offline-only brains in as feared. What remains is folding `wasm_choose_move`'s switch onto the roster + deleting the TS `env` blocks. **The wasm rebuild is NOT a blocker and there is no toolchain skew** — re-verified July 2026: from a CLEAN tree, `make wasm-bots WASM_CC=/opt/homebrew/opt/llvm/bin/clang` (Homebrew LLVM 22.1.8 + wasm-opt 130) reproduces the committed `bots.wasm.gz` md5-identically, so a byte delta means your change. (The phantom keeps coming back because `WASM_CC` defaults to plain `clang` = APPLE clang, which cannot target wasm32 at all — it is `WASM_CC=`, not `CC=`. Confirmed again here: an apparent "670-byte skew" was this session's own edits to `replay.c`/`game.c`, which are in `WASM_BOT_SRC`.) ⚠ Note for the cutover: A5's atom sink grows bots.wasm ~670 B even though `replay_steps.c` is not linked into it — check that against the memory plan when the rebuild lands. | now | **done**: `e2e/bot_roster_parity.test.ts` (roster ≡ registry knob-for-knob; seed.sql ≡ the `seeded` set; `_max` stays dead) + roster/knob tests in `cnitro/tests/tests.c`; ios-smoke + difftests green |
| A2 | **F2 `bot_drive` + F3 pacing classes** — **DONE** (§4.2/§4.3): the cycle, the pacing table, `fio_bot_drive_json`, `wasm_bot_drive`, `LocalGame.runBots` **and `bot_actions.ts`** are one call per cycle. Lease/CAS/broadcast/CPU budget stayed TS-side; the cached-move replay became `BotDrivePref` (the kernel re-checks legality) | with A1 | **done**: fairness/bundling/determinism + pacing + snapshot + seeding-hook tests in `cnitro/tests/tests.c` (fairness, snapshot and hook ones are negative-tested), ios-smoke, Swift tests, difftests, and **`e2e/bot_drive_parity.test.ts`** — seeded games, TS cycle vs kernel cycle, byte-comparing committed products (state blob, log records, per-viewer event streams) across 5 configs incl. belief bots and the pref path |
| A3 | **F4.1 kernel events for local play** — **DONE** (§4.4): `evwire_walk` + sinks; `fio_bot_drive_json` events inline + `fio_last_events_json`; `GameEvent`/`lastEvents` in Swift; `IOS_APP_DESIGN.md` §16.B4 amended to "consume kernel events" and `BoardDiff.swift` cancelled | before iOS Milestone-B animation work | **done**: kernel events verified for bot cycles AND human moves through the real bridge; evwire byte-parity with the TS twin unchanged (`packed_wire_parity`); Swift decodes them; C suites + `ios-smoke` + Swift tests green. **Amended July 2026**: each event now also carries `state` — the viewer-masked board as of that step, the counterpart of the web evwire's per-event `snap_len` payload — because a `bot_drive` cycle applies several actions and the intermediate boards were otherwise unreachable without the diff engine F4 cancelled (`j_state` in `ios_api.c`; `GameEvent.state` in Swift; asserted by `ios-smoke`, which previously had NO coverage of the events path at all). **Remains**: rendering them (Milestone B) |
| A4 | **F5 v6-from-game** — **DONE** (§4.5): `replay_encode_v6_from_game` + `wasm_replay_encode_v6_from_game` + `fio_replay_encode_v6_b32` / `fio_replay_share_code_b32`; `finalizeEndedGame` is one call; `reconstructSeededDeal` + `encodeReplayV6` + `marshalInputV6` keep NO production caller and are frozen as the oracle | after A1–A3 | **done**: `replay_v6_test.c` extended with the seeded from-game path — byte-equal to the marshalled oracle, mid-game cuts, wrong-seed rejection, and the hook/deal-RNG restores (all negative-tested); `ios-smoke` proves 48/48 seeded games share as v6 with no hidden card surviving; and **`e2e/replay_v6_parity.test.ts`** byte-compares the kernel against the TS choreography on real seeded games, through BOTH the JS-log and packed-`logs_packed` paths. It found the trump-witness bug (§4.5.2) on its first run |
| A5 | **F4.2 replay steps from the kernel** — **KERNEL HALF DONE (§4.6)**: `replay_steps_v6` rebuilds the game from a v6 code (`start_game_with_deck`) and replays it through the real engine, so the events are `evwire_walk`'s, not a replay-side projection; `replay_decode_atoms_v6` + `ReplayHeader` are what the decoder had to say out loud (a round end is NOT recoverable from the log stream). `fio_replay_events_json` gives the phone the live event stream for a shared code. **v5 refused** — it hides the deal (owner: v5 is dead). | after A3 (reuses emitter), before native replay polish | **done (kernel)**: C invariants, both mutation-checked — rebuilt final board byte-identical to the played game's (np=2..6, unmasked) and a mid-game cut conserves the deck (this one caught a real missing-deck-tail bug that the finished-game test could not see); `ios-smoke` replays all 48 sweep games as live events, no hand leaked to a spectator. **Remaining**: the web consumer (`view.ts`+`animate.ts`) — note `evwire_serialize`'s `n_events` is a u8 (255 max), so a whole game is NOT one frame |
| A6 | **F6 reset transform**, then **F8 projection deletions** | cleanup wave | `resetToLobby` mirrors deleted; rematch e2e green on web + iOS |
| A7 | **F9 batch**: seed-len header + rejects — **`FOOLISH_SEED_LEN` DONE** (A4 needed it; `game.h`, and `game_set_deal_seed_bytes` rejects against it — the encode/decode-side rejects still ride the iMessage M0) —, `unambiguous_cover` (with the first surface that needs it), `%`-name tidies (with the naming work), and the **`console`+`gpt` drop** — delete the `CONSOLE` key (`types.ts:254`), the gpt lazy-load path + `strategies/gpt_strategy.ts`, and, once unreferenced, `move_stats.ts` + `pass_prob.ts` (neither strategy was ever seeded in production; `registerBotStrategy` itself STAYS — the offlinefun research harnesses use it) | opportunistic | per-item; grep proves no `console`/`gpt` strategy reference survives |
| A8 | **F7 wire-decode moves**, format-by-format on next wire change; module/budget decision documented | opportunistic | mirror file deleted per format; parity test flips to wasm-vs-fixture |
| A9 | **Retire the TS parity twins for C invariants** (owner steer, July 2026) — once a surface is single-sourced in C, a TS re-implementation kept "byte-identical" by a parity test is no longer proving the port, it is *pinning the format in place*: it makes every kernel change a two-repo edit and it can only ever say "the copy agrees", never "the answer is right". Test INVARIANTS in C instead. **Partly done**: `e2e/replay_ts_oracle.ts` (1,078 lines) is deleted and `replay_codec.test.ts` now asserts the decode reproduces the game the ENGINE played — strictly stronger than agreeing with a second codec. **Remaining twins**: `e2e/replay_v6_parity.test.ts`'s frozen v6 choreography (A4 left it deliberately), the evwire `packed_wire_parity` twin, and `bot_drive_parity`'s TS cycle. Each dies when its C invariant exists, NOT before | after the surface is single-sourced | the twin file is deleted and a C test fails when the behaviour breaks (mutation-check it) |
| A10 | **Make the server code server-agnostic** (owner steer, July 2026) — the game logic under `supabase/functions/_shared/` is not actually Supabase-specific; Supabase is one *implementation* of a host (Postgres + Deno edge + realtime). Split it: a **common** part (the rules/bot/replay orchestration, host-neutral) and a **supabase** part that is only the adapter — DB reads/writes, the lease/CAS, realtime broadcast, `EdgeRuntime.waitUntil`. The test is whether a second backend could be added without touching the common part. **Plus a separate SDK layer**: one folder per language, each being the binding to the wasm/C kernel and nothing else (TS today; Swift's `EngineC.swift`/`FoolishKit` is already this shape and should move; Kotlin/etc. later). This is the structural counterpart to the whole C consolidation — the kernel is already host-neutral, but every host still reaches it through a bespoke bridge that lives inside a vendor's folder. **NOT started; do not start as a drive-by** — it is a large mechanical move that will conflict with everything in flight | after the A1–A9 waves settle; sequence against the iMessage work, which adds a THIRD host and is the natural forcing function | a backend adapter can be swapped with no diff in the common part; each SDK folder builds and tests standalone against the wasm |

**What this buys, concretely:** the offline app gets site-identical bots
(strength AND feel) with zero new game logic in Swift; the fairness and
pacing divergences die; offline/iMessage replays become Oracle-exact v6;
the watch inherits everything (it was already snapshot-driven by design);
the iMessage extension's hardest logic is CI-testable without a Mac;
Telegram/Steam become "the same three calls"; and the web sheds up to
~1,800 lines of parity-maintained TS over time (960 wire + ~800 replay).
"The bots feel the same everywhere" becomes a compile-time property instead
of a hope.
