# `player_views` — a server-written, client-read personalized view cache

## What this is

`player_views` stores each player's **already-masked** view of each game,
written **only by the server** (inside the authoritative commit), read **only by
that player** under RLS. It lets the client load its **dashboard list** as a
plain indexed `SELECT` — no `get_my_games` edge round-trip — and receive **live
push updates** over Realtime.

This is the **dashboard-list prototype** from the design handoff (the de-risked
first step): the write path is wired for *every* commit (so per-game live views
are forward-compatible), but the client currently reads only the **list** from
the cache. `get_game` / `get_my_games` stay as the authoritative fallback.

## Why a cache at all

Each player may see **only their own hand** + card-backs for everyone else —
**field-level masking inside one game row**. RLS can hide whole rows but **cannot
mask fields inside a row you're allowed to read**, so there is no RLS policy that
lets a client read `games` while hiding opponents' hands and the deck. Today the
server unpacks `games.state` and emits a per-viewer masked view on every read
(`get_game` / `get_my_games`), which pays an edge cold-start floor (~760ms).

`player_views` flips it: **mask at write time** (once per commit, per viewer) and
store the per-player masked blob in its own row. Now RLS
`player_id = auth.uid()` is **sufficient** — the row is pre-masked, nothing to
hide on read.

## Schema (`supabase/migrations/20260708160000_player_views.sql`, mirrored in `seed.sql`)

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
```

**No client writes**: there is deliberately no INSERT/UPDATE/DELETE policy, so
`authenticated` can only `SELECT`. The service role (which bypasses RLS) is the
sole writer.

`view` is the **same packed single-game envelope** the `get_game` /
`get_my_games` edge functions emit (`encodeGameResponse` → `decodePackedGame`),
so the read side needs nothing new. It is masked for its owner and must **never**
carry the raw `games.state`.

## Write path — consistency is the whole point

The N view rows are written in the **same transaction** as `games.state`, under
the **same version fence**, so the cache can never be torn from the
authoritative state.

- **`commit_game`** (the single commit choke point — every move, meta action,
  deal, continue, and bot move flows through `commitGame` →
  `executeWithGameLock`) gains a `p_views JSONB` param. After the version-gated
  `UPDATE games` succeeds, it upserts the current participants' rows and
  **prunes** rows for players no longer in the game (exit/removal), so a leaver
  stops seeing the game in their list. On a version conflict the RPC returns
  before touching `player_views`, and the retry recomputes.
- **`create_game`** gains a `p_views` param too, seeding the creator's lobby row
  so a new game is immediately readable from the direct `SELECT`.

The rows are built in TS by `_shared/player_views.ts::buildPlayerViewRows`:

- **Dealt game** (blob present): masking stays **in the C kernel** —
  `engine.serializeViewBlobs` deserializes the state blob **once** and calls
  `wasm_view_serialize(seat)` for each human seat (read-only on the resident
  game), so N views cost one deserialize + N `state_put`.
- **Lobby** (no blob, no hidden state): the pure-TS mirror of the same kernel
  format (`writeMaskedState`) — the identical fallback `get_my_games` already
  uses for blob-less rows. No rules-wasm on the create/lobby cold path.

Building the views **never breaks a commit**: on failure `commitGame` passes
`p_views = null`, which leaves `player_views` untouched (a stale row is safe —
the client falls back to `get_my_games`).

## Read path (client, `src/contexts/ServerContext.tsx`)

- **Dashboard list**: `getUserGamesInternal` reads
  `player_views WHERE player_id = auth.uid() ORDER BY updated_at DESC` and
  decodes each `view` with `decodePackedGame` — no edge function. On a cold /
  empty / unreadable cache it falls back to `get_my_games` (the authoritative
  rebuild path, also the backfill for games created before this feature).
- **Live updates**: a user-scoped Realtime `postgres_changes` subscription
  (`pv-<user_id>`) pushes every committed masked snapshot into `games`
  (INSERT/UPDATE) and drops pruned games (DELETE). It is the list-level
  counterpart of `RealtimeAnimationFeed`'s per-game stream — full snapshots
  instead of event deltas. Best-effort: if it can't connect, the list still
  refreshes on the next `getUserGames` / navigation.

## Decisions on the handoff's open questions

1. **Spectators** keep using `get_game` — the dashboard is "my games"
   (participant rows), which is exactly what `player_id = auth.uid()` expresses.
2. **Write scope**: all participants on every commit (simplest and always
   consistent). See *Known limitations* for the write-amplification tradeoff.
3. **`get_my_games` stays** as the cache-miss fallback / rebuild path.
4. **Backfill**: the write path repopulates naturally — a game gets its rows on
   its next commit — and `get_my_games` covers anything not yet cached, so no
   one-off backfill job is required for the prototype.

## Known limitations / follow-ups

- **Write amplification**: every commit writes one row per human participant,
  including per-move commits (this is what makes per-game live views a cheap
  next step, but it is more write volume than the list strictly needs). A future
  optimization: skip viewers whose list-visible representation didn't change.
- **Per-game live reads** (step 3 of the handoff): the cache is already written
  for live games, so the game screen could read/subscribe to its own
  `player_views` row directly. Not wired yet — the game screen still uses
  `get_game` + the `gu-` animation stream.

## Tests

`e2e/player_views.test.ts` (real Postgres, real `commit_game` / `create_game`):
a dealt commit writes one masked, **byte-identical-to-`get_game`**, decodable row
per human (none for bots); `create_game` seeds the lobby row; exiting prunes the
leaver's row; RLS lets a player read only their own rows and blocks client
writes.
