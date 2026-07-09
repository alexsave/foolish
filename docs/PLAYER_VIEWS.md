# `player_views` — a server-written, client-read personalized view cache

## What this is

`player_views` stores each player's **already-masked** view of each game,
written **only by the server** (inside the authoritative commit), read **only by
that player** under RLS. Both the **dashboard list** and the **single-game
screen** now load a player's own view as a plain indexed `SELECT` — no
`get_my_games` / `get_game` edge round-trip on the hot read path. `get_game` /
`get_my_games` remain as the authoritative fallback (spectators, a cold cache).

The write path is wired into *every* commit, so the cache is always the live,
authoritative masked view of every game a player is in.

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
  empty / unreadable cache it falls back to `get_my_games`.
  - **One-time warm per device.** Because the cache is written going *forward*,
    games that predate it have no row — and a direct read that returns a
    *partial* list would (being non-empty) never trigger the rebuild that
    backfills the rest. So until `get_my_games` has run once on this device (a
    `localStorage` `pv_warmed` flag), the client treats the edge function as
    authoritative — that call backfills **all** of the caller's rows (see below).
    After that, the dashboard is a pure `player_views` SELECT and `get_my_games`
    drops out of the read path (~once per device, then quiet).
- **Single game** (`loadGameInternal` → `loadGameFromCache`): a player reads
  their own row for that game (`player_views WHERE game_id = ?`, RLS-scoped to
  the caller, unique by PK) and decodes it — no `get_game` round-trip. Falls
  back to `get_game` for a **spectator** (no row) or a cache miss.

### Cache warm / backfill (the rebuild functions self-heal)

`player_views` is written going forward, so games that existed before it shipped
have no row. Rather than a separate one-off job, the two **read fallbacks
backfill as they serve** — and they write rows for **all human participants** of
each game (via `buildPlayerViewUpserts` → the SAME builder `commit_game` uses, so
each row is byte-identical), not just the invoker. So one player's dashboard load
(`get_my_games`) or any player's / spectator's game fetch (`get_game`) backfills
the whole game for everyone, converging the cache across users in a single pass.

Both write **fill-if-absent** (`Prefer: resolution=ignore-duplicates` /
`ignoreDuplicates`) and **fire-and-forget** (`EdgeRuntime.waitUntil`, so no added
response latency). Fill-if-absent is load-bearing: `commit_game` owns UPDATEs
under the version fence, and a read-path write is *not* fenced — so it may only
INSERT a row that doesn't exist, never overwrite a possibly-newer committed one.
Net effect: the first post-deploy read of each game populates every
participant's row, and every read after is direct.

> This all-participants warm is a **temporary backfill measure** — it (and
> `get_my_games` itself) comes out once the cache is fully populated.
- **Live updates**:
  - The **on-screen game** stays driven by the existing broadcast websockets
    (`RealtimeAnimationFeed`'s `gu-` animation stream) — that path is proven and
    animates moves; `player_views` must not push snapshots into it or the view
    would snap past the in-flight animation (the dashboard subscription
    explicitly skips the routed game for this reason).
  - The **rest of the dashboard** is kept live by a user-scoped Realtime
    `postgres_changes` subscription (`pv-<user_id>`): committed snapshots merge
    into `games` (INSERT/UPDATE), pruned games are dropped (DELETE). Best-effort
    — if it can't connect, the list refreshes on the next `getUserGames` /
    navigation. This is a convenience layer; the latency win is the direct read,
    not the push. It could equally be served by the existing broadcast infra (a
    per-user list channel) if `postgres_changes` isn't enabled on the project.

## Decisions on the handoff's open questions

1. **Spectators** keep using `get_game` — the dashboard is "my games"
   (participant rows), which is exactly what `player_id = auth.uid()` expresses.
2. **Write scope**: all human participants on every commit (simplest and always
   consistent; bots write nothing). See *Write amplification & storage*.
3. **`get_my_games` stays** as the cache-miss fallback / rebuild path.
4. **Backfill**: no one-off job. The read fallbacks (`get_my_games` / `get_game`)
   **self-warm** the cache fill-if-absent as they serve (see *Cache warm /
   backfill*), so games predating the cache populate on their first post-deploy
   read and every read after is direct.

## Write amplification & storage

- **Write volume**: every commit writes one row per human participant. There is
  no useful "skip viewers whose list-visible state didn't change" optimization —
  the row *is* the whole masked view, so essentially any commit that's worth
  broadcasting also changes what a participant sees (hand counts, whose turn,
  the table, status). We just write it. The one free reduction already in place:
  **bots get no row** (no client reads one), so an all-bot game writes nothing.
- **Storage is bounded, not growing**: exactly one row per (game, live human
  participant). Nothing stale is ever kept — `commit_game` **prunes** a row the
  moment a player leaves, and the `ON DELETE CASCADE` FK drops all of a game's
  rows when the game is deleted. So total rows ≈ Σ (humans currently in a game),
  which is small and self-limiting.

## Follow-ups

- The per-game **read** path is now on `player_views`; the on-screen game's
  **live** updates still (deliberately) ride the existing `gu-` animation
  broadcast so moves animate. If we ever want the game screen to consume
  `player_views` snapshots directly, it would need to reconcile with the
  animation timeline first.

## Tests

`e2e/player_views.test.ts` (real Postgres, real `commit_game` / `create_game`):
a dealt commit writes one masked, **byte-identical-to-`get_game`**, decodable row
per human (none for bots); a **move** refreshes each row (version bumped, still
masked, still byte-identical to `get_game` on the new state); `create_game` seeds
the lobby row; exiting prunes the leaver's row; RLS lets a player read only their
own rows and blocks client writes.

> The move test caught a real bug: the envelope's version token was built with
> `expectedVersion + 1`, which is **string concatenation** (`"1" + 1 === "11"`)
> when the loaded version arrives as a BIGINT string — desyncing the client's
> reorder-drop token from the row's version column. Fixed with `Number(...)`.
