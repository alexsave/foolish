-- =============================================================================
-- Supabase Schema for Game Application
-- Copy and paste this entire script into Supabase's SQL Editor
-- =============================================================================

-- Enable necessary extensions first
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- =============================================================================
-- CLEANUP: Drop existing objects in correct dependency order
-- =============================================================================

-- Drop functions first (triggers will be dropped automatically with tables)
DROP FUNCTION IF EXISTS update_updated_at_column() CASCADE;

-- Drop tables in reverse dependency order (this will automatically drop all policies and triggers)
DROP TABLE IF EXISTS spectator_views CASCADE;
DROP TABLE IF EXISTS player_views CASCADE;
DROP TABLE IF EXISTS game_snapshots CASCADE;  -- `supabase start` applies the migrations first, which create it
DROP TABLE IF EXISTS chat_messages CASCADE;
DROP TABLE IF EXISTS bot_hands CASCADE;
DROP TABLE IF EXISTS player_hands CASCADE;
DROP TABLE IF EXISTS games CASCADE;
DROP TABLE IF EXISTS user_elo_ratings CASCADE;
DROP TABLE IF EXISTS bots CASCADE;
DROP TABLE IF EXISTS auto_discard_locks CASCADE;

-- Drop custom types
DROP TYPE IF EXISTS game_status CASCADE;
DROP TYPE IF EXISTS player_status CASCADE;

-- =============================================================================
-- CUSTOM TYPES: Define enums for better type safety
-- =============================================================================

CREATE TYPE player_status AS ENUM (
  'idle',
  'ready', 
  'in',
  'out'
);

CREATE TYPE game_status AS ENUM (
  'waiting',
  'playing',
  'game_over'
);

-- (log_type enum retired with the game_logs table — the session log now lives
-- in games.logs_packed as packed bytes, whose record types are the C kernel's
-- LOG_* ids, not this SQL enum. See migration 20260708120000.)

-- =============================================================================
-- MAIN TABLES: Separated for security
-- =============================================================================

-- Games table - stores PUBLIC game state only (no deck, no hands)
CREATE TABLE games (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL DEFAULT 'Untitled Game',
  deck_length INTEGER NOT NULL DEFAULT 0,
  discard_pile_length INTEGER NOT NULL DEFAULT 0,
  flipped JSONB, -- Card | null - public info
  players JSONB NOT NULL DEFAULT '[]'::jsonb, -- Player[] WITHOUT hands - only name, id, status, position, hand_length
  status game_status NOT NULL DEFAULT 'waiting',
  power_suit INTEGER,
  first_attacker INTEGER,
  defender INTEGER,
  table_battles JSONB NOT NULL DEFAULT '[]'::jsonb, -- Battle[] - public info
  elimination_order JSONB NOT NULL DEFAULT '[]'::jsonb, -- Array of player_ids in order they were eliminated
  good_timestamp BIGINT, -- Timestamp in milliseconds when all attacks were covered, null if not all covered
  good_players JSONB NOT NULL DEFAULT '[]'::jsonb, -- Array of player_ids who have pressed 'good'
  state TEXT, -- packed kernel state blob (\x-hex): the kernel's durable board (c/src/table.h TABLE_STATE_FORMAT). Authoritative once a game is dealt; a WAITING row holds the lobby blob (seats only, no cards), which the games_legacy_bridge trigger derives from `players` while the JSONB writers own the row.
  roster TEXT, -- SENSITIVE like `state` (server-only, not in the column grant): the kernel's durable roster (\x-hex, c/src/roster.h, format 1, 1227 bytes): each seat's id, name and bot brain, and the table title. Nullable until the contract migration (docs/C_GAME_SHAPE_MIGRATION.md 3.4).
  needs_bots BOOLEAN NOT NULL DEFAULT FALSE, -- PLAYING and a bot seat is still IN (the kernel's table_needs_bots): the bot heartbeat's scan predicate.
  writer_gen SMALLINT NOT NULL DEFAULT 1, -- bridge-only, dropped by the contract migration: 1 = the JSONB writers own the row (the bridge trigger derives roster/needs_bots/lobby state), 2 = the kernel writers (commit_table/create_table) own it and commit_game refuses it.
  game_seed TEXT, -- SENSITIVE (server-only, like `state`): 64 hex chars = the 32-byte deal seed the deck was ChaCha-shuffled from. Regenerates the deal for audit/replay. NEVER granted to anon/authenticated (omitted from the column GRANT below); NULL for legacy/never-dealt games.
  logs_packed TEXT, -- packed session log stream (BARE hex, no \\x prefix — appended by plain concat): kernel log records + u48 timestamps, DRAW identities pre-masked. The sole session-log store (the game_logs table was dropped in migration 20260708120000); see wire/logwire.ts and migration 20260707150000.
  version BIGINT NOT NULL DEFAULT 0, -- optimistic-concurrency token (see commit_game RPC); replaces game_locks
  round_epoch BIGINT NOT NULL DEFAULT 0, -- the `version` at which the CURRENT round began — bumped to the new version on every commit whose move closed a round (pickup/discard). The server's round-boundary guard rejects a move whose client-intent version predates this (REJECT_STALE_ROUND); see docs/WEB_RACE_BUG_HANDOFF.md and commit_game's p_closed_round. 0 = round 1 / never-closed (the guard is a no-op).
  bot_lease_token UUID,              -- bot-loop lease holder token (replaces bot_locks)
  bot_lease_until TIMESTAMPTZ,       -- bot-loop lease expiry; auto-expiring, no finally-release needed
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW()
);

-- The same words as the line comments above, but stored in the catalog, which is
-- where the expand migration puts them (20260917140000) and therefore where a
-- reader of the hosted database finds them. A line comment in this file reaches
-- nothing but this file. games.state and games.logs_packed get theirs from the
-- BYTEA migration, which is the next deploy's PR.
COMMENT ON COLUMN games.roster IS
  'The kernel''s durable roster (c/src/roster.h, format 1, 1227 bytes) as \x-hex: each seat''s id, name and bot brain, and the table title. SENSITIVE like state (server-only, not in the column grant).';
COMMENT ON COLUMN games.needs_bots IS
  'PLAYING and a bot seat is still IN (the kernel''s table_needs_bots): the bot heartbeat''s scan predicate.';
COMMENT ON COLUMN games.writer_gen IS
  'Bridge-only, dropped by the contract migration. 1: the JSONB writers own this row and a trigger derives roster/needs_bots/lobby state from players. 2: the kernel writers (commit_table/create_table) own it and commit_game refuses it.';

-- Game decks table - SENSITIVE: Only edge functions can access
-- MEMBERSHIP: which humans are in which game. Named for the hands it used to
-- carry - the dealt state is games.state, and the hand/awaiting_attack columns
-- were write-only by the time they were dropped (migration 20260906120000).
-- Load-bearing even though it holds no cards: the realtime RLS policies below
-- EXISTS over it to answer "is this user in this game".
CREATE TABLE player_hands (
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  player_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  joined_at TIMESTAMP DEFAULT NOW(),
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW(),
  PRIMARY KEY (game_id, player_id) -- One hand per player per game
);
COMMENT ON TABLE player_hands IS
  'Membership: which humans are in which game. Named for the hands it used to carry; the dealt state is games.state. The realtime RLS policies EXISTS over this table, so it is load-bearing even though it holds no cards.';

-- Chat messages table
CREATE TABLE chat_messages (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  message TEXT NOT NULL,
  is_system BOOLEAN NOT NULL DEFAULT false,
  created_at TIMESTAMP DEFAULT NOW()
);

-- User ELO ratings table. Also carries the (immutable) username so the
-- publicly-readable rating rows can be rendered as a leaderboard without
-- touching auth.users — see migration 20260702090000_leaderboard_usernames.
CREATE TABLE user_elo_ratings (
  user_id UUID PRIMARY KEY REFERENCES auth.users(id) ON DELETE CASCADE,
  username TEXT,
  elo_rating INTEGER NOT NULL DEFAULT 1000,
  previous_elo INTEGER NOT NULL DEFAULT 1000,
  games_played INTEGER NOT NULL DEFAULT 0,
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW()
);

-- Bots table - AI players with strategies
CREATE TABLE bots (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  nickname TEXT NOT NULL,
  strategy_key TEXT NOT NULL,
  elo_rating INTEGER NOT NULL DEFAULT 1000,
  previous_elo INTEGER NOT NULL DEFAULT 1000,
  games_played INTEGER NOT NULL DEFAULT 0,
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW()
);

-- Bot hands table - SENSITIVE: Only edge functions can access (similar to player_hands)
-- MEMBERSHIP: which bots are in which game. See player_hands.
CREATE TABLE bot_hands (
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  bot_id UUID NOT NULL REFERENCES bots(id) ON DELETE CASCADE,
  joined_at TIMESTAMP DEFAULT NOW(),
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW(),
  PRIMARY KEY (game_id, bot_id) -- One hand per bot per game
);
COMMENT ON TABLE bot_hands IS
  'Membership: which bots are in which game. See player_hands.';

-- NOTE: game_locks and bot_locks are GONE. Concurrency is now handled by
-- games.version (optimistic CAS via the commit_game RPC) and the games.bot_lease_*
-- columns (auto-expiring bot-loop lease). See migration 20260616030000.

-- (auto_discard_locks removed — it backed the 60s all-good auto-discard, which
-- is disabled in actions/good.ts; see migration 20260702090000.)

-- (The game_logs table was dropped in migration 20260708120000. The session log
-- is stored as packed bytes in games.logs_packed — bot belief imports and the
-- replay snapshot both read it from there. Nothing writes per-record log rows
-- anymore.)

-- Game snapshots - one row per finished session: the complete game compressed
-- by functions/_shared/common/replay/, stored as raw binary. `moves` is the rANS
-- move integer (decodes to the full game); `extras` is the optional names +
-- timing blob. The share code is derived: base32(moves) + '-' + base32(extras)
-- — the moves-only code is just the first part. Replaces the session's
-- packed session log (games.logs_packed), which is cleared after the snapshot
-- is verified and stored.
-- player_ids doubles as the read ACL and records seat order. game_id is
-- SET NULL on delete so replays outlive lobby deletion.
CREATE TABLE game_snapshots (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  game_id TEXT REFERENCES games(id) ON DELETE SET NULL,
  player_ids JSONB NOT NULL DEFAULT '[]'::jsonb, -- player ids in seat order
  moves BYTEA NOT NULL,
  extras BYTEA,
  created_at TIMESTAMP DEFAULT NOW()
);

-- Player views - a server-written, client-read PERSONALIZED VIEW CACHE
-- (docs/PLAYER_VIEWS.md). One row per (game, human participant): `view` is that
-- player's ALREADY-MASKED packed single-game envelope (the same bytes the
-- get_game / get_my_games edge functions emit, decodable by the shared
-- decodePackedGame), so RLS `player_id = auth.uid()` is SUFFICIENT — the row is
-- pre-masked, nothing to hide on read. This lets the client load its dashboard
-- list as a plain indexed SELECT (no edge round-trip) and get live pushes over
-- Realtime. Written ONLY by the service role, inside commit_game / create_game's
-- version-fenced transaction, so the cache can never be torn from games.state.
-- The `view` blob is safe to expose (masked for its owner); it must NEVER carry
-- the raw games.state.
CREATE TABLE player_views (
  game_id    TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  player_id  UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  view       TEXT NOT NULL,            -- MASKED packed view envelope (bare hex), decodable by decodePackedGame
  version    BIGINT NOT NULL,          -- mirrors games.version (optimistic token); client drops stale/reordered
  status     TEXT NOT NULL,            -- denormalized game_status for cheap list filtering/rendering
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (game_id, player_id)
);

-- Spectator views — the SHARED, fully-masked (seat -1) view of a game, readable
-- by ANY authenticated user (docs/PLAYER_VIEWS.md). This replaces get_game's
-- spectate path: a non-participant has no player_views row (that table is keyed
-- by auth.uid()), so spectators read their initial snapshot here and get live
-- updates over the RLS-guarded game-<id> broadcast. `view` is fully masked
-- (every hand a card-back, deck order hidden), so exposing it to all
-- authenticated users is safe — it must NEVER carry the raw games.state. One row
-- per game, written by the service role in commit_game / create_game's version
-- fence, alongside the per-player rows.
CREATE TABLE spectator_views (
  game_id    TEXT PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE,
  view       TEXT NOT NULL,            -- fully-masked packed spectator envelope (bare hex), decodable by decodePackedGame
  version    BIGINT NOT NULL,          -- mirrors games.version
  status     TEXT NOT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- =============================================================================
-- INDEXES: Create indexes for better performance
-- =============================================================================

CREATE INDEX idx_games_status ON games(status);
CREATE INDEX idx_games_name ON games(name);
CREATE INDEX idx_player_hands_game_id ON player_hands(game_id);
CREATE INDEX idx_player_hands_player_id ON player_hands(player_id);
CREATE INDEX idx_chat_messages_game_id ON chat_messages(game_id);
CREATE INDEX idx_chat_messages_user_id ON chat_messages(user_id);
CREATE INDEX idx_chat_messages_created_at ON chat_messages(created_at);
CREATE INDEX idx_games_updated_at ON games(updated_at);
-- bot-heartbeat SCAN (every 10s): status='playing' + updated_at window
CREATE INDEX idx_games_playing_updated_at ON games(updated_at) WHERE status = 'playing';
-- the bot scan once the heartbeat reads the kernel's needs_bots (Phase 4b)
CREATE INDEX idx_games_bot_scan ON games(updated_at) WHERE needs_bots;
CREATE INDEX idx_user_elo_ratings_user_id ON user_elo_ratings(user_id);
CREATE INDEX idx_user_elo_ratings_elo_rating ON user_elo_ratings(elo_rating);
CREATE INDEX idx_bots_strategy_key ON bots(strategy_key);
CREATE INDEX idx_bots_elo_rating ON bots(elo_rating);
CREATE INDEX idx_bot_hands_game_id ON bot_hands(game_id);
CREATE INDEX idx_bot_hands_bot_id ON bot_hands(bot_id);
CREATE INDEX idx_game_snapshots_game_id ON game_snapshots(game_id);
CREATE INDEX idx_game_snapshots_created_at ON game_snapshots(created_at);
CREATE INDEX idx_game_snapshots_player_ids ON game_snapshots USING GIN (player_ids);
-- dashboard list read: the caller's rows, newest game first (see get_my_games /
-- the client's direct player_views SELECT).
CREATE INDEX idx_player_views_player ON player_views(player_id, updated_at DESC);

-- =============================================================================
-- ROW LEVEL SECURITY: Enable RLS on all tables
-- =============================================================================

ALTER TABLE games ENABLE ROW LEVEL SECURITY;
ALTER TABLE player_hands ENABLE ROW LEVEL SECURITY;
ALTER TABLE chat_messages ENABLE ROW LEVEL SECURITY;
ALTER TABLE user_elo_ratings ENABLE ROW LEVEL SECURITY;
ALTER TABLE bots ENABLE ROW LEVEL SECURITY;
ALTER TABLE bot_hands ENABLE ROW LEVEL SECURITY;
ALTER TABLE game_snapshots ENABLE ROW LEVEL SECURITY;
ALTER TABLE player_views ENABLE ROW LEVEL SECURITY;
ALTER TABLE spectator_views ENABLE ROW LEVEL SECURITY;

-- =============================================================================
-- RLS POLICIES: Security-first approach
-- =============================================================================

-- Games: Anyone can view games (PUBLIC DATA ONLY - no sensitive info)
-- This allows users to join games or spectate without being in the game first
CREATE POLICY "Anyone can view games" ON games
  FOR SELECT USING (true);

-- ...but NOT the packed kernel state blob: games.state is the UNMASKED
-- volatile state (every hand + the deck order). Clients receive a
-- per-viewer MASKED view through the get_game edge function instead
-- (docs/PACKED_WIRE_CUTOVER.md); column-level grants keep the blob (and
-- the bot-lease bookkeeping) service-role-only, since RLS cannot hide a
-- column. Mirrors migration 20260707140000_hide_state_blob.sql.
REVOKE SELECT ON public.games FROM anon, authenticated;
GRANT SELECT (
  id, name, deck_length, discard_pile_length, flipped, players, status,
  power_suit, first_attacker, defender, table_battles, elimination_order,
  good_timestamp, good_players, version, created_at, updated_at
) ON public.games TO anon, authenticated;

-- No client INSERT policy: games are created by the create edge function
-- through the service-role create_game RPC. The old "Authenticated users can
-- create games" policy let any signed-in user write a row whose players and
-- state blob they chose (migration 20260917000000).

-- Player Hands: Players can ONLY see their own hands
CREATE POLICY "Player hands select policy" ON player_hands
  FOR SELECT USING (
    player_id = (select auth.uid()) OR 
    (select auth.role()) = 'service_role'
  );

CREATE POLICY "Player hands insert policy" ON player_hands
  FOR INSERT WITH CHECK (
    (select auth.role()) = 'service_role'
  );

CREATE POLICY "Player hands update policy" ON player_hands
  FOR UPDATE USING ((select auth.role()) = 'service_role');

CREATE POLICY "Player hands delete policy" ON player_hands
  FOR DELETE USING (
    (select auth.role()) = 'service_role'
  );

-- Chat messages: Players can view messages for games they're in
CREATE POLICY "Players can view chat messages for their games" ON chat_messages
  FOR SELECT USING (
    game_id IN (
      SELECT game_id FROM player_hands 
      WHERE player_id = (select auth.uid())
    )
  );

CREATE POLICY "Players can send chat messages to their games" ON chat_messages
  FOR INSERT WITH CHECK (
    game_id IN (
      SELECT game_id FROM player_hands 
      WHERE player_id = (select auth.uid())
    ) AND user_id = (select auth.uid())
  );

-- User ELO ratings: Read-only for authenticated users
CREATE POLICY "ELO ratings access policy" ON user_elo_ratings
  FOR SELECT USING (true);

CREATE POLICY "Only service role can modify ELO ratings" ON user_elo_ratings
  FOR INSERT WITH CHECK ((select auth.role()) = 'service_role');

CREATE POLICY "Only service role can update ELO ratings" ON user_elo_ratings
  FOR UPDATE USING ((select auth.role()) = 'service_role');

CREATE POLICY "Only service role can delete ELO ratings" ON user_elo_ratings
  FOR DELETE USING ((select auth.role()) = 'service_role');

-- Bots: Read-only for authenticated users (for lobby bot selection)
CREATE POLICY "Bots access policy" ON bots
  FOR SELECT USING (true);

CREATE POLICY "Only service role can modify bots" ON bots
  FOR INSERT WITH CHECK ((select auth.role()) = 'service_role');

CREATE POLICY "Only service role can update bots" ON bots
  FOR UPDATE USING ((select auth.role()) = 'service_role');

CREATE POLICY "Only service role can delete bots" ON bots
  FOR DELETE USING ((select auth.role()) = 'service_role');

-- Bot hands: ONLY service role can access (edge functions only)
CREATE POLICY "Only service role can access bot hands" ON bot_hands
  FOR ALL USING ((select auth.role()) = 'service_role');


-- Game snapshots: written by edge functions at game end; readable by the
-- players who were in that game (player_ids holds their auth uids in seat
-- order). The replay itself is shared by URL, so this only gates lookup.
CREATE POLICY "Service role can insert snapshots" ON game_snapshots
  FOR INSERT WITH CHECK ((select auth.role()) = 'service_role');

CREATE POLICY "Participants can read snapshots" ON game_snapshots
  FOR SELECT USING (
    (select auth.role()) = 'service_role'
    OR player_ids ? (select auth.uid())::text
  );

-- Player views: read ONLY your own rows; the blob is already masked for its
-- owner, so this simple, auditable policy is the whole personalization boundary
-- (docs/PLAYER_VIEWS.md). NO client writes — there is deliberately no
-- INSERT/UPDATE/DELETE policy, so authenticated can only SELECT; the service
-- role (which bypasses RLS) is the sole writer, via commit_game / create_game.
CREATE POLICY "Players can read their own views" ON player_views
  FOR SELECT USING (player_id = (select auth.uid()));

-- RLS gates ROWS; the table privilege must also be granted. SELECT only — no
-- INSERT/UPDATE/DELETE grant to client roles.
GRANT SELECT ON public.player_views TO authenticated;

-- Spectator views: the row is FULLY masked (seat -1), so ANY authenticated user
-- may read it — this is exactly the spectate case (a non-participant viewing a
-- game). Mirrors the game-<id> broadcast policy (authenticated-only). No client
-- writes (service role only).
CREATE POLICY "Authenticated can read spectator views" ON spectator_views
  FOR SELECT USING ((select auth.role()) = 'authenticated');
GRANT SELECT ON public.spectator_views TO authenticated;

-- Table writes: service_role only. Supabase grants ALL on every public table to
-- anon and authenticated, so without this RLS is the only barrier, and a policy
-- that is missing or wrong (like the old games INSERT policy) is a hole. The one
-- write a client makes on purpose is sending chat, gated by the chat_messages
-- INSERT policy above, so it is granted back. SELECT is untouched. Must run after
-- every CREATE TABLE. Mirrors migration 20260917000000 and is asserted under the
-- platform defaults by e2e/db_migration_grants.test.ts.
DO $$
DECLARE
  rel regclass;
BEGIN
  FOR rel IN
    SELECT c.oid::regclass
    FROM pg_class c
    JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'public'
      AND c.relkind IN ('r', 'p', 'v', 'm', 'f')
  LOOP
    EXECUTE format(
      'REVOKE INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER ON %s FROM PUBLIC, anon, authenticated;',
      rel);
  END LOOP;
END $$;

GRANT INSERT ON public.chat_messages TO authenticated;

-- Realtime: publish player_views so the client can subscribe to its own rows
-- (RLS-enforced) and receive live view pushes on every commit. Guarded: the
-- supabase_realtime publication exists on the Supabase platform but not in the
-- bare-Postgres e2e harness (e2e/schema.sql), where this is a harmless no-op.
DO $$ BEGIN
  ALTER PUBLICATION supabase_realtime ADD TABLE public.player_views;
EXCEPTION
  WHEN undefined_object THEN NULL; -- no such publication (e2e harness): skip
  WHEN duplicate_object THEN NULL; -- already in the publication: idempotent
END $$;

-- =============================================================================
-- FUNCTIONS: Helper functions and triggers
-- =============================================================================

-- Functions for automatic updated_at timestamps
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER 
SET search_path = ''
AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- Function to broadcast chat message changes
CREATE OR REPLACE FUNCTION public.chat_messages_changes()
RETURNS TRIGGER
SECURITY DEFINER
SET search_path = ''
LANGUAGE plpgsql
AS $$
BEGIN
  -- Broadcast to game-specific topic for chat messages
  PERFORM realtime.broadcast_changes(
    'chat:' || COALESCE(NEW.game_id, OLD.game_id)::text, 
    TG_OP,
    TG_OP,
    TG_TABLE_NAME,
    TG_TABLE_SCHEMA,
    NEW,
    OLD
  );
  RETURN COALESCE(NEW, OLD);
END;
$$;

-- Function to create default ELO rating for new users
CREATE OR REPLACE FUNCTION public.create_default_elo_rating()
RETURNS TRIGGER
SECURITY DEFINER
SET search_path = ''
LANGUAGE plpgsql
AS $$
BEGIN
  INSERT INTO public.user_elo_ratings (user_id, elo_rating, games_played, username)
  VALUES (NEW.id, 1000, 0, NEW.raw_user_meta_data->>'username')
  ON CONFLICT (user_id) DO UPDATE SET username = EXCLUDED.username;
  RETURN NEW;
END;
$$;

-- =============================================================================
-- TRIGGERS: Set up automatic triggers
-- =============================================================================

CREATE TRIGGER update_games_updated_at 
  BEFORE UPDATE ON games
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();


CREATE TRIGGER update_player_hands_updated_at 
  BEFORE UPDATE ON player_hands
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_user_elo_ratings_updated_at 
  BEFORE UPDATE ON user_elo_ratings
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_bots_updated_at 
  BEFORE UPDATE ON bots
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_bot_hands_updated_at 
  BEFORE UPDATE ON bot_hands
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();

-- Trigger for chat message changes
CREATE TRIGGER handle_chat_messages_changes
  AFTER INSERT OR UPDATE OR DELETE
  ON public.chat_messages
  FOR EACH ROW
  EXECUTE FUNCTION chat_messages_changes();

-- Trigger to create default ELO rating for new users. Also fires on metadata
-- UPDATE: the app has no rename flow, but GoTrue's updateUser (and the admin
-- dashboard) can change raw_user_meta_data, and the denormalized
-- user_elo_ratings.username copy must follow.
DROP TRIGGER IF EXISTS handle_new_user_elo_rating ON auth.users;
CREATE TRIGGER handle_new_user_elo_rating
  AFTER INSERT OR UPDATE OF raw_user_meta_data
  ON auth.users
  FOR EACH ROW
  EXECUTE FUNCTION create_default_elo_rating();

-- Reserve the bot-name prefix. Replay codes encode only the player NAME (not the
-- is_ai flag), so bot-vs-human must be recoverable from the name alone. Bots are
-- named with a leading '%'; humans may not use it anywhere in their username. A
-- single-byte ASCII prefix keeps the game_snapshots.extras blob tiny. This trigger
-- is the AUTHORITATIVE guard (the client-side check in AuthContext is only for
-- fast UX and is bypassable). NOTE: position() is a LITERAL substring search, so
-- '%' here is just the character, not a LIKE wildcard.
CREATE OR REPLACE FUNCTION public.enforce_username_not_bot()
RETURNS TRIGGER
SECURITY DEFINER
SET search_path = ''
LANGUAGE plpgsql
AS $$
BEGIN
  IF position('%' in coalesce(NEW.raw_user_meta_data->>'username', '')) > 0 THEN
    RAISE EXCEPTION 'username may not contain the reserved bot prefix (%%)'
      USING ERRCODE = 'check_violation';
  END IF;
  RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS enforce_username_not_bot ON auth.users;
CREATE TRIGGER enforce_username_not_bot
  BEFORE INSERT OR UPDATE ON auth.users
  FOR EACH ROW
  EXECUTE FUNCTION public.enforce_username_not_bot();

-- =============================================================================
-- THE JSONB -> KERNEL BRIDGE (docs/C_GAME_SHAPE_MIGRATION.md 3.4). Mirrors
-- migration 20260917140000_table_expand.sql; dropped by the contract migration.
-- While the JSONB writers (commit_game, create_game, delete_account) own a row,
-- a trigger keeps the kernel's durable roster, needs_bots and the lobby state
-- blob in step with `players`, so every row always carries both blobs.
-- =============================================================================

-- The longest prefix of whole UTF-8 scalars that fits p_max bytes: roster.c
-- roster_name_trim. The byte AT the budget is either the lead byte of the scalar
-- that would cross it (cut there) or inside that scalar (back off to its lead).
CREATE OR REPLACE FUNCTION legacy_utf8_cut(p BYTEA, p_max INT)
RETURNS BYTEA
LANGUAGE plpgsql
IMMUTABLE
SET search_path = public
AS $$
DECLARE
  k INT := p_max;
BEGIN
  IF length(p) <= p_max THEN
    RETURN p;
  END IF;
  WHILE k > 0 AND (get_byte(p, k) & 192) = 128 LOOP
    k := k - 1;
  END LOOP;
  RETURN substring(p FROM 1 FOR k);
END;
$$;

-- One durable field: a length byte, the bytes, zero padding to p_cap.
CREATE OR REPLACE FUNCTION legacy_roster_field(p BYTEA, p_cap INT, p_what TEXT)
RETURNS BYTEA
LANGUAGE plpgsql
IMMUTABLE
SET search_path = public
AS $$
BEGIN
  IF length(p) > p_cap THEN
    RAISE EXCEPTION 'legacy roster: % is % bytes, over the kernel''s cap of %', p_what, length(p), p_cap
      USING ERRCODE = 'string_data_right_truncation';
  END IF;
  RETURN set_byte('\x00'::bytea, 0, length(p)) || p || decode(repeat('00', p_cap - length(p)), 'hex');
END;
$$;

-- The durable roster (c/src/roster.h) of a JSONB players array and a title:
--
--   0    1        format = 1
--   1    1        n seats (0..8)
--   2    1+200    title: length byte, bytes, zero padded
--   203  8 x 128  seats, unused ones all zero:
--                   +0   1+36  id
--                   +37  1+64  name (UTF-8)
--                   +102 1+23  brain: bots.strategy_key for an is_ai seat, empty for a human
--                   +126 2     reserved, zero
--
-- Refuses, never clamps, unless p_trim: then a name over 64 bytes and a title
-- over 200 bytes are cut on a scalar boundary exactly as the kernel's own
-- writers cut them (roster_seat_add, table_create). Only the live trigger
-- trims, so a new long username can still join during the bridge; the backfill
-- of rows that already exist does not.
CREATE OR REPLACE FUNCTION legacy_roster_hex(p_players JSONB, p_title TEXT, p_trim BOOLEAN DEFAULT FALSE)
RETURNS TEXT
LANGUAGE plpgsql
STABLE
SET search_path = public
AS $$
DECLARE
  c_suffix CONSTANT BYTEA := convert_to('''s Game', 'UTF8');
  v_title BYTEA := convert_to(COALESCE(p_title, ''), 'UTF8');
  v_n     INT;
  v_out   BYTEA;
  v_seat  JSONB;
  v_id    TEXT;
  v_name  BYTEA;
  v_brain TEXT;
  v_ids   TEXT[] := '{}';
BEGIN
  IF jsonb_typeof(p_players) IS DISTINCT FROM 'array' THEN
    RAISE EXCEPTION 'legacy roster: players is not a JSON array' USING ERRCODE = 'data_exception';
  END IF;
  v_n := jsonb_array_length(p_players);
  IF v_n > 8 THEN
    RAISE EXCEPTION 'legacy roster: % seats, over the kernel''s 8', v_n USING ERRCODE = 'data_exception';
  END IF;

  IF p_trim AND length(v_title) > 200 THEN
    -- table_create's rule: the creator's name cut to fit, then "'s Game".
    IF substring(v_title FROM length(v_title) - length(c_suffix) + 1) = c_suffix THEN
      v_title := legacy_utf8_cut(substring(v_title FROM 1 FOR length(v_title) - length(c_suffix)),
                                 200 - length(c_suffix)) || c_suffix;
    ELSE
      v_title := legacy_utf8_cut(v_title, 200);
    END IF;
  END IF;

  v_out := '\x01'::bytea || set_byte('\x00'::bytea, 0, v_n) || legacy_roster_field(v_title, 200, 'the title');

  FOR v_seat IN SELECT e FROM jsonb_array_elements(p_players) WITH ORDINALITY AS t(e, k) ORDER BY k LOOP
    v_id := v_seat->>'player_id';
    IF v_id IS NULL OR v_id = '' THEN
      RAISE EXCEPTION 'legacy roster: a seat has no player_id' USING ERRCODE = 'data_exception';
    END IF;
    IF v_id = ANY(v_ids) THEN
      RAISE EXCEPTION 'legacy roster: player % is seated twice', v_id USING ERRCODE = 'data_exception';
    END IF;
    v_ids := v_ids || v_id;
    IF jsonb_typeof(v_seat->'name') IS DISTINCT FROM 'string' THEN
      RAISE EXCEPTION 'legacy roster: seat % has no name', v_id USING ERRCODE = 'data_exception';
    END IF;
    v_name := convert_to(v_seat->>'name', 'UTF8');
    IF p_trim THEN
      v_name := legacy_utf8_cut(v_name, 64);
    END IF;

    IF jsonb_typeof(v_seat->'is_ai') IS DISTINCT FROM 'boolean' THEN
      RAISE EXCEPTION 'legacy roster: seat % has no is_ai flag', v_id USING ERRCODE = 'data_exception';
    ELSIF (v_seat->>'is_ai')::boolean THEN
      SELECT b.strategy_key INTO v_brain FROM bots b WHERE b.id::text = v_id;
      -- A brain is 1..23 bytes of printable ASCII (roster.c brain_ok). Whether
      -- this build links it is table_load's question (Q16), not SQL's.
      IF v_brain IS NULL OR v_brain !~ '^[!-~]{1,23}$' THEN
        RAISE EXCEPTION 'legacy roster: bot seat % has no bots row with a valid strategy_key (%)', v_id, v_brain
          USING ERRCODE = 'data_exception';
      END IF;
    ELSE
      v_brain := '';
    END IF;

    v_out := v_out
      || legacy_roster_field(convert_to(v_id, 'UTF8'), 36, format('seat %s''s id', v_id))
      || legacy_roster_field(v_name, 64, format('seat %s''s name', v_id))
      || legacy_roster_field(convert_to(v_brain, 'UTF8'), 23, format('seat %s''s brain', v_id))
      || '\x0000'::bytea;
  END LOOP;

  v_out := v_out || decode(repeat('00', 128 * (8 - v_n)), 'hex');
  IF length(v_out) <> 1227 THEN
    RAISE EXCEPTION 'legacy roster: encoded % bytes, not 1227', length(v_out);
  END IF;
  RETURN E'\\x' || encode(v_out, 'hex');
END;
$$;

-- The durable state blob of a lobby with these seats: [format 02][deterministic
-- deck 00] then view.c state_put, whose every count is zero in a lobby, so the
-- offsets are fixed:
--
--   status 00, n, power_suit 00, first_attacker 00, defender 00, discard 0000,
--   has_flipped 00, flip FE, good mask 00000000, has_ts 00, deck 0000, battles 00,
--   n x {status (idle 00, ready 01, in 02, out 03), awaiting 00, hand 00},
--   eliminated 00
--
-- No goods, no timestamp, no cards: GAME_INVALID_LOBBY_CARDS refuses any of them
-- in WAITING, so whatever a JSONB row still says about them is not carried.
CREATE OR REPLACE FUNCTION legacy_lobby_state_hex(p_players JSONB)
RETURNS TEXT
LANGUAGE plpgsql
IMMUTABLE
SET search_path = public
AS $$
DECLARE
  v_n     INT;
  v_seats  BYTEA := '\x'::bytea;
  v_seat   JSONB;
  v_status INT;
BEGIN
  IF jsonb_typeof(p_players) IS DISTINCT FROM 'array' THEN
    RAISE EXCEPTION 'legacy lobby state: players is not a JSON array' USING ERRCODE = 'data_exception';
  END IF;
  v_n := jsonb_array_length(p_players);
  IF v_n > 8 THEN
    RAISE EXCEPTION 'legacy lobby state: % seats, over the kernel''s 8', v_n USING ERRCODE = 'data_exception';
  END IF;
  FOR v_seat IN SELECT e FROM jsonb_array_elements(p_players) WITH ORDINALITY AS t(e, k) ORDER BY k LOOP
    v_status := array_position(ARRAY['idle', 'ready', 'in', 'out'], v_seat->>'status');
    IF v_status IS NULL THEN
      RAISE EXCEPTION 'legacy lobby state: seat % has status %', v_seat->>'player_id', v_seat->>'status'
        USING ERRCODE = 'data_exception';
    END IF;
    v_seats := v_seats || set_byte('\x000000'::bytea, 0, v_status - 1);
  END LOOP;
  RETURN E'\\x' || encode(
    '\x020000'::bytea || set_byte('\x00'::bytea, 0, v_n)
      || '\x0000000000' || '\x00fe' || '\x00000000' || '\x00' || '\x0000' || '\x00'
      || v_seats || '\x00'::bytea,
    'hex');
END;
$$;

-- Fires on every INSERT, and on every UPDATE that sets players, name or status
-- (commit_game sets all three on every commit; the lease RPCs set none). A row
-- the kernel writers own (writer_gen 2) is theirs: nothing is derived.
CREATE OR REPLACE FUNCTION legacy_games_bridge()
RETURNS TRIGGER
LANGUAGE plpgsql
SET search_path = public
AS $$
BEGIN
  IF NEW.writer_gen <> 1 THEN
    RETURN NEW;
  END IF;
  NEW.roster := legacy_roster_hex(NEW.players, NEW.name, TRUE);
  NEW.needs_bots := NEW.status = 'playing' AND NEW.players @> '[{"is_ai": true, "status": "in"}]';
  -- Every row has both blobs, a lobby included. commit_game still writes NULL
  -- for a WAITING commit (so a finished session's blob never survives into the
  -- lobby); the lobby blob replaces it here. The JSONB board of a lobby is
  -- emptied the way game_reset_to_lobby empties the kernel's: today's server
  -- marshals it into the kernel for every lobby view, and the kernel refuses a
  -- lobby holding goods, a trump, a discard pile, a table or an elimination.
  IF NEW.status = 'waiting' THEN
    NEW.state := legacy_lobby_state_hex(NEW.players);
    NEW.deck_length := 0;
    NEW.discard_pile_length := 0;
    NEW.flipped := NULL;
    NEW.power_suit := 0;
    NEW.first_attacker := 0;
    NEW.defender := 0;
    NEW.table_battles := '[]';
    NEW.elimination_order := '[]';
    NEW.good_players := '[]';
    NEW.good_timestamp := NULL;
  END IF;
  RETURN NEW;
END;
$$;

CREATE TRIGGER games_legacy_bridge
  BEFORE INSERT OR UPDATE OF players, name, status ON games
  FOR EACH ROW
  EXECUTE FUNCTION legacy_games_bridge();

-- =============================================================================
-- CONCURRENCY RPCs (replace game_locks + bot_locks). See migration
-- 20260616030000_cas_concurrency.sql for the rationale. Kept identical here.
-- =============================================================================

-- Atomic, version-gated commit of the whole game state in one transaction.
CREATE OR REPLACE FUNCTION commit_game(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_game             JSONB,
  p_seats            JSONB   DEFAULT NULL,  -- human members ["uuid",...]; NULL leaves player_hands untouched (a dealt commit cannot change the roster)
  p_bot_seats        JSONB   DEFAULT NULL,  -- bot members ["uuid",...]; NULL leaves bot_hands untouched, [] prunes every bot
  p_state            TEXT    DEFAULT NULL,  -- packed kernel state blob (hex); NULL leaves the column unchanged (never-dealt games)
  p_logs_packed      TEXT    DEFAULT NULL,  -- this move's logwire records (bare hex), appended under the version fence
  p_logs_reset       BOOLEAN DEFAULT FALSE, -- session reset (GAME_START in the records): replace instead of append
  p_game_seed        TEXT    DEFAULT NULL,  -- deal seed (hex); set once at the deal, NULL on every other commit leaves it unchanged
  p_views            JSONB   DEFAULT NULL,  -- per-participant masked view cache rows [{player_id,view,status}]; NULL leaves player_views untouched
  p_spectator        TEXT    DEFAULT NULL,  -- fully-masked seat -1 spectator view (bare hex); NULL leaves spectator_views untouched
  p_closed_round     BOOLEAN DEFAULT FALSE  -- TRUE when THIS move closed a round (pickup/discard): stamp round_epoch with the new version so the round-boundary guard rejects moves composed against the prior round (REJECT_STALE_ROUND, docs/WEB_RACE_BUG_HANDOFF.md)
) RETURNS JSONB
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_new_version BIGINT;
  v_round_epoch BIGINT;
  g games%ROWTYPE;
BEGIN
  g := jsonb_populate_record(NULL::games, p_game);

  UPDATE games SET
    name = g.name, deck_length = g.deck_length, discard_pile_length = g.discard_pile_length,
    flipped = g.flipped, players = g.players, status = g.status, power_suit = g.power_suit,
    first_attacker = g.first_attacker, defender = g.defender, table_battles = g.table_battles,
    elimination_order = g.elimination_order, good_timestamp = g.good_timestamp,
    good_players = g.good_players,
    -- A WAITING commit is the `continue` reset (or a lobby op): the finished
    -- session's volatile state must NOT survive into the new lobby. A stale
    -- blob desyncs from the mutable lobby roster (seat-count mismatches brick
    -- every subsequent load) and leaks the previous session's hands through
    -- the blob-authoritative loaders. COALESCE alone never cleared it.
    -- The bridge trigger then writes the lobby blob from g.players in its place.
    state = CASE WHEN g.status = 'waiting' THEN NULL ELSE COALESCE(p_state, state) END,
    -- Same discipline as `state`: a WAITING reset clears the finished session's
    -- seed so the next deal writes a fresh one; every dealt commit sets it (deal)
    -- or leaves it (COALESCE with NULL).
    game_seed = CASE WHEN g.status = 'waiting' THEN NULL ELSE COALESCE(p_game_seed, game_seed) END,
    logs_packed = CASE
      WHEN p_logs_reset THEN COALESCE(p_logs_packed, '')
      WHEN g.status = 'waiting' THEN ''
      ELSE COALESCE(logs_packed, '') || COALESCE(p_logs_packed, '')
    END,
    -- round_epoch = the version at which the CURRENT round began. A new session
    -- (WAITING reset, or a GAME_START in the appended records) restarts it at 0;
    -- a round-closing move (p_closed_round) stamps it with the version this very
    -- commit produces (version + 1, since `version` here is the OLD value);
    -- every other move leaves it untouched. Same version fence as the state
    -- blob, so a client's intent version can be compared to it without a torn read.
    round_epoch = CASE
      WHEN p_logs_reset OR g.status = 'waiting' THEN 0
      WHEN p_closed_round THEN version + 1
      ELSE round_epoch
    END,
    updated_at = now(), version = version + 1
  -- writer_gen = 1: a row the kernel writers own (commit_table) is not this
  -- function's to write. Missing it is an error, not a conflict: a conflict
  -- would reload JSONB the kernel writers no longer keep, and retry forever.
  WHERE id = p_game_id AND version = p_expected_version AND writer_gen = 1
  RETURNING version, round_epoch INTO v_new_version, v_round_epoch;

  IF NOT FOUND THEN
    IF EXISTS (SELECT 1 FROM games WHERE id = p_game_id AND writer_gen <> 1) THEN
      RAISE EXCEPTION 'commit_game: game % is owned by the table writers (writer_gen 2); reload it through them', p_game_id
        USING ERRCODE = 'object_not_in_prerequisite_state';
    END IF;
    RETURN jsonb_build_object('status', 'conflict');
  END IF;

  -- MEMBERSHIP ONLY. These tables used to carry the hands themselves; the dealt
  -- state is games.state and has been since the blob landed, so all that is
  -- left here is who is in the game. player_hands is what the realtime RLS
  -- policies EXISTS over, and the bot prune is how removing a lobby bot takes
  -- effect without a second round-trip.
  IF p_seats IS NOT NULL THEN
    INSERT INTO player_hands (game_id, player_id)
    SELECT p_game_id, s::uuid FROM jsonb_array_elements_text(p_seats) AS s
    ON CONFLICT (game_id, player_id) DO UPDATE SET updated_at = now();
  END IF;

  -- Guarded to lobby commits exactly as before: a dealt commit passes NULL (the
  -- roster cannot change mid-game), and an empty array prunes every bot because
  -- the last one was just removed.
  IF p_bot_seats IS NOT NULL THEN
    IF jsonb_array_length(p_bot_seats) > 0 THEN
      INSERT INTO bot_hands (game_id, bot_id)
      SELECT p_game_id, b::uuid FROM jsonb_array_elements_text(p_bot_seats) AS b
      ON CONFLICT (game_id, bot_id) DO UPDATE SET updated_at = now();
    END IF;

    DELETE FROM bot_hands
    WHERE game_id = p_game_id
      AND bot_id NOT IN (
        SELECT b::uuid FROM jsonb_array_elements_text(p_bot_seats) AS b
      );
  END IF;

  -- The session log is the packed logs_packed column (handled in the UPDATE
  -- above); per-record game_logs rows were retired in migration 20260708120000.

  -- player_views personalized-view cache (docs/PLAYER_VIEWS.md), written in THIS
  -- version-fenced transaction so it can never be torn from games.state. p_views
  -- is the CURRENT participants' masked rows; NULL means "leave the cache
  -- untouched" (a view-build failure upstream — the game still commits). An
  -- empty array means "no human participants" and correctly prunes every row.
  IF p_views IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    SELECT p_game_id, (v->>'player_id')::uuid, v->>'view', v_new_version, v->>'status', now()
    FROM jsonb_array_elements(p_views) AS v
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();

    -- Prune rows for players no longer in the game (exited / removed), so a
    -- participant who left stops seeing the game in their dashboard list.
    DELETE FROM player_views
    WHERE game_id = p_game_id
      AND player_id NOT IN (
        SELECT (v->>'player_id')::uuid FROM jsonb_array_elements(p_views) AS v
      );
  END IF;

  -- The shared spectator view (seat -1), same version fence. One row per game.
  IF p_spectator IS NOT NULL THEN
    INSERT INTO spectator_views (game_id, view, version, status, updated_at)
    VALUES (p_game_id, p_spectator, v_new_version, g.status, now())
    ON CONFLICT (game_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;

  RETURN jsonb_build_object('status', 'ok', 'version', v_new_version, 'round_epoch', v_round_epoch);
END;
$$;

-- create_game: both create-game inserts (games → player_hands membership)
-- in one transaction / one round-trip. See migration 20260618120000.
CREATE OR REPLACE FUNCTION create_game(
  p_game_id   TEXT,
  p_name      TEXT,
  p_player_id UUID,
  p_players   JSONB,
  p_views     JSONB DEFAULT NULL,  -- creator's masked view cache row(s); version 0
  p_spectator TEXT  DEFAULT NULL   -- fully-masked seat -1 spectator view (bare hex); version 0
) RETURNS VOID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  INSERT INTO games (id, name, players, status)
    VALUES (p_game_id, p_name, p_players, 'waiting');

  INSERT INTO player_hands (game_id, player_id)
    VALUES (p_game_id, p_player_id);

  -- Seed the player_views dashboard cache for the creator in the same
  -- transaction, so the new lobby is immediately readable from the client's
  -- direct player_views SELECT (docs/PLAYER_VIEWS.md). version 0 = the initial
  -- games.version.
  IF p_views IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    SELECT p_game_id, (v->>'player_id')::uuid, v->>'view', 0, v->>'status', now()
    FROM jsonb_array_elements(p_views) AS v
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;

  -- The shared spectator view (seat -1) for the new lobby.
  IF p_spectator IS NOT NULL THEN
    INSERT INTO spectator_views (game_id, view, version, status, updated_at)
    VALUES (p_game_id, p_spectator, 0, 'waiting', now())
    ON CONFLICT (game_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;
END;
$$;

-- The kernel writers (docs/C_GAME_SHAPE_MIGRATION.md 3.3), called from Phase 4b on.
-- Every product comes from one table_commit_products call: the state and roster
-- blobs, the status as GAME_STATUS_* (0 waiting, 1 playing, 2 game_over, the
-- enum's order), needs_bots, the log records, the views. The rules are
-- commit_game's (CAS on version, log append or reset, round_epoch, membership,
-- view cache), and a lobby commit's human list also prunes the humans who left,
-- in the same transaction as the roster that no longer seats them.
--
-- The blobs ride as base64 (measured through PostgREST: a third fewer body bytes
-- than hex, the same latency), and the columns stay hex TEXT until
-- 20260918130000_table_bytea.sql makes them BYTEA, so this body writes the text
-- forms the legacy writers write. The signature is final from here on: the
-- functions deployed between 4a and 4c already call it. The views are parallel
-- arrays with the game's status, the roster NULL when the operation left it alone,
-- and the result the OUT columns (committed, new_version, new_round_epoch).
CREATE OR REPLACE FUNCTION commit_table(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_state            TEXT,                  -- base64: the kernel's state blob
  p_status           SMALLINT,              -- GAME_STATUS_*: 0 waiting, 1 playing, 2 game_over (the enum's order)
  p_needs_bots       BOOLEAN,
  p_roster           TEXT    DEFAULT NULL,  -- base64: the durable roster; NULL keeps the stored one (the operation left it alone)
  p_seats            UUID[]  DEFAULT NULL,  -- human member ids; NULL leaves player_hands untouched
  p_bot_seats        UUID[]  DEFAULT NULL,  -- bot member ids; NULL leaves bot_hands untouched
  p_logs_packed      TEXT    DEFAULT NULL,  -- base64: this operation's log records, appended under the version fence
  p_logs_reset       BOOLEAN DEFAULT FALSE, -- the operation dealt: replace the session log instead of appending
  p_game_seed        TEXT    DEFAULT NULL,  -- deal seed (64 hex characters); NULL keeps the stored one
  p_view_players     UUID[]  DEFAULT NULL,  -- the human seats' ids, parallel to p_views; NULL leaves player_views untouched
  p_views            TEXT[]  DEFAULT NULL,  -- base64: each seat's envelope, parallel to p_view_players
  p_spectator        TEXT    DEFAULT NULL,  -- base64: the spectator envelope; NULL leaves spectator_views untouched
  p_closed_round     BOOLEAN DEFAULT FALSE, -- the operation closed a round: stamp round_epoch with the new version
  OUT committed       BOOLEAN,              -- FALSE: the version fence refused (another writer committed first)
  OUT new_version     BIGINT,
  OUT new_round_epoch BIGINT
)
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_status game_status := (enum_range(NULL::game_status))[p_status + 1];
BEGIN
  IF p_state IS NULL OR p_needs_bots IS NULL THEN
    RAISE EXCEPTION 'commit_table: state and needs_bots are required' USING ERRCODE = 'null_value_not_allowed';
  END IF;
  IF v_status IS NULL THEN
    RAISE EXCEPTION 'commit_table: % is not a GAME_STATUS', p_status USING ERRCODE = 'invalid_parameter_value';
  END IF;
  IF (p_view_players IS NULL) <> (p_views IS NULL) OR cardinality(p_view_players) <> cardinality(p_views) THEN
    RAISE EXCEPTION 'commit_table: p_view_players and p_views go together, one envelope per player' USING ERRCODE = 'invalid_parameter_value';
  END IF;

  UPDATE games SET
    status = v_status, state = '\x' || encode(decode(p_state, 'base64'), 'hex'), roster = COALESCE('\x' || encode(decode(p_roster, 'base64'), 'hex'), roster), needs_bots = p_needs_bots, writer_gen = 2,
    game_seed = CASE WHEN v_status = 'waiting' THEN NULL ELSE COALESCE(p_game_seed, game_seed) END,
    logs_packed = CASE
      WHEN p_logs_reset THEN COALESCE(encode(decode(p_logs_packed, 'base64'), 'hex'), '')
      WHEN v_status = 'waiting' THEN ''
      ELSE COALESCE(logs_packed, '') || COALESCE(encode(decode(p_logs_packed, 'base64'), 'hex'), '')
    END,
    round_epoch = CASE
      WHEN p_logs_reset OR v_status = 'waiting' THEN 0
      WHEN p_closed_round THEN version + 1
      ELSE round_epoch
    END,
    updated_at = now(), version = version + 1
  WHERE id = p_game_id AND version = p_expected_version
  RETURNING version, round_epoch INTO new_version, new_round_epoch;

  committed := FOUND;
  IF NOT committed THEN
    RETURN;
  END IF;

  IF p_seats IS NOT NULL THEN
    INSERT INTO player_hands (game_id, player_id)
    SELECT p_game_id, s FROM unnest(p_seats) AS s
    ON CONFLICT (game_id, player_id) DO UPDATE SET updated_at = now();
    DELETE FROM player_hands
    WHERE game_id = p_game_id AND player_id <> ALL (p_seats);
  END IF;

  IF p_bot_seats IS NOT NULL THEN
    INSERT INTO bot_hands (game_id, bot_id)
    SELECT p_game_id, b FROM unnest(p_bot_seats) AS b
    ON CONFLICT (game_id, bot_id) DO UPDATE SET updated_at = now();
    DELETE FROM bot_hands
    WHERE game_id = p_game_id AND bot_id <> ALL (p_bot_seats);
  END IF;

  IF p_views IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    SELECT p_game_id, v.player_id, encode(decode(v.view, 'base64'), 'hex'), new_version, v_status::text, now()
    FROM unnest(p_view_players, p_views) AS v(player_id, view)
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
    DELETE FROM player_views
    WHERE game_id = p_game_id AND player_id <> ALL (p_view_players);
  END IF;

  IF p_spectator IS NOT NULL THEN
    INSERT INTO spectator_views (game_id, view, version, status, updated_at)
    VALUES (p_game_id, encode(decode(p_spectator, 'base64'), 'hex'), new_version, v_status::text, now())
    ON CONFLICT (game_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;
END;
$$;

-- A new lobby from table_create's products, owned by the kernel writers.
CREATE OR REPLACE FUNCTION create_table(
  p_game_id   TEXT,
  p_player_id UUID,
  p_state     TEXT,               -- base64: the lobby's state blob
  p_roster    TEXT,               -- base64: the durable roster, the creator seated
  p_view      TEXT DEFAULT NULL,  -- base64: the creator's envelope; version 0
  p_spectator TEXT DEFAULT NULL   -- base64: the spectator envelope; version 0
) RETURNS VOID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  IF p_state IS NULL OR p_roster IS NULL THEN
    RAISE EXCEPTION 'create_table: state and roster are required' USING ERRCODE = 'null_value_not_allowed';
  END IF;

  INSERT INTO games (id, status, state, roster, needs_bots, writer_gen)
    VALUES (p_game_id, 'waiting', '\x' || encode(decode(p_state, 'base64'), 'hex'), '\x' || encode(decode(p_roster, 'base64'), 'hex'), FALSE, 2);

  INSERT INTO player_hands (game_id, player_id)
    VALUES (p_game_id, p_player_id);

  IF p_view IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    VALUES (p_game_id, p_player_id, encode(decode(p_view, 'base64'), 'hex'), 0, 'waiting', now())
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;

  IF p_spectator IS NOT NULL THEN
    INSERT INTO spectator_views (game_id, view, version, status, updated_at)
    VALUES (p_game_id, encode(decode(p_spectator, 'base64'), 'hex'), 0, 'waiting', now())
    ON CONFLICT (game_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;
END;
$$;

-- Account deletion (migration 20260714120000): redacts the leaving player's
-- display name from shared game rows (the bridge trigger re-derives the roster,
-- so the redaction reaches it too) and the denormalized leaderboard username.
-- Owned rows cascade from auth.users.
CREATE OR REPLACE FUNCTION public.delete_account(p_user_id UUID)
RETURNS VOID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  UPDATE games g
  SET players = (
    SELECT jsonb_agg(
      CASE
        WHEN elem->>'player_id' = p_user_id::text
          THEN jsonb_set(elem, '{name}', '"Deleted player"'::jsonb)
        ELSE elem
      END
    )
    FROM jsonb_array_elements(g.players) AS elem
  )
  WHERE g.players @> jsonb_build_array(jsonb_build_object('player_id', p_user_id::text));

  UPDATE public.user_elo_ratings
  SET username = NULL
  WHERE user_id = p_user_id;
END;
$$;

-- Bot-loop lease: atomic claim (NULL if another loop holds a live lease).
CREATE OR REPLACE FUNCTION try_acquire_bot_lease(p_game_id TEXT, p_ttl_ms INT)
RETURNS UUID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_token UUID;
BEGIN
  UPDATE games SET
    bot_lease_token = gen_random_uuid(),
    bot_lease_until = now() + make_interval(secs => p_ttl_ms / 1000.0)
  WHERE id = p_game_id
    AND (bot_lease_until IS NULL OR bot_lease_until < now())
  RETURNING bot_lease_token INTO v_token;
  RETURN v_token;
END;
$$;

CREATE OR REPLACE FUNCTION release_bot_lease(p_game_id TEXT, p_token UUID)
RETURNS VOID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  UPDATE games SET bot_lease_until = now() - interval '1 second'
  WHERE id = p_game_id AND bot_lease_token = p_token;
END;
$$;

-- Extend our lease each cycle so a long loop keeps a SHORT TTL (fast recovery).
CREATE OR REPLACE FUNCTION renew_bot_lease(p_game_id TEXT, p_token UUID, p_ttl_ms INT)
RETURNS BOOLEAN
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_rows INT;
BEGIN
  UPDATE games SET bot_lease_until = now() + make_interval(secs => p_ttl_ms / 1000.0)
  WHERE id = p_game_id AND bot_lease_token = p_token;
  GET DIAGNOSTICS v_rows = ROW_COUNT;
  RETURN v_rows > 0;
END;
$$;

-- SECURITY: every SECURITY DEFINER function in public is service_role only.
-- They bypass RLS by design (the edge functions call them with the service-role
-- key). Postgres grants EXECUTE to PUBLIC on every new function and Supabase's
-- default privileges add anon and authenticated, so without this anon (the role
-- behind the public anon key) could call them through PostgREST and rewrite any
-- game. No client calls any RPC. Revoking from PUBLIC alone is not enough, hence
-- all three. Not a list of names: a migration that DROPs and re-CREATEs one gets
-- fresh default grants, which is how commit_game reopened on the hosted project
-- (20260906120000, relocked by 20260917000000). Trigger functions are skipped;
-- PostgREST does not expose them. The bridge's legacy_* helpers are not
-- definers, but no client has a reason to call them, so they are included. Must
-- run after every definer function is created; a function a client is meant to call must be granted explicitly
-- after this. See migrations 20260807120000 and
-- 20260917000000, e2e/db_grants.test.ts and e2e/db_migration_grants.test.ts.
DO $$
DECLARE
  fn regprocedure;
BEGIN
  FOR fn IN
    SELECT p.oid::regprocedure
    FROM pg_proc p
    JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'public'
      AND p.prorettype <> 'trigger'::regtype
      AND (p.prosecdef OR p.proname LIKE 'legacy\_%')
  LOOP
    EXECUTE format('REVOKE ALL ON FUNCTION %s FROM PUBLIC, anon, authenticated;', fn);
    -- REVOKE FROM PUBLIC also strips service_role's implicit grant, so re-grant.
    EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO service_role;', fn);
  END LOOP;
END $$;

-- =============================================================================
-- REALTIME AUTHORIZATION POLICIES
-- Enable Supabase Realtime with proper security
-- =============================================================================

-- Enable RLS on the realtime messages table. Guarded: on the Supabase images
-- supabase_realtime_admin owns realtime.messages with RLS already on, and
-- `postgres` (which runs this file on `supabase start`) may create policies on
-- it but not ALTER it ("must be owner of table messages"). The e2e shim's table
-- (e2e/schema.sql) starts with RLS off, and its superuser turns it on here.
DO $$ BEGIN
  IF EXISTS (SELECT 1 FROM pg_class WHERE oid = to_regclass('realtime.messages') AND NOT relrowsecurity) THEN
    ALTER TABLE realtime.messages ENABLE ROW LEVEL SECURITY;
  END IF;
END $$;

-- Drop existing policies if they exist
DROP POLICY IF EXISTS "authenticated can receive game broadcasts" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can receive private messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can send private messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can receive game-user messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can receive chat broadcasts" ON "realtime"."messages";
DROP POLICY IF EXISTS "service role can send game broadcasts" ON "realtime"."messages";
DROP POLICY IF EXISTS "service role can send private messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "service role can send game-user messages" ON "realtime"."messages";

-- Policy for public game channels (topic: game-{game_id})
-- Anyone can read game broadcasts since it's public information
CREATE POLICY "authenticated can receive game broadcasts"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) LIKE 'game-%'
  AND realtime.messages.extension IN ('broadcast')
);


-- (No user-{email} channel policies: nothing uses that topic family, and its
-- receive policy matched on the email's local part. Dropped on hosted by
-- migrations/20260917130000_drop_user_realtime_policies.sql.)

-- Policy for private game-user channels (topic: gu-{game_id}-{user_id})
-- A user reads only their own seat's stream, for a game they are in. The topic is
-- rebuilt from the membership row and compared whole, never parsed: a user id is a
-- hyphenated UUID, so splitting the topic on '-' cannot recover it (see
-- migrations/20260917120000_realtime_channel_exact_topics.sql).
CREATE POLICY "authenticated can receive game-user messages"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) LIKE 'gu-%' AND
  EXISTS (
    SELECT 1
    FROM public.player_hands ph
    WHERE ph.player_id = (SELECT auth.uid())
      AND (SELECT realtime.topic()) = 'gu-' || ph.game_id || '-' || (SELECT auth.uid())::text
  ) AND
  realtime.messages.extension IN ('broadcast')
);

-- Policy for chat broadcasts (topic: chat:{game_id})
-- Users can receive chat broadcasts for games they're participating in, on the
-- exact topic the chat_messages_changes trigger broadcasts to.
CREATE POLICY "authenticated can receive chat broadcasts"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) LIKE 'chat:%' AND
  EXISTS (
    SELECT 1
    FROM public.player_hands ph
    WHERE ph.player_id = (SELECT auth.uid())
      AND (SELECT realtime.topic()) = 'chat:' || ph.game_id
  ) AND
  realtime.messages.extension IN ('broadcast')
);

-- ===============================
-- SERVICE ROLE POLICIES FOR SERVER-SIDE FUNCTIONS
-- Allow Supabase functions to send broadcasts
-- =============================================================================

-- Service role can send game broadcasts (for server-initiated game updates)
CREATE POLICY "service role can send game broadcasts"
ON "realtime"."messages" 
FOR INSERT
TO service_role
WITH CHECK (
  (SELECT realtime.topic()) LIKE 'game-%'
  AND realtime.messages.extension IN ('broadcast')
);

-- Service role can send game-user messages (for server-initiated personalized updates)
CREATE POLICY "service role can send game-user messages"
ON "realtime"."messages"
FOR INSERT
TO service_role
WITH CHECK (
  (SELECT realtime.topic()) LIKE 'gu-%'
  AND realtime.messages.extension IN ('broadcast')
);



-- =============================================================================
-- SEED DATA: Initial bots with different strategies
-- =============================================================================

INSERT INTO bots (nickname, strategy_key) VALUES
-- Handwritten strategy bots (rule-based)
('Handwritten 1', 'handwritten'),
('0x00C0FFEE', 'handwritten'),
('Handwritten 3', 'handwritten'),
('Handwritten 4', 'handwritten'),

-- Random strategy bots (chaotic)
('Random 1', 'random'),
('Random 2', 'random'),
('Random 3', 'random'),
('Random 4', 'random'),
('Random 5', 'random'),
('Random 6', 'random'),
('Random 7', 'random'),

-- Simple heuristic strategy bots (logical rule-based)
('Simple Heuristic 1', 'simple_heuristic'),
('Simple Heuristic 2', 'simple_heuristic'),
('Simple Heuristic 3', 'simple_heuristic'),

-- NOTE: champion, ultimate_champion, hacker, espresso, semtex and semtex_max
-- are intentionally NOT seeded. Those strategies are not compiled into / not
-- dispatched by the production bots.wasm (see wasm_choose_move in
-- c/wasm/wasm_bots_api.c), so a bot carrying one of those keys silently
-- falls back to `random` — a bot that plays nothing like its name and pollutes
-- the Elo leaderboard. Only strategy keys the wasm actually dispatches are
-- seeded: random, simple_heuristic, handwritten (→handwritten_prod),
-- firecracker, blackpowder, cordite, octogen.
--
-- The seeded set must equal the `seeded` column of the C bot roster
-- (c/src/bot_roster.c) — that table is the canonical roster
-- (docs/C_CORE_CONSOLIDATION.md F1).
--
-- The `_max` tiers were retired (migration 20260715120000_drop_max_bot_tiers):
-- octogen_max was a plain alias of octogen (identical knobs), and cordite_max's
-- CD_BUDGET=max is a FLAT world budget that only exceeds the player-count-aware
-- `prod` schedule at 2-4 players and is about HALF of it at 6-8 — so "Max" was
-- the weaker bot in the larger games. One cordite, on the prod budget.

-- Firecracker strategy bots — shipped ladder "Medium" rung (Durak Bot Ordnance
-- Chart). Public-info Monte Carlo: robusta's sampled-world MC with espresso as
-- the rollout policy. Honest (never reads real hidden hands).
('Firecracker 1', 'firecracker'),
('Firecracker 2', 'firecracker'),
('Firecracker 3', 'firecracker'),

-- Blackpowder strategy bots — shipped ladder "Hard" rung. The first
-- belief-constrained Monte Carlo (cordite's predecessor): card memory rebuilt
-- from the public log, void-constraint belief mixture, and an exact endgame
-- solver. Public info only.
('Blackpowder 1', 'blackpowder'),
('Blackpowder 2', 'blackpowder'),
('Blackpowder 3', 'blackpowder'),

-- Cordite strategy bots (belief-constrained Monte Carlo, no cheating —
-- beats every other bot at every player count 2-8; see c/CORDITE.md)
('Cordite 1', 'cordite'),
('Cordite 2', 'cordite'),
('Cordite 3', 'cordite'),

-- (semtex / semtex_max are not seeded — not dispatched by bots.wasm; see the
-- note above. Octogen is semtex's shipped successor and IS dispatched.)

-- Octogen (semtex + extended exact-solve window; provably never worse than
-- semtex, strictly better in deep heads-up endgames — see c/OCTOGEN.md)
('Octogen 1', 'octogen'),
('Octogen 2', 'octogen'),
('Octogen 3', 'octogen');

-- Bots carry the reserved '%' prefix so bot-vs-human is recoverable from the
-- name-only replay codec. Done as an UPDATE (rather than prefixing every literal
-- above) so the list stays readable; idempotent via the left() check. The live
-- DB gets this same rename via migrations/20260615120000_reserve_bot_username_prefix.sql.
UPDATE bots SET nickname = '%' || nickname WHERE left(nickname, 1) <> '%';


-- =============================================================================
-- SETUP COMPLETE!
-- Your database schema is now secure and ready for the game application.
-- Advisory locks are configured for game operation synchronization.
-- Bot system is initialized with sample strategies.
-- =============================================================================
