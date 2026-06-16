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
DROP TABLE IF EXISTS game_logs CASCADE;
DROP TABLE IF EXISTS chat_messages CASCADE;
DROP TABLE IF EXISTS bot_hands CASCADE;
DROP TABLE IF EXISTS player_hands CASCADE;
DROP TABLE IF EXISTS game_decks CASCADE;
DROP TABLE IF EXISTS games CASCADE;
DROP TABLE IF EXISTS user_elo_ratings CASCADE;
DROP TABLE IF EXISTS bots CASCADE;
DROP TABLE IF EXISTS auto_discard_locks CASCADE;

-- Drop custom types
DROP TYPE IF EXISTS log_type CASCADE;
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

CREATE TYPE log_type AS ENUM (
  'game_start',
  'attack',
  'cover',
  'pass',
  'pickup',
  'good',
  'discard',
  'defender_change',
  'player_out',
  'draw'
);

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

-- User ELO ratings table
CREATE TABLE user_elo_ratings (
  user_id UUID PRIMARY KEY REFERENCES auth.users(id) ON DELETE CASCADE,
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

-- Auto discard locks table - Simple table-based locking for auto-discard monitoring
CREATE TABLE auto_discard_locks (
  game_id TEXT PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE,
  lock_id TEXT NOT NULL, -- Random ID to verify lock ownership
  acquired_at TIMESTAMP DEFAULT NOW()
);

-- Game logs table - Log all game actions for bot memory and game history
-- This allows bots to track which cards have been played and infer information about opponent hands
CREATE TABLE game_logs (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  log_type log_type NOT NULL,
  player_id TEXT, -- Player who performed the action (null for system events like discard/defender_change)
  card_pairs JSONB NOT NULL DEFAULT '[]'::jsonb, -- Array of {primary: Card, target?: Card} - target only used for COVER
  defender_index INTEGER, -- For defender_change events, the new defender index
  created_at TIMESTAMP DEFAULT NOW()
);

-- Game snapshots - one row per finished session: the complete game compressed
-- by functions/_shared/replay/, stored as raw binary. `moves` is the rANS
-- move integer (decodes to the full game); `extras` is the optional names +
-- timing blob. The share code is derived: base32(moves) + '-' + base32(extras)
-- — the moves-only code is just the first part. Replaces the session's
-- game_logs rows, which are wiped after the snapshot is verified and stored.
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
CREATE INDEX idx_user_elo_ratings_user_id ON user_elo_ratings(user_id);
CREATE INDEX idx_user_elo_ratings_elo_rating ON user_elo_ratings(elo_rating);
CREATE INDEX idx_bots_strategy_key ON bots(strategy_key);
CREATE INDEX idx_bots_elo_rating ON bots(elo_rating);
CREATE INDEX idx_bot_hands_game_id ON bot_hands(game_id);
CREATE INDEX idx_bot_hands_bot_id ON bot_hands(bot_id);
CREATE INDEX idx_auto_discard_locks_game_id ON auto_discard_locks(game_id);
CREATE INDEX idx_auto_discard_locks_acquired_at ON auto_discard_locks(acquired_at);
CREATE INDEX idx_game_logs_game_id ON game_logs(game_id);
CREATE INDEX idx_game_logs_log_type ON game_logs(log_type);
CREATE INDEX idx_game_logs_player_id ON game_logs(player_id);
CREATE INDEX idx_game_logs_created_at ON game_logs(created_at);
CREATE INDEX idx_game_snapshots_game_id ON game_snapshots(game_id);
CREATE INDEX idx_game_snapshots_created_at ON game_snapshots(created_at);
CREATE INDEX idx_game_snapshots_player_ids ON game_snapshots USING GIN (player_ids);

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
ALTER TABLE auto_discard_locks ENABLE ROW LEVEL SECURITY;
ALTER TABLE game_logs ENABLE ROW LEVEL SECURITY;
ALTER TABLE game_snapshots ENABLE ROW LEVEL SECURITY;

-- =============================================================================
-- RLS POLICIES: Security-first approach
-- =============================================================================

-- Games: Anyone can view games (PUBLIC DATA ONLY - no sensitive info)
-- This allows users to join games or spectate without being in the game first
CREATE POLICY "Anyone can view games" ON games
  FOR SELECT USING (true);

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

-- Auto discard locks: ONLY service role can access (edge functions only)
CREATE POLICY "Only service role can access auto discard locks" ON auto_discard_locks
  FOR ALL USING ((select auth.role()) = 'service_role');

-- Game logs: ONLY service role can write, but can be read for analysis
-- Bots and advanced strategies can read these logs to make better decisions
CREATE POLICY "Service role can insert logs" ON game_logs
  FOR INSERT WITH CHECK ((select auth.role()) = 'service_role');

CREATE POLICY "Service role can read logs" ON game_logs
  FOR SELECT USING ((select auth.role()) = 'service_role');

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
  INSERT INTO public.user_elo_ratings (user_id, elo_rating, games_played)
  VALUES (NEW.id, 1000, 0)
  ON CONFLICT (user_id) DO NOTHING;
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

-- Trigger to create default ELO rating for new users
DROP TRIGGER IF EXISTS handle_new_user_elo_rating ON auth.users;
CREATE TRIGGER handle_new_user_elo_rating
  AFTER INSERT
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
  p_bot_hands        JSONB
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
    good_players = g.good_players, updated_at = now(), version = version + 1
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

  RETURN jsonb_build_object('status', 'ok', 'version', v_new_version);
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
('Cordite Max 3', 'cordite_max');

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
