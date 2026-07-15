# Kernel reuse audit — what else should move into C

*Investigation, 2026-07-15. Trigger: the offline iOS MVP needs a client-side
bot loop — a thing that has only ever existed server-side — and the project's
standing rule is "do not repeat yourself: rules live once, in C." This audit
inventories everything that sits BETWEEN the C kernel and each platform
today, finds what is already duplicated (including one live behavior
divergence), and proposes the C additions that make the server, the iOS
offline client, and every future client (watchOS, Telegram, Steam, `/m/`
web-play) thin shells around the same machine code. No code has been
changed; this is the work order.*

---

## 1. Method

Two production "hosts" run games end-to-end today:

- **Server**: edge functions → `_shared/bot_actions.ts` (the leased bot
  loop) → `pure_bot_actions.ts` → `bot_strategy.ts` → `wasm/bots.ts` →
  kernel. Commit via CAS, broadcast packed views.
- **iOS offline** (built, pre-Mac-verification): `LocalGame.swift` (drive
  loop) → `EngineC.swift` → `cnitro/ios/ios_api.c` → the same kernel,
  statically linked.

For every responsibility in the game path, classify: **[K]** already in the
kernel (done), **[D]** duplicated per-host logic that belongs in C, **[P]**
platform-inherent (correctly per-host). The [D]s are the findings.

## 2. The map

| Responsibility | Server today | iOS offline today | Class |
| --- | --- | --- | --- |
| Rules: legality, apply, deal, refill, win | kernel (`game.c`, `legal.c`) | same kernel | **[K]** |
| Per-viewer masking | kernel (`view.c`) | same (`fio_state_json`, packed-view decode) | **[K]** |
| Legal-move enumeration | kernel (`kernelLegalMoves`) | kernel (`fio_legal_moves_json`) | **[K]** |
| Bot brains | kernel (`bots.wasm`) | same sources in `libfoolish.a` | **[K]** |
| Replay codecs v5/v6 | kernel | kernel (v5 exposed; v6 not yet — §4 F4) | **[K]**/[D] |
| **Strategy roster: key → brain + tuning knobs + logs flag** | TS registry (`bot_strategy.ts:63-99`) | C table in `ios_api.c:37-48`, **no knobs** | **[D] F1** |
| **Bot-loop cycle: eligibility, selection, passive bundling** | TS (`bot_actions.ts:262-411`) | Swift + C first-eligible walk (`LocalGame.runBots`, `fio_bot_step_json`) | **[D] F2** |
| **Pacing policy** (when/how long to pause between bot moves) | TS constants 3000/300 ms + skip rule (`bot_actions.ts:33-34,450`) | Swift 600–1200 ms jitter (`LocalGame.botDelayNanos`) | **[D] F3** |
| **Game-end replay production** (v6 reveal assembly, seeded-deal reconstruction) | TS (`utils.ts finalizeEndedGame:913`, `game_lifecycle.ts reconstructSeededDeal`, `replay/encode.ts`) | absent (v5 only, no extras) | **[D] F4** |
| **Rematch / reset-to-lobby transform** | TS (`meta_actions.ts handleContinue:188` — a 10-field mutation the web client must mirror byte-for-byte, `clientReconcile.ts:10-40`) | bypassed offline (fresh game) — but online rematch will need it | **[D] F5** |
| Turn-eligibility & rules projections (`shouldBotActCore`, `canCover`, `game_done`, `get_next_player_index`) | TS mirrors in `common_utils.ts` (parity-tested, deliberately frozen) | kernel (`fio_actor_mask` etc.) | **[D] F6** |
| Belief-log hydration decision (`strategyUsesLogs`) | TS flag per registry entry | implicit (resident kernel game accrues its own log) | **[D] F1** (roster data) |
| Concurrency: lease, CAS commit, version fence | Postgres RPCs | n/a (single user) | **[P]** |
| CPU/wall budgeting | Supabase isolate limits (`bot_actions.ts:42-56`) | thermal guard (`applyThermalPolicy`) | **[P]** |
| Timers/sleeps, haptics, broadcast, DB, Elo, auth, LLM (`gpt`) adapter | server/TS | Swift/UI | **[P]** |

## 3. The motivating bug (found by this audit)

**Offline cordite is not the website's cordite.** The C strategies read
their tuning through env: `CD_BUDGET` defaults to **0 = arena mode**
(`cordite_strategy.c:90`), and the server sets `CD_BUDGET=prod|max`,
`CD_RACE=1`, `CD_RACE_C=75` per roster entry via the wasm env table
(`bot_strategy.ts:84-98`, `wasm/bots.ts setEnv`). The iOS dispatcher
(`ios_api.c dispatch_choose`) sets **no env at all**, so on the phone
cordite runs at the arena budget with early-race off — different strength,
different latency — and `octogen` matches the site only because
`OG_TRUMP_KEEP`'s C default (40, `octogen_strategy.c:1559`) happens to equal
the server's override today; the next server-side knob change silently forks
the phone. This is exactly the drift class the one-kernel rule exists to
prevent, and it is why the roster must be C data, not three tables.

## 4. Findings & proposals (ranked)

### F1 — One canonical bot roster table, in C

New `cnitro/src/bot_roster.{h,c}`:

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
in `bot_strategy.ts` shrinks to `key → wasm call` plus the `gpt` I/O
exception), `ios_api.c` (replaces `ROSTER[]` + `dispatch_choose`, fixing §3),
and a new tiny wasm export so e2e can assert `supabase/seed.sql` seeds
exactly the `shipped` set (today that invariant lives in a SQL comment).
`uses_logs` rides along so every host hydrates belief bots identically.
Knob application becomes `bot_roster_choose` setting the values directly
(struct-driven, not getenv) with the env vars kept as research overrides.

### F2 — The bot drive cycle becomes a kernel entry point

The server loop's INNER CYCLE (`bot_actions.ts:262-411`) is game logic
wearing a TS coat: find eligible bot seats (`shouldBotActCore` + legal
check), pick one **shuffled** (liveness/fairness), choose via roster, apply
through the kernel, **bundle zero-event passives** (multiple silent "good"s
coalesce so play feels snappy), stop when a human becomes eligible / the
game ends / an event-bearing move lands. The iOS loop re-implements a
worse version: `fio_bot_step_json` walks seats **first-eligible** (not
shuffled — 8-player fairness differs from the site) and `LocalGame` pays the
full UX delay for every silent "good" (no bundling — an 8-player round with
five passives feels padded on the phone today).

Proposal: `bot_drive` in C (wasm export + `fio_bot_drive_json`):

```c
// Drive bot seats until a stop condition. Applies 0..n actions.
// stop: human seat eligible | game over | an event-bearing action applied
//       | max_actions reached. Passive zero-event actions bundle (never stop).
// Selection among simultaneously-eligible bots: seeded shuffle (game RNG) —
// identical fairness on every host, reproducible in replays/tests.
int bot_drive(Game *g, uint32_t human_mask, int max_actions, BotDriveOut *out);
// out: n actions applied (seat+awire each), events?, ended, paused_reason,
//      suggested_pacing_class (see F3)
```

Server keeps: lease, CAS commit of the returned products, broadcast, CPU
budget (`max_actions`/repeat-call maps cleanly onto its measured-cost
bailout). iOS keeps: timers, thermal downgrade (a roster-tier override
between calls), rendering. Telegram/Steam later: same call. The existing
differential-parity method applies: replay N seeded games through the old
TS loop and `bot_drive`, byte-compare committed state/log products.

### F3 — Pacing policy as shared data, not per-host constants

Three feels exist today: 3000 ms with humans / 300 ms bots-only / skip when
silent (server), 600–1200 ms jitter always (iOS), and the docs claim they
match (`IOS_APP_DESIGN.md` §16.B2 cites the server's "deliberate pacing").
Make the kernel return a `pacing_class` per applied action
(`NONE | BUNDLED_PASSIVE | MOVE | ROUND_TRANSITION`) from `bot_drive`, and
put the class→milliseconds table in ONE place (a tiny C fn or a shared
constants header consumed by both TS and Swift). Sleeping remains the
host's job; *how long a move deserves* becomes product policy with one home.

### F4 — v6 replay production from the kernel game

Game end on the server is TS choreography (`finalizeEndedGame`):
re-deal from the stored seed (`reconstructSeededDeal` — TS driving kernel),
assemble the v6 reveal stream (initial hands + stock in draw order,
`replay/encode.ts`), verify round-trip, fall back to v5. The phone can't
reuse any of it — `fio_replay_encode_b32` emits v5, so offline replays lose
exact hidden-hand fidelity (worse Oracle input) and carry no extras.

Proposal: `replay_encode_v6_from_game(const Game *g, ...)` in `replay.c` —
a seeded, kernel-resident game already knows the true deck and draw order
(`docs/REPLAY_FORMAT6_HIDDEN_STATE.md` producer note), so the reveal-stream
assembly is a C loop, not a TS module. Export via wasm (server finalize
becomes: call, verify, store — DB only) and `fio_` (offline shares become
v6). The extras blob (names + move timing, `replay/extras.ts`) can follow
later; it needs per-move timestamps, which only the server's log records
carry today — flag, don't block.

### F5 — Rematch/reset-to-lobby as a kernel transform

`handleContinue` (`meta_actions.ts:188-232`) hand-zeroes ten `Game` fields;
`clientReconcile.ts:10-40` mirrors the list and its comment warns the two
must match byte-for-byte or the UI snaps; the iOS ONLINE rematch (milestone
D5) is specced to port that list a third time. That is a state transition —
kernel property. Add `wasm_reset_to_lobby` / `fio_reset_to_lobby` producing
the post-reset state blob; all three mirrors become decode-and-render.

### F6 — Retire the TS rules projections (deliberate, small)

`common_utils.ts` keeps four kernel-mirrored projections for synchronous
client use (`shouldBotActCore:147`, `canCover:79`, `game_done:182`,
`get_next_player_index:62`) — parity-tested but still a second
implementation of rules. The browser now ships guards.wasm anyway
(`src/wasm/clientGuards.ts`), and iOS never had them. Once F2 lands (the
last real consumer, the bot loop, moves into C), migrate client callers to
kernel calls and delete the mirrors — or, where a synchronous JS answer is
genuinely needed before wasm warms, keep them but demote to documented
render-only hints. Zero new C required; this is deletion work.

### F7 — Small shared invariants worth one header

- Deal-seed width (32 bytes) and the "shorter seed silently degrades"
  hazard: a `#define FOOLISH_SEED_LEN 32` next to
  `game_set_deal_seed_bytes` + encode/decode-side rejects (the iMessage
  handoff already requires this; give it a C home).
- The UX card of "bot names are keys, localized at render time"
  (`docs/IOS_BOT_NAMES.md`) pairs with F1's roster: `key` is the join
  column for display names on every platform.

## 5. Explicit non-goals (stays per-platform, on purpose)

Lease/CAS/version fence and broadcast fan-out (Postgres semantics);
Supabase CPU-budget prediction and the isolate wall clock; iOS thermal
downgrade and haptics; timers/sleeps (I/O); Elo and leaderboards (DB);
auth; the `gpt` strategy (an I/O adapter, not game logic); all rendering.
The kernel decides *what happens and what it's worth pausing for*; hosts
decide *how to wait, persist, and draw*.

## 6. Suggested order & verification

1. **F1 roster table** — smallest, fixes the §3 drift immediately, and F2
   depends on it. Verify: existing bot-parity e2e plus a new assertion that
   server-side chooses through the roster equal today's registry behavior
   knob-for-knob (fixture the env table).
2. **F2 `bot_drive`** + **F3 pacing classes** — port the server loop onto it
   behind the existing differential-parity harness (seeded games, old TS
   cycle vs kernel cycle, byte-compare committed products); then collapse
   `LocalGame.runBots` onto `fio_bot_drive_json`.
3. **F4 v6-from-game** — native test extends `replay_v6_test.c`; server
   finalize diff-tested against the TS assembly on real finished games.
4. **F5 reset transform**, then **F6 deletions** as cleanup.

Each step follows the repo's established migration pattern (mirror → parity
harness → cutover → freeze the old path as test oracle), so none of this is
novel process — it is the same playbook that moved rules and bots into C.

## 7. What future clients then get for free

| Client | Needs today | After F1–F5 |
| --- | --- | --- |
| iOS offline MVP | Swift loop + unknobbed bots | `fio_bot_drive_json` + roster: site-identical bots, site-identical feel |
| watchOS | (spectates via server; any future offline mode would re-duplicate) | links the same `libfoolish.a` slices |
| iMessage | none (no bots in v1) | pass-and-play vs a bot becomes a drawer feature: one call |
| Telegram bot / Steam | would have re-implemented the loop a 3rd/4th time | wasm (Node) or native lib + the same three calls: `new_game`, `apply`, `bot_drive` |
| `/m/` web play (v2) | — | guards.wasm already in the page; `bot_drive` export makes "practice vs bot from a shared link" trivial |
