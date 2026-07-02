# Architecture review — July 2026

A full pass over the project: the e2e harness was run against the real server
code + real Postgres to surface glitches, and the server architecture flow and
client data flow were audited end to end. This document records what was found,
what was **fixed on this branch**, and a ranked list of the biggest remaining
improvements with file references.

Method: `npm run test:e2e` (all 58 tests passed, but the run's *logs* surfaced
real production glitches the assertions didn't cover), then targeted repro
scripts against the harness to root-cause them, then a code audit of
`supabase/functions/` and `src/`.

---

## Part 1 — Glitches found by the harness, fixed on this branch

### 1.1 Replay snapshots silently failed for ~1 in 5 finished games (fixed)

**Symptom.** Every full e2e run printed several of:

```
[REPLAY] Snapshot failed for game … — keeping logs:
Error: replay desync: logged attack not in menu of 4
```

A finished game is supposed to be compressed into a `game_snapshots` row and
its `game_logs` wiped (`finalizeEndedGame`, `_shared/utils.ts`). Instead,
roughly 20% of finished games (soak-measured: 3–4 per 20) kept raw logs and got
no shareable replay. The failure is silent by design — game completion is never
blocked on the snapshot — which is exactly why it survived in production.

**Root cause.** `game_logs.created_at` is stamped client-side by `addLog`
(`new Date().toISOString()`, millisecond precision), and one move's cascade
(attack → player_out → defender_change → draw…) emits several logs inside the
same millisecond. `loadCurrentSessionLogs` ordered the session by
`created_at` **alone**; Postgres sorts are not stable, so tied rows came back
in arbitrary order. The replay encoder consumes that stream as the exact action
sequence — a scrambled cascade desyncs it.

Proven by repro: for every failing game, re-running `verifyRoundTrip` on the
same rows ordered by *insertion order* succeeded; ordered by bare `created_at`
it failed. Log saves are strictly sequential (instrumented: max 1 in-flight
insert), so insertion order is commit order.

**Fix.**
- `supabase/migrations/20260701120000_game_logs_seq.sql` + `seed.sql`: add
  `seq BIGSERIAL` to `game_logs` (assigned in insert order — `saveGameLogs`
  upserts each move's logs as one array in emit order, and moves commit
  serially through the `games.version` CAS).
- `log_utils.ts loadCurrentSessionLogs`: order by `(created_at, seq)`.
- New regression test in `e2e/server.test.ts`: every finished game must have
  exactly one snapshot row and zero remaining logs (6 games per run).
- Post-fix soak: 30/30 finished games snapshot cleanly.

### 1.2 The last player exiting a lobby got an HTTP 400 (fixed)

`handleExit` (`_shared/meta_actions.ts`) deletes the `games` row directly when
the last player leaves — then returned into `executeWithGameLock`, whose
version-CAS `UPDATE` found no row, read that as a `conflict`, and the retry's
`loadCompleteGame` threw `Game … not found` → HTTP 400 for a teardown that had
succeeded. The e2e test even papered over it with `.catch(() => {})`.

**Fix.** The handler now returns `deleted: true` and `executeWithGameLock`
skips the commit for a deleted game. The test asserts the call *resolves*.

### 1.3 e2e adapter gaps: `.lt()` and chained `.order()` (fixed)

Every game end logged
`TypeError: supabaseClient.from(...).delete(...).eq(...).lt is not a function`:
the pg-backed supabase shim (`e2e/adapters/supabase.ts`) didn't implement the
`.lt` filter that `cleanupOldGameLogs` uses, so that fallback path was dead
under test. It also kept only the last `.order()` call, while supabase-js
appends. Both now match the real client's behavior (needed for the
`(created_at, seq)` ordering above).

### 1.4 Client optimistic patches wrote state derived from a stale closure (fixed)

`ServerContext.attack/pass/pickup/cover` fire the server request immediately,
then patch local state optimistically after `ANIMATION_TIME` (500ms). The patch
read `games[game_id]` from the **render-time closure** — state captured when
the move was fired — computed the new `table_battles`/hand from that snapshot,
and wrote it into `setGames`. Any broadcast landing inside the 500ms window was
clobbered by the stale reconstruction (e.g. an opponent's move vanishing from
the table until the next broadcast).

**Fix.** All four patches now derive everything inside the
`setGames(prev => …)` updater from `prev`, so they compose with whatever state
is current when the timer fires.

---

## Part 2 — Server architecture

### The flow (one human move)

```
client POST /action {type, game_id, …}
  wrap400 (utils.ts)            CORS → auth (JWT claims) → parse body
  executeWithGameLock           load → compute → CAS-commit loop (≤5 attempts)
    loadCompleteGame            ONE joined select (games+deck+hands); logs lazy
    handler                     validate → execute; mutates game in memory,
                                emits AnimationEvents + addLog entries
    check_win_sync              pure GAME_OVER detection, in memory
    commit_game RPC             single txn: UPDATE games WHERE version=expected
                                (+ deck/hands upserts), bumps version
    [game over] saveGameLogs → finalizeEndedGame (ELO, replay snapshot, wipe)
    broadcastAnimationEvents    fire-and-forget, ONE batched Realtime POST,
                                personalized per human + public spectator msg
    saveGameLogs                this move's logs (after broadcast kickoff)
  wrap400 tail                  personalize response; EdgeRuntime.waitUntil →
                                lockedBotLoop (lease-guarded bot drive)
```

The core concurrency design is sound and verified by the harness: no locks are
held across compute; the `games.version` CAS makes a stale commit impossible
(card conservation held under bursts of overlapping submits in every run), the
bot lease gives exactly-one bot driver with TTL recovery, and broadcasts carry
the committed version so clients can drop reordered deliveries.

### Biggest server improvements (ranked, not yet done)

1. **CAS-conflict retries re-run the full bot compute.**
   `executeWithGameLock` re-invokes the whole operation on conflict — for a
   bot move that means re-running strategy compute (`strategies/cordite_core.ts`
   is a Monte-Carlo engine) up to 5×. Under contention with a human move this
   can blow Supabase's ~2s CPU budget and get the isolate killed holding the
   lease (until TTL). Consider computing the move once and only re-validating
   cheaply on retry, or capping strategy time under retry.
   (`_shared/utils.ts` retry loop; `_shared/bot_actions.ts`.)

2. **Game start doesn't wake the bots.** `meta` runs with `run_bots=false`, so
   when a bot is first attacker after `start`/`add-bot`, nothing drives it
   until the 10s `bot-heartbeat` cron or a client `bump`. Kicking the bot loop
   after a `start` that dealt cards would remove the dead first seconds.
   (`meta/index.ts`; compare `action/index.ts` which passes `run_bots=true`.)

3. **`bump` is an unauthenticated-relative-to-the-game compute trigger.** It
   deliberately skips `verify_player_in_game` (spectators may nudge), but that
   means any authenticated user can repeatedly fire any game's bot loop; the
   lease caps concurrency, not invocation rate. A cheap membership-or-spectator
   check, or rate limit, would close it. (`action/index.ts`.)

4. **`currentBotDelay` is a module-global mutated per game.** One warm isolate
   can drive multiple games (heartbeat SCAN); a bots-only game setting 300ms
   pacing leaks into a concurrent humans game (3000ms expected) and vice versa.
   Should be a local of `lockedBotLoop`. (`_shared/bot_actions.ts`.)

5. **The all-good 60s timeout is disabled — a vanished attacker stalls the
   round forever.** Deliberate (`good.ts`: "long-game sessions no longer
   auto-discard out from under absent attackers"), but the trade-off leaves
   live games stallable by one disconnect, and its scaffolding
   (`good_timestamp`, `oneMinutePassed`, the whole `auto_discard_locks` table)
   is now dead weight. Either re-enable with a longer window and presence
   detection, or delete the scaffolding.

6. **`finalizeEndedGame` does ELO serially** — one select per player plus
   separate upserts on the game-ending hot path, delaying the final
   MAGIC_TRANSITION broadcast. Batchable into one RPC.
   (`_shared/utils.ts updateEloRatings`.)

7. **Broadcast is fire-and-forget with no retry.** Correct-by-version but a
   dropped POST means a missed animation until the next event; a single cheap
   retry would cover transient Realtime hiccups. (`utils.ts broadcastMessages`.)

Cleanups: dead `auto_discard_locks` table (see 5); comments referencing removed
constructs (`saveCompleteGame`, `game_locks`/`bot_locks`); an abandoned no-op
loop in `pure_bot_actions.ts`; unused `seededRandom` with the seeded-draw path
commented out (`common_utils.ts`).

---

## Part 3 — Client data flow

### The flow (one move)

```
tap → ActionButtons/DragContext → AnimationContext.attack(cards)
  ServerContext.attack           fires POST /action FIRST (server authoritative)
  validate locally → triggerOptimisticAnimation (tracked in optimistic maps)
  AnimationOverlay               flies the card hand→table over 500ms
  setTimeout(ANIMATION_TIME)     optimistic state patch (now derived from prev)
server broadcast (gu-<game>-<user>)
  RealtimeAnimationFeed          republish onto the animationFeed bus
  AnimationContext               version gate (drop stale) → sequence/content
                                 dedup → skip re-animating optimistic cards →
                                 queue remaining events → commit message.game
reconnect: RealtimeAnimationFeed re-SUBSCRIBED → REST reload →
  applyOptimisticOverlay re-injects unconfirmed local cards
```

The strong part: reconciliation is extracted into pure, e2e-tested modules
(`src/state/clientReconcile.ts`, `optimisticAnimation.ts`,
`optimisticOverlay.ts`), and the `animationFeed` bus lets live play, replays,
and the tutorial drive the identical animation pipeline.

### Biggest client improvements (ranked, not yet done)

1. **Two subscriptions to the same Realtime topic, one of them inert.** Both
   `ServerContext.subscribeToGame` and `RealtimeAnimationFeed` join
   `gu-<gameId>-<userId>`. The ServerContext channel's message handler is a
   no-op (its only state-updating branch is commented out), so it's pure
   duplicate socket load — and its cleanup does
   `supabase.getChannels().forEach(removeChannel)`, tearing down channels owned
   by `RealtimeAnimationFeed` too, making reconnection ordering a cross-context
   race. Delete the dead subscription and scope the cleanup to channels the
   context created. (`src/contexts/ServerContext.tsx` `handleGameMessage`,
   cleanup effect; `src/state/RealtimeAnimationFeed.tsx`.)

2. **`game_id` (context state) vs `url_game_id` (route param) can diverge.**
   Actions POST with `game_id` while the version gate, bot-bump timer, and
   animation handling key off `url_game_id`; during navigation a move can
   target a different game than the one being gated/animated. Pick one source
   of truth (the URL) and derive the other. (`ServerContext.tsx`,
   `AnimationContext.tsx`.)

3. **Dedup windows are asymmetric.** Content-signature dedup clears whenever
   the animation queue drains, and `processedSequenceIds` trims to the last 25
   — both leave windows where a re-delivered broadcast re-animates a move. The
   version gate catches most, but same-version redelivery (Realtime at-least-
   once) slips through right after a drain. (`AnimationContext.tsx`.)

4. **Context values regenerate every render.** `ServerContext`'s provider value
   is a fresh object containing the whole `games` map; two dozen `useServer()`
   consumers re-render on every `setGames` (each animation commit). Splitting
   current-game state from action methods (or memoizing selectors) would cut
   most of the churn. Same for `AnimationContext`'s value object.

5. **`resolveOptimisticConflicts` is a ~400-line imperative hotspot** with
   `JSON.parse(JSON.stringify(…))` deep clones per conflicting broadcast. It
   concentrates the hardest reconciliation races; extracting it into a pure,
   e2e-tested module (like `clientReconcile.ts`) would make regressions
   catchable by the harness. (`AnimationContext.tsx`.)

6. **Timing is a matched-constant contract.** Optimistic correctness leans on
   `ANIMATION_TIME`-synchronized `setTimeout`s across three files (the comments
   themselves say "change them together"). Backgrounded-tab timer throttling or
   >500ms realtime latency breaks the assumed ordering; driving the patch from
   the animation's completion callback instead of a parallel timer would remove
   the coupling.

Cleanups: inert `handleGameMessage` + dead `localHand` state and disabled
blocks in `ServerContext.tsx`; commented-out relics and a `FeedAnimationEvent`
union that omits the `revert` type actually used client-side
(`AnimationContext.tsx`, `src/state/animationFeed.ts`); `offlinefun/` is an
unwired parallel copy of engine/types that drifts silently (excluded in
`tsconfig.json`); leftover debug logging (`rearrangePlayer`).

---

## Suggested order of attack

| # | Item | Kind | Effort |
|---|------|------|--------|
| ✅ | Replay snapshot log ordering | prod bug | done (this branch) |
| ✅ | Last-player exit 400 | prod bug | done (this branch) |
| ✅ | Stale-closure optimistic patches | client bug | done (this branch) |
| ✅ | Adapter `.lt` / multi-`.order` | harness gap | done (this branch) |
| 1 | Delete ServerContext's dead `gu-` subscription + scoped channel cleanup | client bug | small |
| 2 | Kick bot loop on game start | UX | small |
| 3 | Bot compute reuse across CAS retries | perf/stability | medium |
| 4 | `game_id` vs `url_game_id` single source of truth | client bug | medium |
| 5 | `currentBotDelay` → per-loop local | server bug | small |
| 6 | `bump` membership/rate check | hardening | small |
| 7 | Context value splitting / memoization | perf | medium |
| 8 | Extract `resolveOptimisticConflicts` into a pure tested module | robustness | large |
| 9 | Decide the disconnected-attacker story (timeout vs presence) | design | medium |
| 10 | Dead-code sweep (auto_discard_locks, inert handlers, offlinefun drift) | cleanup | small |
