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
DROP TABLE IF EXISTS player_views CASCADE;
DROP TABLE IF EXISTS chat_messages CASCADE;
DROP TABLE IF EXISTS bot_hands CASCADE;
DROP TABLE IF EXISTS player_hands CASCADE;
DROP TABLE IF EXISTS game_decks CASCADE;
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
  state TEXT, -- packed kernel state blob (hex): the volatile game state the server reconstructs from on load (wasm_state_serialize / engine.ts serializeGameState). Authoritative once a game is dealt; the hand/deck JSONB columns are a client-facing read-model dual-written alongside it. NULL for never-dealt (waiting) games.
  game_seed TEXT, -- SENSITIVE (server-only, like `state`): 64 hex chars = the 32-byte deal seed the deck was ChaCha-shuffled from. Regenerates the deal for audit/replay. NEVER granted to anon/authenticated (omitted from the column GRANT below); NULL for legacy/never-dealt games.
  logs_packed TEXT, -- packed session log stream (BARE hex, no \\x prefix — appended by plain concat): kernel log records + u48 timestamps, DRAW identities pre-masked. The sole session-log store (the game_logs table was dropped in migration 20260708120000); see wire/logwire.ts and migration 20260707150000.
  version BIGINT NOT NULL DEFAULT 0, -- optimistic-concurrency token (see commit_game RPC); replaces game_locks
  bot_lease_token UUID,              -- bot-loop lease holder token (replaces bot_locks)
  bot_lease_until TIMESTAMPTZ,       -- bot-loop lease expiry; auto-expiring, no finally-release needed
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW()
);

-- Game decks table - SENSITIVE: Only edge functions can access
CREATE TABLE game_decks (
  game_id TEXT PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE,
  deck JSONB NOT NULL DEFAULT '[]'::jsonb, -- Card[] - deck cards
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW()
);

-- Player hands table - SENSITIVE: Players can only see their own hands
-- This table also serves as the player-game relationship table
CREATE TABLE player_hands (
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  player_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  hand JSONB NOT NULL DEFAULT '[]'::jsonb, -- Card[] - player's cards
  awaiting_attack BOOLEAN NOT NULL DEFAULT false, -- Private status for attack confirmation
  joined_at TIMESTAMP DEFAULT NOW(),
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW(),
  PRIMARY KEY (game_id, player_id) -- One hand per player per game
);

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
CREATE TABLE bot_hands (
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  bot_id UUID NOT NULL REFERENCES bots(id) ON DELETE CASCADE,
  hand JSONB NOT NULL DEFAULT '[]'::jsonb, -- Card[] - bot's cards
  awaiting_attack BOOLEAN NOT NULL DEFAULT false, -- Private status for attack confirmation
  joined_at TIMESTAMP DEFAULT NOW(),
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW(),
  PRIMARY KEY (game_id, bot_id) -- One hand per bot per game
);

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
-- by functions/_shared/replay/, stored as raw binary. `moves` is the rANS
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

-- =============================================================================
-- INDEXES: Create indexes for better performance
-- =============================================================================

CREATE INDEX idx_games_status ON games(status);
CREATE INDEX idx_games_name ON games(name);
CREATE INDEX idx_game_decks_game_id ON game_decks(game_id);
CREATE INDEX idx_player_hands_game_id ON player_hands(game_id);
CREATE INDEX idx_player_hands_player_id ON player_hands(player_id);
CREATE INDEX idx_chat_messages_game_id ON chat_messages(game_id);
CREATE INDEX idx_chat_messages_user_id ON chat_messages(user_id);
CREATE INDEX idx_chat_messages_created_at ON chat_messages(created_at);
CREATE INDEX idx_games_updated_at ON games(updated_at);
-- bot-heartbeat SCAN (every 10s): status='playing' + updated_at window
CREATE INDEX idx_games_playing_updated_at ON games(updated_at) WHERE status = 'playing';
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
ALTER TABLE game_decks ENABLE ROW LEVEL SECURITY;
ALTER TABLE player_hands ENABLE ROW LEVEL SECURITY;
ALTER TABLE chat_messages ENABLE ROW LEVEL SECURITY;
ALTER TABLE user_elo_ratings ENABLE ROW LEVEL SECURITY;
ALTER TABLE bots ENABLE ROW LEVEL SECURITY;
ALTER TABLE bot_hands ENABLE ROW LEVEL SECURITY;
ALTER TABLE game_snapshots ENABLE ROW LEVEL SECURITY;
ALTER TABLE player_views ENABLE ROW LEVEL SECURITY;

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

CREATE POLICY "Authenticated users can create games" ON games
  FOR INSERT WITH CHECK (
    (select auth.role()) = 'authenticated'
  );

-- Game Decks: ONLY service role can access (edge functions only)
CREATE POLICY "Only service role can access game decks" ON game_decks
  FOR ALL USING ((select auth.role()) = 'service_role');

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

CREATE TRIGGER update_game_decks_updated_at 
  BEFORE UPDATE ON game_decks
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
-- CONCURRENCY RPCs (replace game_locks + bot_locks). See migration
-- 20260616030000_cas_concurrency.sql for the rationale. Kept identical here.
-- =============================================================================

-- Atomic, version-gated commit of the whole game state in one transaction.
CREATE OR REPLACE FUNCTION commit_game(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_game             JSONB,
  p_deck             JSONB,
  p_hands            JSONB,
  p_bot_hands        JSONB,
  p_state            TEXT    DEFAULT NULL,  -- packed kernel state blob (hex); NULL leaves the column unchanged (never-dealt games)
  p_logs_packed      TEXT    DEFAULT NULL,  -- this move's logwire records (bare hex), appended under the version fence
  p_logs_reset       BOOLEAN DEFAULT FALSE, -- session reset (GAME_START in the records): replace instead of append
  p_game_seed        TEXT    DEFAULT NULL,  -- deal seed (hex); set once at the deal, NULL on every other commit leaves it unchanged
  p_views            JSONB   DEFAULT NULL   -- per-participant masked view cache rows [{player_id,view,status}]; NULL leaves player_views untouched
) RETURNS JSONB
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_new_version BIGINT;
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
    updated_at = now(), version = version + 1
  WHERE id = p_game_id AND version = p_expected_version
  RETURNING version INTO v_new_version;

  IF NOT FOUND THEN
    RETURN jsonb_build_object('status', 'conflict');
  END IF;

  IF p_deck IS NOT NULL THEN
    INSERT INTO game_decks (game_id, deck) VALUES (p_game_id, p_deck)
    ON CONFLICT (game_id) DO UPDATE SET deck = EXCLUDED.deck, updated_at = now();
  END IF;

  IF p_hands IS NOT NULL AND jsonb_array_length(p_hands) > 0 THEN
    INSERT INTO player_hands (game_id, player_id, hand, awaiting_attack)
    SELECT p_game_id, (h->>'player_id')::uuid, h->'hand',
           COALESCE((h->>'awaiting_attack')::bool, false)
    FROM jsonb_array_elements(p_hands) AS h
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET hand = EXCLUDED.hand, awaiting_attack = EXCLUDED.awaiting_attack, updated_at = now();
  END IF;

  IF p_bot_hands IS NOT NULL AND jsonb_array_length(p_bot_hands) > 0 THEN
    INSERT INTO bot_hands (game_id, bot_id, hand, awaiting_attack)
    SELECT p_game_id, (b->>'bot_id')::uuid, b->'hand',
           COALESCE((b->>'awaiting_attack')::bool, false)
    FROM jsonb_array_elements(p_bot_hands) AS b
    ON CONFLICT (game_id, bot_id) DO UPDATE
      SET hand = EXCLUDED.hand, awaiting_attack = EXCLUDED.awaiting_attack, updated_at = now();
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

  RETURN jsonb_build_object('status', 'ok', 'version', v_new_version);
END;
$$;

-- create_game: the three create-game inserts (games → game_decks → player_hands)
-- in one transaction / one round-trip. See migration 20260618120000.
-- Callable DIRECTLY by the authenticated client (no create edge function). The
-- security boundary is auth.uid(): for a client call we DERIVE the creator's
-- identity from the verified JWT and IGNORE any client-supplied identity params,
-- so a client can't create a game as someone else. The explicit-param path
-- remains only for a service-role caller (no user JWT). EXECUTE is granted to
-- authenticated + service_role (never anon) below.
CREATE OR REPLACE FUNCTION create_game(
  p_game_id   TEXT,
  p_name      TEXT  DEFAULT NULL,   -- ignored for an authenticated caller (derived from the JWT)
  p_player_id UUID  DEFAULT NULL,   -- ignored for an authenticated caller (auth.uid())
  p_players   JSONB DEFAULT NULL,   -- ignored for an authenticated caller (built server-side)
  p_views     JSONB DEFAULT NULL    -- creator's masked view cache row(s); version 0
) RETURNS VOID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_uid      UUID := auth.uid();
  v_player   UUID;
  v_name     TEXT;
  v_players  JSONB;
  v_username TEXT;
BEGIN
  IF v_uid IS NOT NULL THEN
    -- Authenticated client-direct call: identity comes ONLY from the verified
    -- token (anti-spoof) — client p_player_id/p_name/p_players are ignored.
    v_player   := v_uid;
    v_username := current_setting('request.jwt.claims', true)::jsonb -> 'user_metadata' ->> 'username';
    v_name     := coalesce(v_username, 'Player') || '''s Game';
    v_players  := jsonb_build_array(jsonb_build_object(
                    'player_id', v_uid, 'name', v_username, 'status', 'idle', 'is_ai', false));
  ELSE
    -- Service-role caller (no user JWT): trust the explicit params it computed
    -- from a verified token. anon can't reach here (see the GRANT below).
    v_player  := p_player_id;
    v_name    := p_name;
    v_players := p_players;
  END IF;

  INSERT INTO games (id, name, players, status)
    VALUES (p_game_id, v_name, v_players, 'waiting');

  INSERT INTO game_decks (game_id, deck)
    VALUES (p_game_id, '[]'::jsonb);

  INSERT INTO player_hands (game_id, player_id, hand, awaiting_attack)
    VALUES (p_game_id, v_player, '[]'::jsonb, false);

  -- Seed the player_views dashboard cache for the creator in the same
  -- transaction, so the new lobby is immediately readable from the client's
  -- direct player_views SELECT (docs/PLAYER_VIEWS.md). version 0 = the initial
  -- games.version. Only the CALLER'S OWN row is honored — a create must not seed
  -- a player_views row for another user (which they'd then see in their list).
  IF p_views IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    SELECT p_game_id, v_player, v->>'view', 0, v->>'status', now()
    FROM jsonb_array_elements(p_views) AS v
    WHERE (v->>'player_id')::uuid = v_player
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;
END;
$$;

-- Lock down execution: authenticated (client-direct) + service_role only.
REVOKE ALL ON FUNCTION create_game(TEXT, TEXT, UUID, JSONB, JSONB) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION create_game(TEXT, TEXT, UUID, JSONB, JSONB) TO authenticated, service_role;

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

-- =============================================================================
-- REALTIME AUTHORIZATION POLICIES
-- Enable Supabase Realtime with proper security
-- =============================================================================

-- Enable RLS on the realtime messages table
ALTER TABLE IF EXISTS realtime.messages ENABLE ROW LEVEL SECURITY;

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


-- Policy for private user channels (topic: user-{email_prefix})
-- Users can read messages sent to them  
CREATE POLICY "authenticated can receive private messages"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) = CONCAT('user-', split_part(((select current_setting('request.jwt.claims', true))::jsonb ->> 'email'), '@', 1))
  AND realtime.messages.extension IN ('broadcast')
);

-- Anyone can send private messages to any user (for system notifications)
CREATE POLICY "authenticated can send private messages"
ON "realtime"."messages"
FOR INSERT
TO authenticated
WITH CHECK (
  (SELECT realtime.topic()) LIKE 'user-%'
  AND realtime.messages.extension IN ('broadcast')
);

-- Policy for private game-user channels (topic: gu-{game_id}-{user_id})
-- Users can only read messages from their own game-user channels
CREATE POLICY "authenticated can receive game-user messages"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) LIKE 'gu-%' AND
  -- Extract user_id from topic (gu-{game_id}-{user_id}) and verify it matches current user
  split_part((SELECT realtime.topic()), '-', 3) = (select auth.uid())::text AND
  -- Extract game_id and verify user is in that game
  EXISTS (
    SELECT 1
    FROM player_hands
    WHERE 
      player_id = (select auth.uid())
      AND game_id = split_part((SELECT realtime.topic()), '-', 2)
  ) AND
  realtime.messages.extension IN ('broadcast')
);

-- Policy for chat broadcasts (topic: chat-{game_id})
-- Users can receive chat broadcasts for games they're participating in
CREATE POLICY "authenticated can receive chat broadcasts"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) LIKE 'chat:%' AND
  -- Extract game_id from topic (chat:{game_id}) and verify user is in that game
  EXISTS (
    SELECT 1
    FROM player_hands
    WHERE 
      player_id = (select auth.uid())
      AND game_id = split_part((SELECT realtime.topic()), ':', 2)
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

-- Service role can send private messages (for server notifications)
CREATE POLICY "service role can send private messages"
ON "realtime"."messages"
FOR INSERT
TO service_role  
WITH CHECK (
  (SELECT realtime.topic()) LIKE 'user-%'
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

-- Ultimate champion strategy bots (advanced AI)
('Ultimate Champion 1', 'ultimate_champion'),
('Ultimate Champion 2', 'ultimate_champion'),
('Ultimate Champion 3', 'ultimate_champion'),

-- Champion strategy bots (tournament winners)
('Champion 1', 'champion'),
('Champion 2', 'champion'),
('Champion 3', 'champion'),
('Champion 4', 'champion'),

-- Hacker strategy bots (perfect information - UNFAIR ADVANTAGE)
('Hacker 1', 'hacker'),
('Hacker 2', 'hacker'),

-- Espresso strategy bots (perfect-info + lookahead, beats handwritten ~54%)
('Espresso 1', 'espresso'),
('Espresso 2', 'espresso'),
('Espresso 3', 'espresso'),

-- Cordite strategy bots (belief-constrained Monte Carlo, no cheating —
-- beats every other bot at every player count 2-8; see cnitro/CORDITE.md)
('Cordite 1', 'cordite'),
('Cordite 2', 'cordite'),
('Cordite 3', 'cordite'),

-- Cordite Max (same brain, larger sampled-world budget, <2s per decision)
('Cordite Max 1', 'cordite_max'),
('Cordite Max 2', 'cordite_max'),
('Cordite Max 3', 'cordite_max'),

-- Semtex (cordite's successor: exact leaf endgames in rollouts, extended
-- exact-solve window, per-seat MC-tells + opponent profiling — beats cordite
-- head-to-head and exploits weak opponents harder; see cnitro/SEMTEX.md)
('Semtex 1', 'semtex'),
('Semtex 2', 'semtex'),
('Semtex 3', 'semtex'),

-- Semtex Max (same brain, cordite_max world budget)
('Semtex Max 1', 'semtex_max'),
('Semtex Max 2', 'semtex_max'),
('Semtex Max 3', 'semtex_max'),

-- Octogen (semtex + extended exact-solve window; provably never worse than
-- semtex, strictly better in deep heads-up endgames — see cnitro/OCTOGEN.md)
('Octogen 1', 'octogen'),
('Octogen 2', 'octogen'),
('Octogen 3', 'octogen'),

-- Octogen Max (same brain, semtex_max world budget)
('Octogen Max 1', 'octogen_max'),
('Octogen Max 2', 'octogen_max'),
('Octogen Max 3', 'octogen_max');

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
