# `player_views` — a server-written, client-read personalized view cache

## What this is

`player_views` stores each player's **already-masked** view of each game,
written **only by the server** (inside the authoritative commit), read **only by
that player** under RLS. A sibling table, **`spectator_views`**, stores the
**shared, fully-masked (seat -1)** view of each game, readable by **any**
authenticated user. Together they mean **every** read path — dashboard list,
single-game screen (player), and spectate — is a plain indexed `SELECT`, with
**no edge function** on the hot read path at all. The `get_game` and
`get_my_games` edge functions have both been **removed**.

The write path is wired into *every* commit, so the caches are always the live,
authoritative masked view of every game.

## Why a cache at all

Each player may see **only their own hand** + card-backs for everyone else —
**field-level masking inside one game row**. RLS can hide whole rows but **cannot
mask fields inside a row you're allowed to read**, so there is no RLS policy that
lets a client read `games` while hiding opponents' hands and the deck. The server
used to unpack `games.state` and emit a per-viewer masked view on every read (the
`get_game` / `get_my_games` edge functions), which paid an edge cold-start floor
(~760ms).

The view caches flip it: **mask at write time** (once per commit, per viewer) and
store the masked blob in its own row. Now RLS `player_id = auth.uid()`
(player_views) / `auth.role() = 'authenticated'` (spectator_views) is
**sufficient** — the row is pre-masked, nothing to hide on read.

## Schema (`player_views`: `migrations/20260708160000_player_views.sql`; `spectator_views`: `migrations/20260709050000_spectator_views.sql`; both mirrored in `seed.sql`)

```sql
CREATE TABLE player_views (
  game_id    TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  player_id  UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  view       TEXT NOT NULL,   -- MASKED packed view envelope (bare hex), decodable by decodePackedGame
  version    BIGINT NOT NULL, -- mirrors games.version (client's reorder-drop token)
  status     TEXT NOT NULL,   -- denormalized game_status for cheap list rendering
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (game_id, player_id)
);
CREATE INDEX idx_player_views_player ON player_views(player_id, updated_at DESC);

ALTER TABLE player_views ENABLE ROW LEVEL SECURITY;
CREATE POLICY "Players can read their own views" ON player_views
  FOR SELECT USING (player_id = (select auth.uid()));
GRANT SELECT ON public.player_views TO authenticated;      -- SELECT only
ALTER PUBLICATION supabase_realtime ADD TABLE public.player_views; -- Realtime

-- The SHARED, fully-masked (seat -1) view — one row per game, readable by ANY
-- authenticated user (it carries no hidden state). Replaces get_game's spectate.
CREATE TABLE spectator_views (
  game_id    TEXT PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE,
  view       TEXT NOT NULL,   -- FULLY-masked packed envelope (bare hex), decodable by decodePackedGame
  version    BIGINT NOT NULL, -- mirrors games.version
  status     TEXT NOT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
ALTER TABLE spectator_views ENABLE ROW LEVEL SECURITY;
CREATE POLICY "Authenticated can read spectator views" ON spectator_views
  FOR SELECT USING ((select auth.role()) = 'authenticated');
GRANT SELECT ON public.spectator_views TO authenticated;   -- SELECT only
```

**No client writes** on either table: there is deliberately no
INSERT/UPDATE/DELETE policy, so `authenticated` can only `SELECT`. The service
role (which bypasses RLS) is the sole writer.

`view` is the **same packed single-game envelope** `decodePackedGame` reads
(`encodeGameResponse`), so the read side needs nothing new. A `player_views` row
is masked **for its owner** (their own hand visible); a `spectator_views` row is
**fully** masked (seat -1 → every hand a card-back). Neither may **ever** carry
the raw `games.state` — which is exactly why exposing the spectator row to all
authenticated users is safe.

## Write path — consistency is the whole point

The N per-player view rows **and** the one shared spectator row are written in
the **same transaction** as `games.state`, under the **same version fence**, so
the caches can never be torn from the authoritative state.

- **`commit_game`** (the single commit choke point — every move, meta action,
  deal, continue, and bot move flows through `commitGame` →
  `executeWithGameLock`) gains a `p_views JSONB` param **and** a `p_spectator
  TEXT` param. After the version-gated `UPDATE games` succeeds, it upserts the
  current participants' `player_views` rows and **prunes** rows for players no
  longer in the game (exit/removal), then upserts the single `spectator_views`
  row. On a version conflict the RPC returns before touching either cache, and
  the retry recomputes.
- **`create_game`** gains `p_views` **and** `p_spectator` params too, seeding the
  creator's lobby row and the shared spectator lobby row so a new game is
  immediately readable from both direct `SELECT`s.

The rows are built in TS by `_shared/common/player_views.ts`
(`buildPlayerViewRows` / `buildSpectatorView`):

- **Dealt game** (blob present): masking stays **in the C kernel** —
  `engine.serializeViewBlobs` deserializes the state blob **once** and calls
  `wasm_view_serialize(seat)` for every seat it's handed, read-only on the
  resident game. `buildPlayerViewRows` hands it all human seats (N views, one
  deserialize + N `state_put`); `buildSpectatorView` hands it `seat -1`
  (`VIEW_SPECTATOR`).
- **Lobby** (no blob, no hidden state): the pure-TS mirror of the same kernel
  format (`writeMaskedState`, called with `seat -1` for the spectator row). No
  rules-wasm on the create/lobby cold path.

Building the views **never breaks a commit**: on failure `commitGame` passes
`p_views = null` / `p_spectator = null`, which leaves the caches untouched (a
stale row self-corrects on the next commit and the client's version token drops
it).

## Read path (client, `src/contexts/ServerContext.tsx`)

- **Dashboard list**: `getUserGamesInternal` reads
  `player_views WHERE player_id = auth.uid() ORDER BY updated_at DESC` and
  decodes each `view` with `decodePackedGame` — a plain indexed RLS SELECT, **no
  edge function, no fallback**. `commit_game` / `create_game` keep the cache
  complete, so an empty result simply means the user has no games. (`get_my_games`
  has been removed.)
- **Single game** (`loadGameInternal`): a player reads their own row for that
  game (`loadGameFromCache`: `player_views WHERE game_id = ?`, RLS-scoped to the
  caller, unique by PK) and decodes it. A **non-participant** (spectator) reads
  the shared row (`loadSpectatorFromCache`: `spectator_views WHERE game_id = ?`,
  readable by any authenticated user) and decodes it with no `self`. Both are
  plain indexed RLS SELECTs — **no edge function**. A total miss (a game
  predating both caches, or pruned) is a genuine not-found; there is no edge
  fallback anymore.

### Backfill (`buildPlayerViewUpserts`)

There is **no runtime cache-warm path** now that `get_game` is gone: steady state
is `commit_game` / `create_game` writing both caches on every commit. The
`buildPlayerViewUpserts` helper (the SAME builder `commit_game` uses, shaped for
a **fill-if-absent** write from outside a commit) is retained and e2e-tested — it
pins the byte-identical-rebuild + fill-if-absent invariants a future backfill job
would rely on — but has no live caller. `get_game` was removed only once **all
live games already had views**, so no in-flight game needs backfilling.

- **Live updates**:
  - The **on-screen game** stays driven by the existing broadcast websockets.
    A **player** consumes `RealtimeAnimationFeed`'s `gu-<game>-<user>` animation
    stream; a **spectator** consumes the **`game-<game>`** broadcast — the server
    emits a fully-masked (seat -1) animation stream there
    (`broadcastPackedEventBuffers` / `broadcastAnimationEvents`, built by the same
    WASM/event-wire encoder), and `ServerContext`'s spectator subscription
    republishes it into `animationFeed`. The caches must not push snapshots into
    the on-screen game or the view would snap past the in-flight animation (the
    dashboard subscription explicitly skips the routed game for this reason).
  - The **rest of the dashboard** is kept live by a user-scoped Realtime
    `postgres_changes` subscription (`pv-<user_id>`): committed snapshots merge
    into `games` (INSERT/UPDATE), pruned games are dropped (DELETE). Best-effort
    — if it can't connect, the list refreshes on the next `getUserGames` /
    navigation. This is a convenience layer; the latency win is the direct read,
    not the push. It could equally be served by the existing broadcast infra (a
    per-user list channel) if `postgres_changes` isn't enabled on the project.

## Decisions on the handoff's open questions

1. **Spectators** read `spectator_views` — the shared, fully-masked (seat -1)
   row, readable by any authenticated user (it carries no hidden state). This
   replaced `get_game`'s spectate path; live spectate updates ride the RLS-guarded
   `game-<id>` broadcast.
2. **Write scope**: all human participants + the one shared spectator row on every
   commit (simplest and always consistent; bots write no per-player row). See
   *Write amplification & storage*.
3. **`get_my_games` and `get_game` removed**: the dashboard reads `player_views`,
   the single game reads `player_views` (player) / `spectator_views` (spectator),
   all directly. No game-read edge function remains.
4. **Backfill**: no one-off job and no runtime self-warm. `get_game` was dropped
   only once all live games had views; `buildPlayerViewUpserts` is retained (and
   e2e-tested) as the primitive a future backfill would reuse.

## Write amplification & storage

- **Write volume**: every commit writes one `player_views` row per human
  participant plus one shared `spectator_views` row. There is no useful "skip
  viewers whose list-visible state didn't change" optimization — the row *is* the
  whole masked view, so essentially any commit that's worth broadcasting also
  changes what a viewer sees (hand counts, whose turn, the table, status). We just
  write it. The one free reduction: **bots get no per-player row** (no client
  reads one), so an all-bot game writes only the single spectator row.
- **Storage is bounded, not growing**: one `player_views` row per (game, live
  human participant) plus one `spectator_views` row per game. Nothing stale is
  ever kept — `commit_game` **prunes** a `player_views` row the moment a player
  leaves, and the `ON DELETE CASCADE` FK on both tables drops all of a game's rows
  when the game is deleted. So total rows ≈ Σ (humans currently in a game) +
  (live games), which is small and self-limiting.

## Follow-ups

- The per-game **read** path is now on `player_views` / `spectator_views`; the
  on-screen game's **live** updates still (deliberately) ride the existing
  animation broadcasts (`gu-` for players, `game-<id>` for spectators) so moves
  animate. If we ever want the game screen to consume cache snapshots directly, it
  would need to reconcile with the animation timeline first.

## Tests

`e2e/player_views.test.ts` (real Postgres, real `commit_game` / `create_game`):
a dealt commit writes one masked, **byte-identical-to-the-builder**, decodable
row per human (none for bots); a **move** refreshes each row (version bumped,
still masked, still byte-identical on the new state); `create_game` seeds the
lobby row; exiting prunes the leaver's row; RLS lets a player read only their own
rows and blocks client writes. Spectator coverage: a dealt commit /
`create_game` writes one **fully-masked, no-self** `spectator_views` row
(byte-identical to `buildSpectatorView`, opponents' cards never present), and RLS
lets **any** authenticated user (a non-participant) read it while blocking client
writes.

> The move test caught a real bug: the envelope's version token was built with
> `expectedVersion + 1`, which is **string concatenation** (`"1" + 1 === "11"`)
> when the loaded version arrives as a BIGINT string — desyncing the client's
> reorder-drop token from the row's version column. Fixed with `Number(...)`.
