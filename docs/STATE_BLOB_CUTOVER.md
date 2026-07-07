# Packed state-blob cut-over — status & remaining verification

The game's volatile state (hands, deck, table battles, seat positions/statuses,
good-mask, elimination order) is now persisted and loaded as **one packed,
versioned binary blob produced by the C rules kernel** (`games.state`, hex),
instead of the scattered JSONB columns + `game_decks` / `player_hands` /
`bot_hands` joins. The blob is ~96 bytes vs multi-KB of JSONB.

This doc records what is **verified in-repo**, and what still needs **live
verification against a real Supabase stack** (which the dev container cannot run
— no `supabase` CLI; a hand-rolled Kong/PostgREST/Realtime/GoTrue/edge stack was
out of scope).

## What shipped (commits on this branch)

1. **Versioned state codec** — `wasm_state_serialize`/`deserialize` in the kernel
   (`cnitro/wasm/wasm_api.c`) + `serializeGameState`/`deserializeGameState` in
   `supabase/functions/_shared/wasm/engine.ts`. Format-version byte for forward
   compat (mirrors the replay codec's v2–v5 discipline).
2. **Blob persistence (dual-write)** — `commit_game` gained `p_state`;
   `loadCompleteGame` reconstructs from the blob.
3. **Cut-over (this change)**:
   - `commitGame` **stops writing** `player_hands`/`bot_hands`/`game_decks`
     **once a game is dealt** (blob is authoritative). It still writes them while
     the game is a lobby, so `player_hands` keeps doubling as the player↔game
     **membership index**.
   - `loadCompleteGame` sources bot `strategy_key` from the `bots` table (not the
     now-unwritten `bot_hands`), and no longer depends on hand rows existing.
   - New read-only edge functions **`get_game`** and **`get_my_games`** return the
     caller's server-personalized view (own hand; everyone else's as card-backs).
   - The web client (`ServerContext.tsx`) no longer reads `games`/`player_hands`
     directly — it calls `get_game` (single game / spectate) and `get_my_games`
     (dashboard list). The `handsQuery` PostgREST projection was removed.

## Verified here

- **`npm run test:e2e` → 94/94** against real Postgres: card conservation
  (sequential + under contention), fuzz, concurrent games, lobby→playing→game-over
  transitions, leaderboard, lease. The card-conservation counter (`e2e/dispatch.ts`)
  now reconstructs deck+hands from the blob.
- **`e2e/state_codec.test.ts`** — 36,787 seeded round-trips, byte-lossless.
- **`npm run build`** — TypeScript type-checks clean with the client changes.
  (The build's `/_not-found` prerender error is **pre-existing** — it reproduces
  with `src/` stashed — and unrelated to this change.)
- Kernel perf unchanged (`bench_engine` ~333 games/s); wasm embeds smaller after
  the export-trim change.

## NOT verified here — do this in a real Supabase env before merge

The dev container can't run the full stack, so **live client play was not
exercised.** After deploying (edge functions + migration
`20260707120000_game_state_blob.sql`):

1. **Deploy the two new functions**: `supabase functions deploy get_game get_my_games`.
2. **Load & play a game**: open a game you're in → your hand renders, others show
   card-backs; play attack/cover/pass/pickup/good; **reload mid-game** → state is
   restored from the blob (this is the core blob round-trip through the live DB).
3. **Dashboard list** (`get_my_games`): the "my games" list populates, ordered
   most-recent-first, with correct per-game status.
4. **Spectate** a game you're not in → public view, no hand leak.
5. **Multi-client**: two browsers in one game → each sees only its own hand; a
   move broadcasts and both converge (realtime path is unchanged, but confirm the
   initial `get_game` fetch is consistent with the broadcast state).
6. **Bot game**: add a bot, start → bot plays; confirm `strategy_key` resolves
   (now from the `bots` table) and the bot's hand never leaks to the client.
7. **Cold-start latency**: confirm `create`/lobby cold starts are unaffected (the
   blob codec is lazy-imported; they should not pull the rules-wasm embed).

## Rollout / migration notes

- **Legacy in-flight games** (rows committed before `games.state` existed) have
  `state = NULL`; `loadCompleteGame` falls back to the JSONB-join path for them,
  and their next commit produces a blob. Safe to deploy without a backfill. A
  one-time backfill (load→re-commit each active game) would let you delete the
  fallback sooner.
- **The hand tables are now vestigial during play** (lobby rows only). They are
  intentionally **not dropped** in this change — dropping `game_decks` /
  `player_hands` / `bot_hands` (and simplifying `create_game`, `meta_actions`
  exit/add-bot, `commit_game` params, the `seedGame` harness, and the
  `meta.test.ts` row-count assertions) is a follow-up once (a) all active games
  carry a blob and (b) the membership index is moved off `player_hands` (e.g. a
  `game_players` table or a `games.player_ids uuid[]`).

## The payoff path — SHIPPED (see PACKED_WIRE_CUTOVER.md)

The plan below has landed: `get_game` returns the kernel-masked packed blob,
actions POST binary wire bodies, broadcasts carry packed per-viewer event
streams, and the mask is computed inside the kernel (`cnitro/src/view.c`).
Kept for history:

Once the client runs the rules kernel in-browser, **`get_game` stops returning
JSON and returns a per-player *masked* packed blob**, which the client unpacks
directly — **zero conversion**, the server becomes a near-pure byte-shuffler, and
the **same versioned format** (`serializeGameState`) spans storage, server
compute, and client. The one thing that must stay server-side is the **mask**
(zeroing other players' hands before the bytes leave the building) — the
"you only see your own hand" rule can't move to the client. `get_game` is
already the seam for this; only its response body changes.
